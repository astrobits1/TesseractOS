#include <kernel64/memory/paging.h>
#include <common/drivers/vga/vga.h>
#include <libk/hashtb.h>

struct paging_pvt {
    struct paging_ops ops;
    bool initialized_refcount_map;
    struct hashtb refcount_map;
    volatile void* pml4;
};

struct paging_pvt paging_state = {
    .ops = {
        .allocate_page = NULL,
        .free_page = NULL,
        .p_ptr = NULL,
        .v_ptr = NULL,

        .cleanup_enabled = false,
        .allocate_block = NULL,
        .free_block = NULL
    },

    .initialized_refcount_map = false,
    .pml4 = NULL
};

/* Refcount functions that deal with counting and tracking of maps
 * allocated by the paging API in the hashtable. This has to be explicitly enabled
 * in the ops when initializing */

/* Create/Destroy refcount entry for map */
static inline void paging_refcount_create(uintptr_t p_map) { 
    hashtb_insert(&paging_state.refcount_map, p_map>>4, 0);
}

static inline void paging_refcount_destroy(uintptr_t p_map) {
    hashtb_remove(&paging_state.refcount_map, p_map>>4);
}

/* Increment/Decrement refcount entry for map */
static void paging_refcount_increment(uintptr_t p_map) {
    uint64_t refs;
    int s = hashtb_read(&paging_state.refcount_map, p_map>>4, &refs);
    if (s)
        return;

    hashtb_insert(&paging_state.refcount_map, p_map>>4, refs+1);
}

static void paging_refcount_decrement(uintptr_t p_map) {
    uint64_t refs;
    int s = hashtb_read(&paging_state.refcount_map, p_map>>4, &refs);
    if (s)
        return;

    hashtb_insert(&paging_state.refcount_map, p_map>>4, refs-1);
}

/* Check if a map is being referenced by any direct sub maps */
static bool paging_refcount_check_map_empty(uintptr_t p_map) {
    uint64_t refs;
    int s = hashtb_read(&paging_state.refcount_map, p_map>>4, &refs);
    if (s)
        return false;

    /* No linkage exists */
    if (refs == 0)
        return true;
    else
        return false;
}

/* Initialize map to unmapped by default, (P bit 0) */
static void paging_initialize_map(volatile uint8_t* map) {
    for (uint32_t i=0; i<MAP_SIZE; i++) {
        map[i] = 0;
    }
}

/* Generalised function used to write address to pt, pd, pdpt, pml4
 *
 * pd and pdpt support pagesize bit which lets you define a contigious chunk of 2MB or 1GB 
 * in physical memory as a page. You write the physical memory address using SET_PAGESIZE and
 * the appropriate mask for them, otherwise pass NOSET_PAGESIZE, and NO_PAGE_MASK
 *
 * Flag configuration is minimal and default for now TODO */
static void paging_write_map_entry(volatile uint8_t* map, uint16_t index, uintptr_t addr, uint8_t pagesize, uint64_t pagesize_mask) {
    uint64_t a = addr&(((uint64_t)1<<M)-1);

    uint32_t flag_data = 0x003;
    if (pagesize) {
        /* Interpret physical address as single 2MB/1GB page (only for PD/PDTE) */
        flag_data |= 1 << 7;
        a &= pagesize_mask;
    } else a &= ~0xFFF;

    a |= flag_data;
 
    uint64_t* ptr = (uint64_t*)&map[index*8];
    *ptr = a;
}

/* Read address in entry at index in map, and get pagesize info */
static uintptr_t paging_read_map_entry(volatile uint8_t* map, uint16_t index, uint8_t* pagesize) { 
    uint64_t* ptr = (uint64_t*)&map[index*8];
    uint64_t a = ptr[0];

    *pagesize = a >> 7 & 1;
    /* Remove flag bits */
    a &= ~0xFFF;

    /* Address is M bits long */
    a &= ((uint64_t)1<<M)-1;
    return (uintptr_t)a;
}

/* Clear entry by setting present bit (and overall entry) to 0 */
static void paging_clear_map_entry(volatile uint8_t* map, uint16_t index) {
    uint64_t* ptr = (uint64_t*)&map[index*8];
    *ptr = 0;
}

/* Check if entry is present, by returning the present bit */
static bool paging_check_map_entry_present(volatile uint8_t* map, uint16_t index) {
    return (bool)(map[index*8] & 1);
}

static void paging_free_map(volatile void* map) {
    paging_state.ops.free_page((void*)map);
    
    /* TODO: TLB Invalidation */
}

/* Free map recursively, recursion is stopped when:
 * 1. We have gone 'depth' steps deep,
 * 2. or, when we have encountered an entry with PS=1 */
static void paging_free_map_recursive(volatile uint8_t* map, int depth) {
    if (depth > 0) {
        for (uint16_t i=0; i<MAP_ENTRY_COUNT; i++) {
            if (!paging_check_map_entry_present(map, i)) continue;

            uint8_t PS;
            volatile void* addr = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(map, i, &PS));
            if (PS == 1) continue;

            /* We have a nested map(s) which need to be freed */
            paging_free_map_recursive(addr, depth-1); 
        }
    }

    /* If refcount system is enabled, destroy its entry in the hashmap
     * at free */
    if (paging_state.initialized_refcount_map) {
        paging_refcount_destroy(paging_state.ops.p_ptr((void*)map));
    }
    paging_free_map(map);
}

/* Create a new map and use refcount system if enabled */
static volatile void* paging_create_map_entry(volatile void* map, bool inc_refcount) {
    volatile void* submap = paging_state.ops.allocate_page();    
    if (submap == NULL) 
        return NULL;
    
    paging_initialize_map(submap);
    
    if (paging_state.initialized_refcount_map) {
        paging_refcount_create(paging_state.ops.p_ptr((void*)submap)); 

        if (inc_refcount)
            paging_refcount_increment(paging_state.ops.p_ptr((void*)map));
    }

    return submap;
}

/* Deletes a map entry using the refcount system if cleanup is enabled
 * and hashmap exists, else leaks it
 * Return value: freed map or not */
static bool paging_delete_and_free_map_entry(volatile void* map, uint16_t entry) {
    paging_clear_map_entry(map, entry);

    if (paging_state.initialized_refcount_map) {
        uintptr_t p_map = paging_state.ops.p_ptr((void*)map);
        paging_refcount_decrement(p_map); 

        if (paging_refcount_check_map_empty(p_map)) {
            paging_free_map(map);
            paging_refcount_destroy(p_map);
            return true;
        } else
            return false;
    } else
        return false;
}

/* Hashmap allocate and free wrapper functions for power of 2 blocks */
static inline void* hashmap_buffer_allocate_pow2(uint64_t size) {
    return paging_state.ops.allocate_block(63-__builtin_clzll(PAGE_COUNT_4K_IN_LEN(size)));
}

static inline void hashmap_buffer_free(void* ptr) {
    return paging_state.ops.free_block(ptr);
}

/* Initialize allocator operations, this is generalized to handle anything from basic
 * virtual memory at boot to full kernel management 
 *
 * Cleanup can only be enabled if block allocate and free functions are
 * supplied. These are required for hashmap, and without them we cannot free many
 * pages we allocate and leak intentionally. This option exists to handle bootstrap
 * situation. */
int paging_initialize_allocator(struct paging_ops ops) {
    paging_state.ops = ops;

    /* If cleanup is enabled, initialize a new hashmap */
    if (ops.cleanup_enabled) {
        struct hashtb_ops h_ops;
        h_ops.hashing_algorithm = hash_multiplicative;
        h_ops.buffer_allocate_pow2 = hashmap_buffer_allocate_pow2;
        h_ops.buffer_free = hashmap_buffer_free;

        /* Initial block capacity is 1 4K page */
        if (hashtb_initialize(&paging_state.refcount_map, PAGE_4K, h_ops))
            return 1;
        paging_state.initialized_refcount_map = true;
    } else
        paging_state.initialized_refcount_map = false;

    return 0;
}

void paging_uninitialize_allocator() {
    /* Free hashmap if it was allocated */
    if (paging_state.ops.cleanup_enabled) {
        hashtb_free(&paging_state.refcount_map);
    }
}

volatile void* paging_new_pml4() {
    volatile void* ptr = paging_state.ops.allocate_page();
    if (ptr == NULL) return NULL;

    paging_initialize_map(ptr);

    return ptr;
}

void paging_free_pml4(volatile void* pml4) {
    paging_state.ops.free_page((void*)pml4);
}

/* Set top level pml4 */
void paging_set_pml4(volatile void* pml4) {
    paging_state.pml4 = pml4;
}

void paging_reload_pml4(void(*reload)(volatile void* pml4)) {
    if (paging_state.pml4 == NULL) return;

    reload(paging_state.pml4);
}

static inline uint64_t map_indices_to_v_addr(uint16_t pte, uint16_t pde, uint16_t pdpte, uint16_t pml4e) {
    /* Translation from map indices to virtual address, with bit 63-48 extension */
    uint64_t v_addr = ((uint64_t)pte<<12)|((uint64_t)pde<<21)|((uint64_t)pdpte<<30)|((uint64_t)pml4e<<39);
    return (uint64_t)(((int64_t)(v_addr<<16))>>16);
}

/* It is important to disable interrupts when doing paging map/unmap operations */

/* Linear map virtual address to given physical address in PAGE_SIZE sized page blocks,
 * count represents no. of blocks starting from physical and mapped to virtual 1:1
 *
 * Virtual and Physical address must be page aligned to respective PAGE_SIZE or UND*/
int paging_map(uint64_t v_addr, uintptr_t p_addr, enum PAGE_SIZE size, uint32_t count) {
    /* Decode virtual address into PML4, PDPT, PD, PT indices
     * PT/PD and their entries may or may not be used based on size
     * If maps at those indices dont exist then create them */ 

    uint16_t pte = v_addr >> 12 & 0x1FF;
    uint16_t pde = v_addr >> 21 & 0x1FF;
    uint16_t pdpte = v_addr >> 30 & 0x1FF;
    uint16_t pml4e = (uint64_t)v_addr >> 39 & 0x1FF;

    volatile void* pt = NULL;
    volatile void* pd = NULL;
    volatile void* pdpt = NULL;
    
    for (uint32_t i=0; i<count; i++) {
        /* PML4E decoding */ 
        if (!paging_check_map_entry_present(paging_state.pml4, pml4e)) {
            volatile void* map = paging_create_map_entry(paging_state.pml4, false);
            if (map == NULL) 
                return 9;

            paging_write_map_entry(paging_state.pml4, pml4e, paging_state.ops.p_ptr((void*)map), NOSET_PAGESIZE, NO_PAGE_MASK);
            pdpt = map;
            /* No free required as we just created a new one */
        } else {
            uint8_t PS;
            pdpt = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(paging_state.pml4, pml4e, &PS));
        }

        /* PDPTE Write (For 1G pages) */
        if (size == PAGE_1G) {
            uint8_t prevPS;
            volatile void* prevMap = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(\
                                        pdpt, pdpte, &prevPS));

            paging_write_map_entry(pdpt, pdpte, p_addr, SET_PAGESIZE, PDPT_1G_PAGE_MASK);

            if (prevMap != NULL) {
                if (prevPS == 0) {
                    /* A map exists here that has been unlinked after overwrite,
                    * free it recursively 
                    * Flush TLB as many entries may have changed */
                    paging_free_map_recursive(prevMap, PAGING_FREE_PD_DEPTH);
                    tlb_flush();
                } else {
                    /* 1G hugepage was unmapped, invalidate */
                    tlb_invalidate((void*)map_indices_to_v_addr(0, 0, pdpte, pml4e));
                }
            } else if (paging_state.initialized_refcount_map) {
                /* If previous map was not present, increment refcount */
                paging_refcount_increment(paging_state.ops.p_ptr((void*)pdpt));
            }

            p_addr += PAGE_1G;
            pdpte += 1;

            if ((p_addr + PAGE_1G) < p_addr) {
                vga_print_color("Physical address overflow while mapping\n", VGA_COLOR_RED);
                return 2;
            }

            goto pdpte_check;
        }

        /* PDPTE decoding (For 2M and 4K pages) */
        bool found = false;
        if (paging_check_map_entry_present(pdpt, pdpte)) {
            uint8_t PS;
            pd = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(pdpt, pdpte, &PS));

            if (PS == 0) found = true;
        }

        if (!found) {
            volatile void* map = paging_create_map_entry(pdpt, true);
            if (map == NULL) 
                return 9;

            paging_write_map_entry(pdpt, pdpte, paging_state.ops.p_ptr((void*)map), NOSET_PAGESIZE, NO_PAGE_MASK);
            pd = map;
        }

        /* PDE Write (For 2M pages) */
        if (size == PAGE_2M) {
            uint8_t prevPS;
            volatile void* prevMap = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(\
                                        pd, pde, &prevPS));
            paging_write_map_entry(pd, pde, p_addr, SET_PAGESIZE, PDE_2M_PAGE_MASK);
            if (prevMap != NULL) {
                if (prevPS == 0) {
                    /* A map exists here that has been unlinked after overwrite,
                    * free it recursively
                    * Flush TLB as many entries may have changed */
                    paging_free_map_recursive(prevMap, PAGING_FREE_PT_DEPTH);
                    tlb_flush();
                } else {
                    /* Invalidate previous 2M hugepage */
                    tlb_invalidate((void*)map_indices_to_v_addr(0, pde, pdpte, pml4e));
                }
            } else if (paging_state.initialized_refcount_map) {
                /* If previous map was not present, increment refcount */
                paging_refcount_increment(paging_state.ops.p_ptr((void*)pd));
            }

            p_addr += PAGE_2M;
            pde += 1;

            if ((p_addr + PAGE_2M) < p_addr) {
                vga_print_color("Physical address overflow while mapping\n", VGA_COLOR_RED);
                return 2;
            }

            goto pde_check;
        }

        /* PDE decoding (For 4K pages) */
        found = false;
        if (paging_check_map_entry_present(pd, pde)) {
            uint8_t PS;
            pt = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(pd, pde, &PS));

            if (PS == 0) found = true;
        }

        if (!found) {
            volatile void* map = paging_create_map_entry(pd, true);
            if (map == NULL) 
                return 9;

            paging_write_map_entry(pd, pde, paging_state.ops.p_ptr((void*)map), NOSET_PAGESIZE, NO_PAGE_MASK);
            pt = map; 
        }
 
        /* PTE write (For 4K pages) */
        paging_write_map_entry(pt, pte, p_addr, NOSET_PAGESIZE, NO_PAGE_MASK);

        if (!paging_check_map_entry_present(pt, pte)) {
            /* Increment refcount if entry not present before */
            if (paging_state.initialized_refcount_map)
                paging_refcount_increment(paging_state.ops.p_ptr((void*)pt));
        } else {
            /* Overwriting an entry, dont increment refcount and invalidate it
             * We choose to do singular invalidation here */
            tlb_invalidate((void*)map_indices_to_v_addr(pte, pde, pdpte, pml4e));
        }

        if ((p_addr + PAGE_4K) < p_addr) {
            vga_print_color("Physical address overflow while mapping\n", VGA_COLOR_RED);
            return 2;
        }

        p_addr += PAGE_4K;
        pte += 1;
        /* Handle overflow into next maps */
        if (pte == MAP_ENTRY_COUNT) {
            pte = 0;
            pde += 1;
pde_check:
            if (pde == MAP_ENTRY_COUNT) {
                pde = 0;
                pdpte += 1;
pdpte_check:
                if (pdpte == MAP_ENTRY_COUNT) {
                    pdpte = 0;
                    pml4e += 1;
                    if (pml4e == MAP_ENTRY_COUNT) {
                        vga_print_color("Virtual address overflow while mapping\n", VGA_COLOR_RED);
                        return 3;
                    }
                }
            }
        }
    }

    return 0;
}

/* Unmaps 'count' number of PAGE_SIZE pages. 
 * Any underlying map structures which are unlinked are freed */
int paging_unmap(uint64_t v_addr, enum PAGE_SIZE size, uint32_t count) {
    /* Decode virtual address into PML4, PDPT, PD, PT indices
     * PT/PD and their entries may or may not be used based on size */
    uint16_t pte = v_addr >> 12 & 0x1FF;
    uint16_t pde = v_addr >> 21 & 0x1FF;
    uint16_t pdpte = v_addr >> 30 & 0x1FF;
    uint16_t pml4e = (uint64_t)v_addr >> 39 & 0x1FF;

    volatile void* pt = NULL;
    volatile void* pd = NULL;
    volatile void* pdpt = NULL;

    /* TLB */
    bool invl_singular;
    bool to_tlb_flush = false;
    /* Singular invalidation vs full flush threshold
     * >THRESH 4K/2M/1G pages */
    if (count > TLB_SINGULAR_INV_THRESH)
        invl_singular = false;
    else
        invl_singular = true;

    for (uint32_t i=0; i<count; i++) {
        bool destroyed_pdpt = false;
        bool destroyed_pd = false;
        bool destroyed_pt = false;

        /* PML4E decoding */
        if (!paging_check_map_entry_present(paging_state.pml4, pml4e))
            return 1;

        uint8_t PS;
        pdpt = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(paging_state.pml4, pml4e, &PS));

        if (!paging_check_map_entry_present(pdpt, pdpte))
            return 2;

        pd = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(pdpt, pdpte, &PS));
        if (size == PAGE_1G) {
            if (PS == 0) return 9;
            /* Clear entry */
            destroyed_pdpt = paging_delete_and_free_map_entry(pdpt, pdpte);
            if (invl_singular)
                tlb_invalidate((void*)map_indices_to_v_addr(0, 0, pdpte, pml4e));
            else
                to_tlb_flush = true;
            pdpte += 1;
            goto pdpte_check;
        }

        if (!paging_check_map_entry_present(pd, pde))
            return 3;

        pt = (volatile void*)paging_state.ops.v_ptr(paging_read_map_entry(pd, pde, &PS));
        if (size == PAGE_2M) {
            if (PS == 0) return 8;
            /* Clear entry */
            destroyed_pd = paging_delete_and_free_map_entry(pd, pde);
            if (invl_singular)
                tlb_invalidate((void*)map_indices_to_v_addr(0, pde, pdpte, pml4e));
            else
                to_tlb_flush = true;
            pde += 1;
            goto pde_check;
        }

        destroyed_pt = paging_delete_and_free_map_entry(pt, pte);
        if (invl_singular)
            tlb_invalidate((void*)map_indices_to_v_addr(pte, pde, pdpte, pml4e));
        else
            to_tlb_flush = true;
        pte += 1;
        /* Handle overflow into next maps */
        if (destroyed_pt) {
            if (i<count-1) {
                vga_print_color("PT destroyed but unmap requests next entry\n", VGA_COLOR_RED);
                return 7;
            }
            destroyed_pd = paging_delete_and_free_map_entry(pd, pde);
        }
        if (pte == MAP_ENTRY_COUNT) {
            pte = 0;
            pde += 1;
pde_check:

            if (destroyed_pd) {
                if (size==PAGE_2M && i<count-1) {
                    vga_print_color("PD destroyed but unmap requests next entry\n", VGA_COLOR_RED);
                    return 6;
                }
                destroyed_pdpt = paging_delete_and_free_map_entry(pdpt, pdpte);
            }
            if (pde == MAP_ENTRY_COUNT) {
                pde = 0;
                pdpte += 1;
pdpte_check:

                if (destroyed_pdpt) {
                    if (size==PAGE_1G && i<count-1) {
                        vga_print_color("PDPT destroyed but unmap requests next entry\n", VGA_COLOR_RED);
                        return 5;
                    }
                    paging_clear_map_entry(paging_state.pml4, pml4e);
                }
                if (pdpte == MAP_ENTRY_COUNT) {
                    pdpte = 0;
                    pml4e += 1;
                    if (pml4e == MAP_ENTRY_COUNT) {
                        vga_print_color("Virtual address overflow while unmapping\n", VGA_COLOR_RED);
                        return 4;
                    }
                } 
            } else if (destroyed_pdpt) {
destroyed_pdpt_jmp:
                if (size==PAGE_1G && i<count-1) {
                    vga_print_color("PDPT destroyed but unmap requests next entry\n", VGA_COLOR_RED);
                    return 5;
                }
                paging_clear_map_entry(paging_state.pml4, pml4e);
            }
        } else if (destroyed_pd) {
            if (size==PAGE_2M && i<count-1) {
                vga_print_color("PD destroyed but unmap requests next entry\n", VGA_COLOR_RED);
                return 6;
            }
            destroyed_pdpt = paging_delete_and_free_map_entry(pdpt, pdpte);

            if (destroyed_pdpt) {
                goto destroyed_pdpt_jmp;
            }
        }
    }

    /* If we had chosen to do a full flush instead of singular
     * invalidation, do it now */
    if (!invl_singular && to_tlb_flush)
        tlb_flush();
    return 0;
}

/* Does best-fit physical to virtual address map/unmap to minimize total mapped physical
 * page count by using huge pages, as permitted by 'max_page_size'. 'v_addr', 'p_addr' 
 * and 'length' must be 4K page aligned at least */
int paging_set_map_best_fit(uint64_t v_addr, uint64_t p_addr, uint64_t length, enum PAGE_SIZE max_page_size, bool map) {
    if (length == 0) return 2;
    /* 4K page alignment check */
    if (!CHECK_PAGE_4K_ALIGN(v_addr) || !CHECK_PAGE_4K_ALIGN(p_addr) || !CHECK_PAGE_4K_ALIGN(length))
        return 3;

    while (length > 0) {
        /* Go to the max page size to use to skip higher ones */
        enum PAGE_SIZE highest_size;

        if (max_page_size == PAGE_1G && \
                CHECK_PAGE_1G_ALIGN(v_addr) && CHECK_PAGE_1G_ALIGN(p_addr) && length >= PAGE_1G)
            highest_size = PAGE_1G;
        else if (max_page_size >= PAGE_2M && \
                CHECK_PAGE_2M_ALIGN(v_addr) && CHECK_PAGE_2M_ALIGN(p_addr) && length >= PAGE_2M)
            highest_size = PAGE_2M;
        else
            highest_size = PAGE_4K;
        
        int s;
        if (map)
            s = paging_map(v_addr, p_addr, highest_size, 1);
        else
            s = paging_unmap(v_addr, highest_size, 1);

        if (s)
            return 1;
 
        v_addr += highest_size;
        p_addr += highest_size;
        length -= highest_size;
    }

    return 0;
}


