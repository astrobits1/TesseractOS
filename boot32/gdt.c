#include <boot32/gdt.h>

/* Allocate space for segment descriptors */

uint8_t GDT[8*SEG_COUNT_BOOT];

void gdt_write_entry(uint16_t index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    /* Writes GDT Descriptor values to given index in the GDT */
    uint16_t offset = index*8;

    GDT[offset] = limit&0xFF;
    GDT[offset+1] = limit>>8 & 0xFF;
    GDT[offset+2] = base&0xFF;
    GDT[offset+3] = base>>8 & 0xFF;
    GDT[offset+4] = base>>16 & 0xFF;
    GDT[offset+5] = access;
    GDT[offset+6] = (limit>>16 & 0xF) | ((flags&0xF) << 4);
    GDT[offset+7] = base>>24 & 0xFF;
}


void gdt_initialise() {
    /* 32 bit */
    /* Null descriptor */
    gdt_write_entry(0, 0, 0, 0, 0);

    /* Kernel ring 0 code segment (flat) */
    gdt_write_entry(1, 0, 0xFFFFF, 0x9A, 0xC); 

    /* Kernel ring 0 data segment (flat) */
    gdt_write_entry(2, 0, 0xFFFFF, 0x92, 0xC);

    /* Load our GDT to the GDTR using assembly routine */
    gdtr_load((void*)GDT, SEG_COUNT_BOOT*8-1);

    /* Reloads code segment located at index 1 to CS 
     * and data segment at index 2 to DS and the rest
     * This has to be hardcoded because CS cannot be written to from
     * registers */
    reload_seg_regs();
}


void gdt_set_long_mode() {
    /* Enable long mode bit, disable DB size flag *
     * base and limit are ignored in long mode */

    /* Kernel ring 0 code */
    gdt_write_entry(1, 0, 0xFFFFF, 0x9A, 0xA);
    /* Kernel ring 0 data */
    gdt_write_entry(2, 0, 0xFFFFF, 0x92, 0xA);
}
