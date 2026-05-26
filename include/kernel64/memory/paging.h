#ifndef KERNEL64_PAGING_H
#define KERNEL64_PAGING_H

#include <stdint.h>
#include <stdbool.h>

#define M 52

enum PAGE_SIZE {
    PAGE_4K = 0x1000,
    PAGE_2M = 0x200000,
    PAGE_1G = 0x40000000
};

enum PAGING_FREE_DEPTH {
    /* Frees PT in PDE */
    PAGING_FREE_PT_DEPTH = 0,
    /* Frees PD in PDPTE */
    PAGING_FREE_PD_DEPTH = 1
};

/* Aligns an address by rounding up to 4K boundary */
#define PAGE_4K_ALIGN(x) (((x)&~(PAGE_4K-1))+PAGE_4K)
/* Aligns an address by rounding it down to 4K boundary */
#define PAGE_4K_ALIGN_DOWN(x) ((x)&~(PAGE_4K-1))

/* Check if address is aligned to 4K/2M/1G pages */
#define CHECK_PAGE_4K_ALIGN(x) (((x)&~(PAGE_4K-1))==(x))
#define CHECK_PAGE_2M_ALIGN(x) (((x)&~(PAGE_2M-1))==(x))
#define CHECK_PAGE_1G_ALIGN(x) (((x)&~(PAGE_1G-1))==(x))

/* How many 4K pages are there in 'x' 2M/1G pages? */
#define PAGE_COUNT_4K_TO_2M(x) (512*(x))
#define PAGE_COUNT_4K_TO_1G(x) (512*512*(x))

/* How many 4K/2M/1G pages are there in zone of 'x' bytes */
#define PAGE_COUNT_4K_IN_LEN(x) ((x)>>12)
#define PAGE_COUNT_2M_IN_LEN(x) ((x)>>21)
#define PAGE_COUNT_1G_IN_LEN(x) ((x)>>30)

/* What are the minimum number of 4K pages required to contain
 * a chunk of size 'x' */
#define PAGE_COUNT_4K_CONTAINING_LEN(x) (((x)+0xFFF)>>12)

#define MAP_SIZE PAGE_4K
#define MAP_ENTRY_COUNT 512

#define PDE_2M_PAGE_MASK ~0x1FFFFF
#define PDPT_1G_PAGE_MASK ~0x3FFFFFFF
#define NO_PAGE_MASK 0

#define SET_PAGESIZE 1
#define NOSET_PAGESIZE 0

/* If unmap page count crosses singular TLB invalidation threshold,
 * a full TLB flush (CR3 reload) is done instead */
#define TLB_SINGULAR_INV_THRESH 16

struct paging_ops {
    void* (*allocate_page)();
    void (*free_page)(void*);
    uintptr_t (*p_ptr)(void*);
    void* (*v_ptr)(uintptr_t);

    bool cleanup_enabled;
    void* (*allocate_block)(uint8_t);
    void (*free_block)(void*);
};

/* Initialize/Unintialize page allocator functions */
int paging_initialize_allocator(struct paging_ops ops);
void paging_uninitialize_allocator(); 
/* Functions to work with PML4 (top level map entry) */
volatile void* paging_new_pml4();
void paging_free_pml4(volatile void* pml4);
void paging_set_pml4(volatile void* pml4);
/* Reload PML4 to CPU, which immediately takes action */
void paging_reload_pml4(void (*reload)(volatile void* pml4));

/* Functions to map/unmap physical to virtual */
int paging_map(uint64_t v_addr, uintptr_t p_addr, enum PAGE_SIZE size, uint32_t count);
int paging_unmap(uint64_t v_addr, enum PAGE_SIZE size, uint32_t count);
int paging_set_map_best_fit(uint64_t v_addr, uint64_t p_addr, uint64_t length, enum PAGE_SIZE max_page_size, bool map);

/* paging_as.s
 * TLB Invalidation */
void tlb_invalidate(void* page);
void tlb_flush();

#endif
