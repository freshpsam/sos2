    .syntax unified
    .arm
    .text
    .align 2

    .global arm_cache_control
    .type arm_cache_control, %function
arm_cache_control:
    mrc     p15, 0, r0, c1, c0, 0
    bx      lr

/* ARM DDI0406, cache maintenance by set/way. CCSIDR describes both supported
 * CPUs (Cortex-A15/A53; neither implements the extended CCIDX layout).
 * Save registers before cleaning; the traversal itself never writes memory.
 */
    .global arm_dcache_invalidate_all
    .type arm_dcache_invalidate_all, %function
arm_dcache_invalidate_all:
    mov     r0, #0
    mov     r1, #14
    b       cache_all
    .global arm_dcache_invalidate_local
    .type arm_dcache_invalidate_local, %function
arm_dcache_invalidate_local:
    mov     r0, #0
    mov     r1, #2              @ Only private L1; other CPUs may own dirty L2 lines.
    b       cache_all
/* Clean and invalidate every data cache level, L2 included, before the last
 * CPU powers the cluster's L2 off (msm-pm.c flush_cache_all); then the
 * private L1 alone, before one CPU powers down with the L2 kept. Lines
 * dirtied after the traversal hold only stack the resume does not read. */
    .global arm_dcache_clean_invalidate_all
    .type arm_dcache_clean_invalidate_all, %function
arm_dcache_clean_invalidate_all:
    mov     r0, #1
    mov     r1, #14
    b       cache_all
    .global arm_dcache_clean_invalidate_local
    .type arm_dcache_clean_invalidate_local, %function
arm_dcache_clean_invalidate_local:
    mov     r0, #1
    mov     r1, #2
    b       cache_all
    .type cache_all, %function
cache_all:
    push    {r4-r11, lr}
    mov     r11, r0
    mov     r6, r1
    dsb     sy
    mrc     p15, 1, r10, c0, c0, 1  @ CLIDR
    mov     r8, #0                  @ level << 1
1:  add     r2, r8, r8, lsr #1
    mov     r1, r10, lsr r2
    and     r1, r1, #7
    cmp     r1, #2                  @ Skip absent and instruction-only caches.
    blo     4f
    cmp     r1, #4
    bhi     4f
    mcr     p15, 2, r8, c0, c0, 0   @ CSSELR
    isb
    mrc     p15, 1, r1, c0, c0, 0   @ CCSIDR
    and     r2, r1, #7
    add     r2, r2, #4              @ log2(line bytes)
    ubfx    r3, r1, #3, #10         @ last way
    clz     r4, r3                  @ way operand shift (32 is safe for way zero)
    ubfx    r5, r1, #13, #15        @ last set
2:  mov     r7, r5
3:  orr     r9, r8, r3, lsl r4
    orr     r9, r9, r7, lsl r2
    cmp     r11, #0
    mcreq   p15, 0, r9, c7, c6, 2   @ DCISW
    mcrne   p15, 0, r9, c7, c14, 2  @ DCCISW
    subs    r7, r7, #1
    bpl     3b
    subs    r3, r3, #1
    bpl     2b
4:  add     r8, r8, #2
    cmp     r8, r6
    blo     1b
    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0
    dsb     sy
    isb
    pop     {r4-r11, pc}

/* Flush the temporary cached identity mapping before changing memory types.
 * No stores (including stack spills) occur between the flush and SCTLR write.
 * The saved control value has M=C=0, as required by the loader contract.
 */
    .global arm_cache_restore
    .type arm_cache_restore, %function
arm_cache_restore:
    push    {r0, lr}
    mov     r0, #1
    mov     r1, #14
    bl      cache_all
    pop     {r0, lr}
    dsb     sy
    mcr     p15, 0, r0, c1, c0, 0
    isb
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0
    mcr     p15, 0, r0, c7, c5, 0
    mcr     p15, 0, r0, c7, c5, 6
    dsb     sy
    isb
    bx      lr
    .section .note.GNU-stack, "", %progbits
