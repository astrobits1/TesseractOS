.section .text
.code64

/* void tlb_invalidate(void* page) 
 * Invalidate specific page */
.global tlb_invalidate
.type tlb_invalidate, @function
tlb_invalidate:
    invlpg (%rdi)
    ret

/* void tlb_flush() 
 * Flush full TLB except global pages (not used for now) */
.global tlb_flush
.type tlb_flush, @function
tlb_flush:
    push %rax
    mov %cr3, %rax
    mov %rax, %cr3
    pop %rax
    ret
