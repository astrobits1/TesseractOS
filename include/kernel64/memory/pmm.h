#ifndef COMMON_PMM_H
#define COMMON_PMM_H

#include <common/bootinfo.h>
#include <stdint.h>
#include <stdbool.h>

/* 4KB is order 0 (fundamental page)
 * 8KB is order 1
 * ...
 * 1GB is order 18 (max supported order)
 *
 * This macro checks if given memory address is aligned to the given order block size */
#define CHECK_ORDER_ALIGN(x, od) (((x)&~(((uint64_t)PAGE_4K<<(od))-1))==(x))

enum PMM_PAGE_STATE {
    PMM_PAGE_ALLOCATED,
    PMM_PAGE_FREE,
    PMM_PAGE_RESERVED
};

/* Metadata for one page (or block of some order) */
struct pmm_page_meta {
    uint8_t state;
    uint8_t order;
    uint8_t ancestor_order;
} __attribute__((packed));

/* Metadata for one zone for the local page list */
struct pmm_zone_meta {
    struct pmm_page_meta* page_list_base;
    uint64_t zone_p_base;
    uint64_t page_list_length;
    uint64_t page_count;
    bool usable;
};

int pmm_initialize(struct memory_map_entry* entries, uint32_t entry_count, uintptr_t bottom, uintptr_t top, uint64_t mirror_base);

void* pmm_allocate_block(uint8_t order);
void pmm_free_block(void* block);

/* Wrappers for handling the case of a page, for the paging API */
void* pmm_allocate_page();
void pmm_free_page(void* page);

uintptr_t pmm_p_ptr(void* v_ptr);
void* pmm_v_ptr(uintptr_t p_ptr);

void pmm_print_memory();
void pmm_print_freelists();

#endif
