    .text
    .global arm_cache_control
    .type arm_cache_control, %function
arm_cache_control:
    mrs     x0, sctlr_el1
    ret

/* Data cache maintenance by set/way (Arm ARM D7.2, CLIDR_EL1 and CCSIDR_EL1
 * without FEAT_CCIDX, as on the Cortex-A53). Whole-cache maintenance is
 * restricted to boot, before releasing other CPUs; the traversal itself never
 * writes memory. */
    .global arm_dcache_invalidate_all
    .type arm_dcache_invalidate_all, %function
arm_dcache_invalidate_all:
    mov     x0, #0
    mov     x1, #14
    b       cache_all
    .global arm_dcache_invalidate_local
    .type arm_dcache_invalidate_local, %function
arm_dcache_invalidate_local:
    mov     x0, #0
    mov     x1, #2              /* Only private L1; other CPUs may own dirty L2 lines. */

    /* x0: 0 invalidates, 1 cleans and invalidates; x1: levels << 1. */
cache_all:
    dsb     sy
    mrs     x10, clidr_el1
    mov     x8, #0              /* level << 1 */
1:  add     x2, x8, x8, lsr #1
    lsr     x3, x10, x2
    and     x3, x3, #7
    cmp     x3, #2              /* Skip absent and instruction-only caches. */
    b.lo    5f
    cmp     x3, #4
    b.hi    5f
    msr     csselr_el1, x8
    isb
    mrs     x3, ccsidr_el1
    and     x2, x3, #7
    add     x2, x2, #4          /* log2(line bytes) */
    ubfx    x4, x3, #3, #10     /* last way */
    clz     w5, w4              /* way operand shift (32 is safe for way zero) */
    ubfx    x6, x3, #13, #15    /* last set */
2:  mov     x7, x6
3:  lsl     x9, x4, x5
    orr     x9, x9, x8
    lsl     x11, x7, x2
    orr     x9, x9, x11
    cbz     x0, 4f
    dc      cisw, x9
    b       6f
4:  dc      isw, x9
6:  subs    x7, x7, #1
    b.pl    3b
    subs    x4, x4, #1
    b.pl    2b
    dsb     sy                  /* Finish writebacks before cleaning the next level. */
5:  add     x8, x8, #2
    cmp     x8, x1
    b.lo    1b
    msr     csselr_el1, xzr
    dsb     sy
    isb
    ret

/* Back to the boot contract after arch/arm64/boot_cache.c: every data cache
 * level cleaned and invalidated to the point of coherency, then SCTLR_EL1
 * restored from w0 (M=C=0). Nothing is stored, the stack included, between
 * the flush and the SCTLR write; a clean line loaded meanwhile is invalidated
 * before translation starts again (enable_translation). */
    .global arm_cache_restore
    .type arm_cache_restore, %function
arm_cache_restore:
    mov     w12, w0
    mov     x13, x30
    mov     x0, #1
    mov     x1, #14
    bl      cache_all
    msr     sctlr_el1, x12
    isb
    tlbi    vmalle1
    ic      iallu
    dsb     sy
    isb
    ret     x13
    .section .note.GNU-stack, "", %progbits
