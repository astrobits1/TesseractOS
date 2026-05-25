#include <kernel64/state.h>
#include <kernel64/cpu/gdt.h>
#include <kernel64/cpu/idt.h>
#include <kernel64/memory/paging.h>
#include <kernel64/memory/bump.h>
#include <kernel64/memory/pmm.h>
#include <common/bootinfo.h>
#include <common/drivers/vga/vga.h>

/* paging_as.s */
extern void* get_pml4();

void kernel_setup(struct bootinfo* info) {
    /* Put the kernel in a stable long mode state */

    /* Set up kernel and user mode code and data segments + TSS (TODO)*/
    gdt_initialise();

    /* Initialise 64 bit IDT and link exception handlers */
    idt_initialise();  

    /* Initialize linear allocator for allocation during PMM bootstrap */
    uint64_t bottom = info->kernel_physical.end;
    /* Ideally this should be a value in bootinfo that tells us
     * how much extra space was mapped, but 1MB is an agreement for now TODO */
    uint64_t top = info->kernel_physical.end+0x100000;
    uint64_t v_base = (uint64_t)_KERNEL_END;

    if (!CHECK_PAGE_4K_ALIGN(bottom))
        bottom = PAGE_4K_ALIGN(bottom);
    if (!CHECK_PAGE_4K_ALIGN(top))
        top = PAGE_4K_ALIGN_DOWN(top);
    if (!CHECK_PAGE_4K_ALIGN(v_base))
        v_base = PAGE_4K_ALIGN(v_base);

    /* TODO Kernel must stage PML4 sub-structure in bump allocator
     * to take ownership of them 
     *
     * IMP
     * We cannot free any map allocated by boot32, as it is out of allocation
     * area and address translations will fail if it is freed and reallocated.
     * So we cant overwrite any mapping set up by boot32, in the current PML4 */

    /* This expects a map of the allocator region physical bottom to a virtual address 
     * of our choice which is the kernel virtual end in this case.
     *
     * The map should already be present because bootloader has mapped some additional
     * space after kernel in virtual and told us how much. Which is what top is adjusted to */
    bump_initialize(bottom, top, v_base);
    /* bump allocator can be supplied to paging API now */

    /* Initialize allocator for paging, no cleanup enabled */
    struct paging_ops pg_ops1;
    pg_ops1.allocate_page = bump_allocate_page;
    pg_ops1.free_page = bump_free_page;
    pg_ops1.p_ptr = bump_p_ptr;
    pg_ops1.v_ptr = bump_v_ptr;

    pg_ops1.cleanup_enabled = false;
    pg_ops1.allocate_block = NULL;
    pg_ops1.free_block = NULL;
    paging_initialize_allocator(pg_ops1);
    /* Paging API can be used now */

    /* Get the already loaded PML4 and load it in our paging manager */
    volatile void* pml4 = get_pml4();
    paging_set_pml4(pml4);
    /* Allocation of new pages should work fine and any new changes are 
     * appended to the already loaded PML4 */

    /* Starting from the end of the kernel, map the entire possible memory map
     * Kernel and low memory is intentionally left out from the PMM as a safety measure */
    int s = pmm_initialize((void*)info->map_entries, info->map_entry_count, \
            top+1, STATE_PMM_MAX_P_ADDR, STATE_V_PMM_BASE);
    if (s > 0) {
        vga_print_color("Fatal PMM error during initialization\n", VGA_COLOR_BROWN);
        goto panic;
    }
    
    /* PMM is initialized
     * Reinitialize paging API, this time configured to use PMM as memory supplier */
    struct paging_ops pg_ops2;
    pg_ops2.allocate_page = pmm_allocate_page;
    pg_ops2.free_page = pmm_free_page;
    pg_ops2.p_ptr = pmm_p_ptr;
    pg_ops2.v_ptr = pmm_v_ptr;
    
    pg_ops2.cleanup_enabled = true;
    pg_ops2.allocate_block = pmm_allocate_block;
    pg_ops2.free_block = pmm_free_block;
    
    paging_uninitialize_allocator();
    if (paging_initialize_allocator(pg_ops2)) {
        vga_print_color("Fatal Paging error during initialization\n", VGA_COLOR_BROWN);
        goto panic;
    }

    return;
panic:
    kernel_panic();
}

__attribute__((noreturn))
void kernel_main(struct bootinfo* info) {
    vga_initialize();
    vga_setcolor(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_clear();

    vga_print_color("Entered long mode\n", VGA_COLOR_LIGHT_MAGENTA);
    
    kernel_setup(info);

    vga_print("\n");
    vga_print("Welcome to\n");
    vga_print_color("|''||''|                       ", VGA_COLOR_LIGHT_CYAN);
    vga_print_color(" ..|''||    .|'''.|  \n", VGA_COLOR_LIGHT_BROWN);
    vga_print_color("   ||      ....   ....   ....  ", VGA_COLOR_LIGHT_CYAN);
    vga_print_color(".|'    ||   ||..  '  \n", VGA_COLOR_LIGHT_BROWN);
    vga_print_color("   ||    .|...|| ||. '  ||. '  ", VGA_COLOR_LIGHT_CYAN);
    vga_print_color("||      ||   ''|||.  \n", VGA_COLOR_LIGHT_BROWN);
    vga_print_color("   ||    ||      . '|.. . '|.. ", VGA_COLOR_LIGHT_CYAN);
    vga_print_color("'|.     || .     '|| \n", VGA_COLOR_LIGHT_BROWN);
    vga_print_color("  .||.    '|...' |'..|' |'..|' ", VGA_COLOR_LIGHT_CYAN);
    vga_print_color(" ''|...|'  |'....|'  \n", VGA_COLOR_LIGHT_BROWN);
    lock();
}
