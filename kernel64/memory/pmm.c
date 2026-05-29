#include <kernel64/memory/paging.h>
#include <kernel64/memory/pmm.h>
#include <common/bootinfo.h>
#include <stddef.h>

#include <common/drivers/vga/vga.h>

/* Important conventions:
 *
 * Physical addresses are always unsigned integers specified by uintptr_t
 * and additionally have prefix p_ before name
 *
 * Virtual addresses should preferably be used and stored only as pointers
 * with no prefix, and when converting to integer, use uint64_t
 *
 * Physical addresses must use 0 as 'NULL' and virtual addresses must use NULL,
 * interconverting them should yield the other
 *
 * Physical addresses are what we reason with in the algorithm,
 * while virtual addresses are how we communicate with other subsystems, internally,
 * or for reads/writes. 
 *
 * Essentially virtual addressing here is just the physical address range
 * mirrored into a virtual range with a known base, and offsets are preserved.
 * Use pmm_v_ptr/pmm_p_ptr to convert between them
 *
 * Internally functions can expect either based on whichever is convenient */

struct pmm_pvt {
    struct memory_map_entry* memory_map;
    uint32_t memory_map_count;
    uintptr_t p_bottom;
    uintptr_t p_top;

    /* Virtual address base pointer where usable PMM owned RAM is mirrored 
     * with physical offsets */
    uint64_t mirror_base;

    struct pmm_page_meta* page_list_top;
    struct pmm_page_meta* page_list_bottom;

    struct pmm_zone_meta* zone_list_bottom;
    uint32_t zone_usable_count;
    /* Free list head pointer for every order */
    void* free_lists[19];

    /* Memory occupancy trackers */
    uint64_t page_count_free;
    uint64_t page_count_allocated;
    uint64_t page_count_reserved;
    uint64_t page_count_protected;
    uint64_t page_count_total;
};

struct pmm_pvt pmm_state;

/* Raw read from metadata array */
static inline struct pmm_page_meta* pmm_page_meta_read(struct pmm_page_meta* list_base, uint64_t page_index) {
    return &list_base[page_index];
}

/* Raw write to page metadata, no guards or checks */
static void pmm_page_meta_write(struct pmm_page_meta* list_base, uint64_t page_index, uint8_t state, uint8_t order) {
    struct pmm_page_meta* meta = pmm_page_meta_read(list_base, page_index);
    meta->order = order;
    meta->state = state;
}

/* Print metadata */
__attribute__((unused))
static void pmm_page_meta_print(struct pmm_page_meta* meta) {
    vga_print("State: ");
    switch (meta->state) {
        case PMM_PAGE_ALLOCATED:
            vga_print_color("ALLOCATED", VGA_COLOR_LIGHT_MAGENTA);
            break;
        case PMM_PAGE_FREE:
            vga_print_color("FREE", VGA_COLOR_LIGHT_MAGENTA);
            break;
        case PMM_PAGE_RESERVED:
            vga_print_color("RESERVED", VGA_COLOR_LIGHT_MAGENTA);
            break;
        default:
            vga_print_color("UNKNOWN", VGA_COLOR_LIGHT_RED);
            break;
    }
    vga_print(" | Order: ");
    vga_print_uint_color(meta->order, 10, -1, VGA_COLOR_LIGHT_MAGENTA);
    vga_print("\n");
}

/* Page meta/Zone meta binary lookups gives O(logN) complexity which is suitable
 * for zone metadata lookups for wildcard addresses. Zone metadata is initialized in
 * order of memory map zones, which gives us a sorted list to work with.
 * 
 * In general, full physical address lookups should only be done in case of wildcard addresses. 
 * Once that is done, for any derived address read/writes, the base can be used with offsets 
 * based on order values and lookup becomes cheap */

/* Given a physical page address, look up its zone if its usable
 * otherwise return NULL */
/* Binary search */
static struct pmm_zone_meta* pmm_zone_meta_binary_lookup(uintptr_t p_addr) {
    uint32_t start = 0;
    uint32_t end = pmm_state.memory_map_count-1;

    /* start overflow is handled by while condition
     * and treated at out of bounds */
    while (start <= end) {
        uint32_t i=start+(end-start)/2;
        struct memory_map_entry entry = pmm_state.memory_map[i];

        bool gte_start = p_addr >= entry.addr;
        bool lt_end = p_addr < entry.addr+entry.length;

        if (gte_start && lt_end) {
            /* Get corresponding zone metadata once memory map region
             * has been identified */
            struct pmm_zone_meta* base = pmm_state.zone_list_bottom;
            struct pmm_zone_meta* meta = &base[i];

            if (!meta->usable) 
                return NULL;

            return meta;
        } else if (!gte_start) {
            /* Trim mid and after mid */
            /* Underflow */
            if (i == 0)
                return NULL;
            end = i-1;
        } else if (!lt_end) {
            /* Trim mid and before mid */
            start = i+1;
        }
    }

    /* Out of bounds/Not found */
    return NULL;
}

static struct pmm_page_meta* pmm_page_meta_binary_lookup(uintptr_t p_addr) {
    struct pmm_zone_meta* zone_meta = pmm_zone_meta_binary_lookup(p_addr);
    /* Check for zone validity */
    if (zone_meta == NULL) 
        return NULL;

    /* Within zone, find page index */
    uint64_t page_index = (p_addr-zone_meta->zone_p_base)>>12;

    /* Lookup page metadata and return it */
    struct pmm_page_meta* page_meta = pmm_page_meta_read(zone_meta->page_list_base, page_index);
    return page_meta;
}

static int pmm_page_meta_binary_lookup_write(uintptr_t p_addr, uint8_t state, uint8_t order) {
    /* Lookup page metadata and write to it */
    struct pmm_page_meta* page_meta = pmm_page_meta_binary_lookup(p_addr);
    if (page_meta == NULL)
        return 1;
    page_meta->order = order;
    page_meta->state = state;

    return 0;
}

static void pmm_block_freelist_insert(void* block, uint8_t order) {
    void* next = pmm_state.free_lists[order];
    *(uint64_t*)block = (uint64_t)next;
    pmm_state.free_lists[order] = block;
}

static inline void* pmm_block_freelist_read_next(void* block) {
    return (void*)(*(uint64_t*)block);
}

/* Assumes atleast 1 element */
static void* pmm_block_freelist_pop(uint8_t order) {
    void* head = pmm_state.free_lists[order];
    void* next = pmm_block_freelist_read_next(head);
    pmm_state.free_lists[order] = next;

    return head;
}

/* Removes an element from freelist */
static void pmm_block_freelist_remove(void* block, uint8_t order) {
    void* prev = NULL;
    void* next = pmm_state.free_lists[order];

    if (next == block) {
        pmm_block_freelist_pop(order);
        return;
    }

    prev = next;
    next = pmm_block_freelist_read_next(next);

    while (next != NULL) {
        if (next == block) {
            *(uint64_t*)prev = (uint64_t)pmm_block_freelist_read_next(next);
            return;
        }
        prev = next;
        next = pmm_block_freelist_read_next(next);
    }
}

/* split and split_recursive are only for free blocks in freelist that have been popped off
 * block_meta is page metadata address of block, which acts as a base for derived
 * relative addresses */
static void pmm_block_split(void* block, uint8_t block_order, struct pmm_page_meta* block_meta) {
    /* b0 address is same as block */
    void* b1 = (void*)((uint64_t)block+(PAGE_4K<<(block_order-1)));

    /* Only insert b1 in free list, b0 left for the caller
     * to decide what to do with, to optimize further splits */
    pmm_block_freelist_insert(b1, block_order-1);
    /* Update metadata for b0 and b1 */
    pmm_page_meta_write(block_meta, 0, PMM_PAGE_FREE, block_order-1);
    pmm_page_meta_write(block_meta, 1<<(block_order-1), PMM_PAGE_FREE, block_order-1);
}

/* Recursively split a free block of higher order till we obtain a block of desired lower order
 * The blocks are split recursively on the left, when we reach the desired order we just stop
 * The address given for the main block will match one of the final lower order blocks.
 * 
 * Every other split block is inserted into the freelist except the leftmost block.
 * Metadata initialization is done for all blocks
 *
 * 'block_meta' is the page metadata address for initial block given
 * */
static void pmm_block_split_recursive(void* block, uint8_t block_order, struct pmm_page_meta* block_meta, uint8_t target_order) {
    while (block_order > target_order) {
        pmm_block_split(block, block_order, block_meta);
        block_order--;
    }
}

/* Free block by merging recursively as long as buddy is free */
static void pmm_block_merge_free_recursive(uintptr_t p_block, uint8_t block_order, struct pmm_page_meta* block_meta) {
    if (block_order < block_meta->ancestor_order) {
        /* Buddy lookup */
        uintptr_t p_buddy = p_block^(PAGE_4K<<block_order);
        int64_t offset;

        if (p_block > p_buddy) {
            offset = -PAGE_COUNT_4K_IN_LEN(p_block-p_buddy);
        } else {
            offset = PAGE_COUNT_4K_IN_LEN(p_buddy-p_block);
        }

        struct pmm_page_meta* buddy_meta = &block_meta[offset];

        if (buddy_meta->state == PMM_PAGE_FREE) {
            /* If buddy is free, then merge
             * Metadata isnt updated, the buddy is just removed from freelist */
            pmm_block_freelist_remove(pmm_v_ptr(p_buddy), block_order);

            /* When we recurse for merge, the address and metadata address for
             * the combined block is going to be the leftmost block of the
             * two buddies */
            uintptr_t p_left;
            struct pmm_page_meta* left_meta;

            if (p_block > p_buddy) {
                p_left = p_buddy;
                left_meta = buddy_meta;
            } else {
                p_left = p_block;
                left_meta = block_meta;
            }

            /* Check for merges upwards recursively till max order */
            return pmm_block_merge_free_recursive(p_left, block_order+1, left_meta);
        } else {
            /* Otherwise conclude and add block to freelist */
            pmm_page_meta_write(block_meta, 0, PMM_PAGE_FREE, block_order);
            pmm_block_freelist_insert(pmm_v_ptr(p_block), block_order);
        }
    } else {
        /* Max order reached, just free it and return */
        pmm_page_meta_write(block_meta, 0, PMM_PAGE_FREE, block_order);
        pmm_block_freelist_insert(pmm_v_ptr(p_block), block_order);
    }
}

/* Marks ancestor_order property in page_metadata for every page in block
 * to the given value. ancestor_order is a constant property of the page address
 * and will not change after initialization. 
 *
 * It is used to store max top level block order so it can return to initial 
 * top level block when completely merged */
static void pmm_block_set_ancestor_order(struct pmm_page_meta* block_meta, uint8_t order) {
    for (uint64_t page_i=0; page_i<(uint64_t)(PAGE_4K<<order)>>12; page_i++) {
        struct pmm_page_meta* meta = &block_meta[page_i];
        meta->ancestor_order = order;
    }
}

__attribute__((unused))
void pmm_print_memory() {
    vga_print_color("Memory: ", VGA_COLOR_LIGHT_BLUE);
    vga_print_color("Allocated: ", VGA_COLOR_LIGHT_MAGENTA);
    vga_print_uint_color(pmm_state.page_count_allocated<<12, 10, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print(" Bytes | ");
    vga_print_color("Free: ", VGA_COLOR_LIGHT_MAGENTA);
    vga_print_uint_color(pmm_state.page_count_free<<12, 10, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print(" Bytes | ");
    vga_print_color("Reserved: ", VGA_COLOR_LIGHT_MAGENTA);
    vga_print_uint_color(pmm_state.page_count_reserved<<12, 10, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print(" Bytes | ");
    vga_print_color("Protected: ", VGA_COLOR_LIGHT_MAGENTA);
    vga_print_uint_color(pmm_state.page_count_protected<<12, 10, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print(" Bytes\n");
}

__attribute__((unused))
void pmm_print_freelists() {
    vga_print_color("Freelists: \n", VGA_COLOR_LIGHT_BLUE);
    for (int i=0; i<19; i++) {
        void* head = pmm_state.free_lists[i];
        if (head != NULL) {
            vga_print("OD "); vga_print_uint_color(i, 10, -1, VGA_COLOR_LIGHT_GREEN);
            vga_print(": ");
            while (head != NULL) {

                vga_print_uint_color((uint64_t)head, 16, 12, VGA_COLOR_LIGHT_BROWN);
                head = pmm_block_freelist_read_next(head);

                if (head == NULL) {
                    vga_print("\n");
                    break;
                } else
                    vga_print(" -> ");
            }
        }
    }
}

static int pmm_allocate_lists(uintptr_t p_zone_list_base, uint32_t zone_count, uint64_t list_size) {
    /* Read memory map and allocate page list and zone list starting at
     * the base address, size has been precalculated and it is ensured that
     * the memory we are writing to is free and usable */
   
    /* In order to access physical memory, we have to page it into an accessible
     * virtual range, as we are mapped and operating in virtual memory
     * PMM has to have set a mirror map at least for the zone we are allocating this
     * page list in 
     *
     * Any pointers we use for internal use are always virtual addresses 
     * we can directly access and write. We can convert back and forth between
     * virtual and physical because of the mirroring scheme */


    uintptr_t p_page_list_base = p_zone_list_base+sizeof(struct pmm_zone_meta)*pmm_state.memory_map_count;
    pmm_state.zone_list_bottom = pmm_v_ptr(p_zone_list_base);
    pmm_state.page_list_bottom = pmm_v_ptr(p_page_list_base);
    pmm_state.page_list_top = pmm_v_ptr(p_zone_list_base+list_size);
    pmm_state.zone_usable_count = zone_count; 
    
    for (uint32_t i=0; i<pmm_state.memory_map_count; i++) {
        struct memory_map_entry entry = pmm_state.memory_map[i];
        bool usable = true;

        if (entry.type != MEMORY_MAP_ENTRY_USABLE &&\
            entry.type != MEMORY_MAP_ENTRY_USABLE_ACPI)
            usable = false;

        /* A zone is any usable RAM region
         * Write zone list entry */
        struct pmm_zone_meta* zone_meta = pmm_v_ptr(p_zone_list_base); 
        uintptr_t zone_base_addr;
        uint64_t zone_length;

        if (!CHECK_PAGE_4K_ALIGN(entry.length))
            zone_length = PAGE_4K_ALIGN_DOWN(entry.length);
        else
            zone_length = entry.length;

        if (!CHECK_PAGE_4K_ALIGN(entry.addr))
            zone_base_addr = PAGE_4K_ALIGN_DOWN(entry.addr);
        else
            zone_base_addr = entry.addr;

        zone_meta->usable = usable;
        zone_meta->zone_p_base = zone_base_addr;

        if (!usable) {
            zone_meta->page_list_base = NULL;
            zone_meta->page_list_length = 0;
            zone_meta->page_count = 0;
            p_zone_list_base += sizeof(struct pmm_zone_meta);
            continue;
        }

        zone_meta->page_count = PAGE_COUNT_4K_CONTAINING_LEN(zone_length);
        zone_meta->page_list_base = pmm_v_ptr(p_page_list_base);
        zone_meta->page_list_length = zone_meta->page_count*sizeof(struct pmm_page_meta);

        /* For next zone, if any */
        p_zone_list_base += sizeof(struct pmm_zone_meta);
        p_page_list_base += zone_meta->page_list_length;

        /* Write page entry list at corresponding offset
         * Initialize to free and order 0 for now */
        pmm_state.page_count_total += zone_meta->page_count;
        for (uint64_t page_meta_i=0; page_meta_i<zone_meta->page_count; page_meta_i++) {
            pmm_page_meta_write(zone_meta->page_list_base, page_meta_i, PMM_PAGE_FREE, 0);
        }
    }
 
    /* Zone and page lists have been written and initialized,
     * we can now mark the pages occupied by the lists themselves as RESERVED */ 
    struct pmm_zone_meta* reserved_zone_meta = pmm_zone_meta_binary_lookup(\
            pmm_p_ptr(pmm_state.zone_list_bottom));
    uint64_t base_page_index = PAGE_COUNT_4K_CONTAINING_LEN( \
            pmm_p_ptr(pmm_state.zone_list_bottom)-reserved_zone_meta->zone_p_base);

    pmm_state.page_count_reserved = PAGE_COUNT_4K_CONTAINING_LEN(list_size);
    for (uint64_t i=0; i<pmm_state.page_count_reserved; i++) {
        struct pmm_page_meta* meta = &reserved_zone_meta->page_list_base[base_page_index+i];
        meta->order = 0;
        meta->state = PMM_PAGE_RESERVED;
        meta->ancestor_order = 0;
    }

    return 0;
}

/* Calculate page list + zone list size */
static uint64_t pmm_get_list_size(uint32_t* _zone_count) {
    uint64_t list_size = 0;
    uint32_t zone_count = 0;
    for (uint32_t i=0; i<pmm_state.memory_map_count; i++) {
        struct memory_map_entry entry = pmm_state.memory_map[i];
        if (entry.type != MEMORY_MAP_ENTRY_USABLE &&\
            entry.type != MEMORY_MAP_ENTRY_USABLE_ACPI)
            continue;

        uintptr_t p_addr = entry.addr;
        uint64_t length = entry.length;

        if (!CHECK_PAGE_4K_ALIGN(p_addr))
            p_addr = PAGE_4K_ALIGN(p_addr);
        if (!CHECK_PAGE_4K_ALIGN(length))
            length = PAGE_4K_ALIGN_DOWN(length);

        uint64_t max_local_index = PAGE_COUNT_4K_CONTAINING_LEN(length)-1;
        uint64_t local_list_size = (max_local_index+1)*sizeof(struct pmm_page_meta);
        uint64_t local_size = sizeof(struct pmm_zone_meta) + local_list_size;

        list_size += local_size;
        zone_count++;
    }

    *_zone_count = zone_count;

    /* Align list size to page boundary */
    if (!CHECK_PAGE_4K_ALIGN(list_size))
        list_size = PAGE_4K_ALIGN(list_size);
    return list_size;
}

/* Load and initialize page list after reading usable zones */
static int pmm_initialize_zones() {
    uint32_t zone_count = 0;
    uint64_t list_size = pmm_get_list_size(&zone_count);

    bool list_allocated = false;

    for (uint32_t i=0; i<pmm_state.memory_map_count; i++) {
        struct memory_map_entry entry = pmm_state.memory_map[i];
        if (entry.addr >= pmm_state.p_top) break;
        if (entry.type != MEMORY_MAP_ENTRY_USABLE &&\
            entry.type != MEMORY_MAP_ENTRY_USABLE_ACPI)
            continue;

        uintptr_t p_addr = entry.addr;
        uint64_t length = entry.length;

        /* Make sure page alignment is there */
        if (!CHECK_PAGE_4K_ALIGN(p_addr)) {
            uint64_t end = p_addr+length;
            p_addr = PAGE_4K_ALIGN(p_addr);
            length = end-p_addr;
        }
        /* There may be some wastage near the end of the mapping
         * if length is not page aligned, but will always be lesser than 4KB */
        if (!CHECK_PAGE_4K_ALIGN(length))
            length = PAGE_4K_ALIGN_DOWN(length);

        if (p_addr + length - 1 < pmm_state.p_bottom)
            continue;

        if (p_addr < pmm_state.p_bottom) {
            length = p_addr+length-pmm_state.p_bottom;
            p_addr = pmm_state.p_bottom;
        }

        if (p_addr + length - 1 >= pmm_state.p_top) 
            length -= p_addr+length - pmm_state.p_top;

        /* We have accounted for bottom/top trimming, and this is the right
         * time to set virtual memory mirror for usable physical zone */
        int s = paging_set_map_best_fit(pmm_state.mirror_base+p_addr, p_addr, length, PAGE_1G, true);
        if (s) {
            return 11;
        }

        /* Handle the case of allocating page list inside
         * this zone, as early as possible, if it is large enough
         * to contain it. We skip over it for zone init */
        if (!list_allocated && length >= list_size) {
            int s = pmm_allocate_lists(p_addr, zone_count, list_size);

            /* Error occured during allocation */
            if (s) 
                return s;
            p_addr = pmm_p_ptr(pmm_state.page_list_top);
            length -= list_size;
            list_allocated = true;
        }
                
        /* Length should end up at 0, as we have page aligned everything beforehand */
        while (length > 0) {
            /* Find highest order address alignment,
             * initialize block with highest possible order block given the size of region
             *
             * Order 0 is minimum requirement, which is ensured by previous page alignment */

            uint8_t highest_fit_od=18;
            for (int i=1; i<=18; i++) {
                if (!CHECK_ORDER_ALIGN(p_addr, i)) {
                    highest_fit_od = i-1;
                    break;
                }
            }

            uint8_t size_od = (64-__builtin_clzll(length))-(1+12);
            /* Cap at order 18 */
            if (size_od > 18) size_od = 18;

            /* Choose suitable order based on alignment and size constraints (min) */
            uint8_t order = highest_fit_od;
            if (order > size_od) order = size_od;

            void* addr = pmm_v_ptr(p_addr);
 
            pmm_block_freelist_insert(addr, order);
            pmm_state.page_count_free += (PAGE_4K<<order)>>12;

            uint64_t block_length = PAGE_4K<<order;

            length -= block_length;
            p_addr += block_length; 
        }
    }

    /* Suitable region couldnt be found to allocate lists */
    if (!list_allocated)
        return 2;

    /* Free, Reserved and Total pages have been calculated, find Protected pages (unstaged) */
    pmm_state.page_count_protected = pmm_state.page_count_total-(pmm_state.page_count_free+pmm_state.page_count_reserved);

    /* Traverse every free top level block in every freelist to mark their metadata
     * with their order.
     * We cannot do this while initializing because allocation of metadata is not
     * certain while filling freelist for all zones */
    for (int i=0; i<19; i++) {
        void* next = pmm_state.free_lists[i];

        while (next != NULL) {
            struct pmm_page_meta* meta = pmm_page_meta_binary_lookup(pmm_p_ptr(next));
            pmm_page_meta_write(meta, 0, PMM_PAGE_FREE, i);

            /* For every page in top level block, set ancestor order
             * of every page metadata entry to the top level order.
             * This ensures that any combination of split and merge
             * will eventually return to this top level block
             * when maximally free */
            pmm_block_set_ancestor_order(meta, i);
            next = pmm_block_freelist_read_next(next);
        }
    }

    return 0;
}

/* Get memory map in format specified in bootinfo.h,
 * get base physical address below which we dont touch memory */
int pmm_initialize(struct memory_map_entry* entries, uint32_t entry_count, uintptr_t bottom, uintptr_t top, uint64_t mirror_base) {
    pmm_state.memory_map = entries;
    pmm_state.memory_map_count = entry_count;
    pmm_state.p_bottom = bottom;
    pmm_state.p_top = top;
    pmm_state.mirror_base = mirror_base;

    pmm_state.page_count_allocated = 0;
    pmm_state.page_count_free = 0;
    pmm_state.page_count_reserved = 0;
    pmm_state.page_count_protected = 0;
    pmm_state.page_count_total = 0;

    if (!CHECK_PAGE_4K_ALIGN(pmm_state.p_bottom))
        pmm_state.p_bottom = PAGE_4K_ALIGN(pmm_state.p_bottom);
    if (!CHECK_PAGE_4K_ALIGN(pmm_state.p_top))
        pmm_state.p_top = PAGE_4K_ALIGN_DOWN(pmm_state.p_top);

    for (int i=0; i<19; i++) {
        pmm_state.free_lists[i] = NULL;
    }

    int s = pmm_initialize_zones();
    if (s)
        return s;
   
    vga_print("[");vga_print_color("Physical Memory Manager",VGA_COLOR_LIGHT_GREEN);vga_print("] ");
    vga_print("Initialized\n");
    vga_print("    Bottom: ");
    vga_print_uint_color(pmm_state.p_bottom, 16, -1, VGA_COLOR_LIGHT_MAGENTA);
    vga_print(" | Top: ");
    vga_print_uint_color(pmm_state.p_top, 16, -1, VGA_COLOR_LIGHT_MAGENTA);
    vga_print("\n");
    //pmm_print_memory();
    //pmm_print_freelists();
    return 0;
}

/* Allocate a block of order 0-18 from physical memory, return NULL if not possible */ 
void* pmm_allocate_block(uint8_t order) {
    uint64_t page_count = PAGE_COUNT_4K_IN_LEN(PAGE_4K<<order);
    void* block = NULL;

    /* Check freelist of order for available block */
    if (pmm_state.free_lists[order] != NULL) {
        block = pmm_block_freelist_pop(order);
        pmm_page_meta_binary_lookup_write(pmm_p_ptr(block), PMM_PAGE_ALLOCATED, order);
        pmm_state.page_count_allocated += page_count;
        pmm_state.page_count_free -= page_count;

        goto allocated;
    }

    /* No blocks are available of the requested size in the freelist,
     * we now try to split higher orders if possible */
    uint8_t higher_order;

    for (int i=order; i<19; i++) {
        void* b = pmm_state.free_lists[i];
        if (b != NULL) {
            /* If block is found, it is popped off the freelist */
            block = pmm_block_freelist_pop(i);
            higher_order = i;
            break;
        }
    }

    /* Not enough free memory to fullfill request */
    if (block == NULL)
        return NULL;

    struct pmm_page_meta* block_meta = pmm_page_meta_binary_lookup(pmm_p_ptr(block));
    /* Closest higher order block has been found and popped
     * split recursively till we reach desired order.
     * 'block' will contain final block */ 
    pmm_block_split_recursive(block, higher_order, block_meta, order);
    /* Mark block as allocated */
    pmm_page_meta_write(block_meta, 0, PMM_PAGE_ALLOCATED, order);
    
    pmm_state.page_count_allocated += page_count;
    pmm_state.page_count_free -= page_count;

allocated:
    /*
    vga_print("\nAllocated ");
    vga_print_uint_color(page_count<<12, 10, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print(" Bytes, Address: ");
    vga_print_uint_color((uint64_t)block, 16, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print("\n");

    pmm_print_memory();
    */
    return block;
}

void pmm_free_block(void* block) {
    uintptr_t p_block = pmm_p_ptr(block);
    struct pmm_page_meta* block_meta = pmm_page_meta_binary_lookup(p_block);

    /* False free protection for protected and reserved regions,
     * also for already free blocks. However this doesnt protect against everything.
     * Calling free on allocated block part of bigger allocated block is essentially UND */
    if (block_meta == NULL) return;
    if (block_meta->state != PMM_PAGE_ALLOCATED) return;

    uint8_t order = block_meta->order;
    uint64_t page_count = (PAGE_4K<<order)>>12;

    /* Free and merge recursively upward */
    pmm_block_merge_free_recursive(p_block, order, block_meta);

    pmm_state.page_count_free += page_count;
    pmm_state.page_count_allocated -= page_count;

    /*
    vga_print("\nFreed ");
    vga_print_uint_color(page_count<<12, 10, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print(" Bytes, Address: ");
    vga_print_uint_color((uint64_t)block, 16, -1, VGA_COLOR_LIGHT_GREEN);
    vga_print("\n");
    
    pmm_print_memory();
    */
}

uintptr_t pmm_p_ptr(void* v_ptr) {
    if (v_ptr == NULL) return 0;
    return (~pmm_state.mirror_base)&(uint64_t)v_ptr;
}

void* pmm_v_ptr(uintptr_t p_ptr) {
    if (p_ptr == 0) return NULL;
    return (void*)(pmm_state.mirror_base|(uint64_t)p_ptr);
}

void* pmm_allocate_page() {
    return pmm_allocate_block(0);
}

void pmm_free_page(void* page) {
    return pmm_free_block(page);
}
