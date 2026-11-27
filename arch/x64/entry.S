/* x64 traps, user slices and kernel context switches.
 *
 * Kernel services are serialized on the boot CPU. A user slice is a bounded
 * call, cpu_user_run, which saves the kernel's callee-saved registers on its
 * stack (struct cpu_record's kernel_sp) and enters user mode with IRETQ.
 * Every trap from user mode (SYSCALL, the scheduler's timer, a fault) saves
 * the user's registers straight into the CPU's struct cpu_trap through GS,
 * calls x86_trap_dispatch on the kernel stack and returns from cpu_user_run:
 * arm64's entry.S contract. The kernel runs with interrupts masked; the only
 * ones it takes in kernel mode end an idle CPU's HLT (smp.c) or CPU 0's
 * standby sleep (apic.c), and only acknowledge. Any exception in kernel mode
 * is fatal and runs on the CPU's exception stack (IST), out of reach of an
 * overflow of the stack that faulted.
 */
#include "arch/smp.h"
#define TRAP(offset) %gs:(ARCH_CPU_TRAP + (offset))
#define CONTEXT_SP    120
#define CONTEXT_PC    128
#define CONTEXT_FLAGS 136
#define TRAP_VECTOR   144
#define TRAP_STATUS   152
#define TRAP_ADDRESS  160
#define TRAP_CS       168

    .text

    /* One 16-byte stub per vector, which pushes a zero error code where the
     * CPU pushes none, then the vector. cpu.c points the IDT at them. */
    .macro stub vector
    .balign 16
    .if (\vector == 8) || (\vector >= 10 && \vector <= 14) || (\vector == 17) || (\vector == 21) || (\vector == 29) || (\vector == 30)
    pushq   $\vector
    .else
    pushq   $0
    pushq   $\vector
    .endif
    jmp     x86_trap_common
    .endm

    .balign 16
    .global x86_trap_stubs
x86_trap_stubs:
    .altmacro
    .set vector, 0
    .rept 256
    stub %vector
    .set vector, vector + 1
    .endr
    .noaltmacro

    /* rsp: vector, error code, then the CPU's frame: rip, cs, rflags, rsp,
     * ss. */
x86_trap_common:
    cmpq    $X86_WAKE_VECTOR, (%rsp)
    je      acknowledge
    cmpq    $X86_SPURIOUS_VECTOR, (%rsp)
    je      resume
    testb   $3, 24(%rsp)
    jz      kernel_trap
    swapgs
    movq    %rdi, TRAP(0)
    movq    %rsi, TRAP(8)
    movq    %rdx, TRAP(16)
    movq    %r10, TRAP(24)
    movq    %r8, TRAP(32)
    movq    %r9, TRAP(40)
    movq    %rax, TRAP(48)
    movq    %rbx, TRAP(56)
    movq    %rcx, TRAP(64)
    movq    %rbp, TRAP(72)
    movq    %r11, TRAP(80)
    movq    %r12, TRAP(88)
    movq    %r13, TRAP(96)
    movq    %r14, TRAP(104)
    movq    %r15, TRAP(112)
    movq    16(%rsp), %rax
    movq    %rax, TRAP(CONTEXT_PC)
    movq    24(%rsp), %rax
    movq    %rax, TRAP(TRAP_CS)
    movq    32(%rsp), %rax
    movq    %rax, TRAP(CONTEXT_FLAGS)
    movq    40(%rsp), %rax
    movq    %rax, TRAP(CONTEXT_SP)
    movq    (%rsp), %rax            /* vector */
    movl    8(%rsp), %ecx           /* error code */
    movq    %rax, %rdx
    shlq    $32, %rdx
    orq     %rcx, %rdx
    movq    %rdx, TRAP(TRAP_STATUS)
    movq    %cr2, %rdx
    movq    %rdx, TRAP(TRAP_ADDRESS)
    /* arch/arm's codes: 6 the timer, 3 or 4 a page fault on an instruction
     * fetch or data (the error code's I/D bit), 1 anything else. */
    movq    $1, %rdx
    cmpq    $X86_TIMER_VECTOR, %rax
    jne     1f
    movq    $6, %rdx
    jmp     2f
1:  cmpq    $14, %rax
    jne     2f
    testl   $0x10, %ecx
    movq    $4, %rdx
    jz      2f
    movq    $3, %rdx
2:  movq    %rdx, TRAP(TRAP_VECTOR)

    /* Back on the kernel stack that cpu_user_run left, to return from it. */
to_kernel:
    movq    %gs:0, %rsp
    testb   $1, x86_smap(%rip)
    jz      1f
    clac
1:  cld
    movq    %gs:ARCH_CPU_SELF, %rdi
    addq    $ARCH_CPU_TRAP, %rdi
    call    x86_trap_dispatch
    addq    $8, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

    /* The kernel was interrupted only if it halted: the scheduler's timer
     * ends CPU 0's standby sleep. Anything else is a fault. */
kernel_trap:
    cmpq    $X86_TIMER_VECTOR, (%rsp)
    jne     kernel_fault
    /* An interrupt to acknowledge, in either mode, before returning to what
     * it interrupted: GS is not touched, and nor is anything but these
     * caller-saved registers. */
acknowledge:
    pushq   %rax
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    cld
    call    x86_interrupt_eoi
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rax
resume:
    addq    $16, %rsp
    iretq

    /* struct x86_fault (cpu.c): r15 up to rax, then the vector, the error
     * code and the CPU's frame. */
kernel_fault:
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, %rdi
    cld
    call    x86_kernel_fault
1:  cli
    hlt
    jmp     1b

    /* SYSCALL from user mode, interrupts masked (SFMASK): rcx holds the
     * return address and r11 the flags. No stack is used before
     * to_kernel's. */
    .global x86_syscall_entry
    .type x86_syscall_entry, @function
x86_syscall_entry:
    swapgs
    movq    %rsp, TRAP(CONTEXT_SP)
    movq    %rdi, TRAP(0)
    movq    %rsi, TRAP(8)
    movq    %rdx, TRAP(16)
    movq    %r10, TRAP(24)
    movq    %r8, TRAP(32)
    movq    %r9, TRAP(40)
    movq    %rax, TRAP(48)
    movq    %rbx, TRAP(56)
    movq    %rcx, TRAP(64)
    movq    %rbp, TRAP(72)
    movq    %r11, TRAP(80)
    movq    %r12, TRAP(88)
    movq    %r13, TRAP(96)
    movq    %r14, TRAP(104)
    movq    %r15, TRAP(112)
    movq    %rcx, TRAP(CONTEXT_PC)
    movq    %r11, TRAP(CONTEXT_FLAGS)
    movq    $X86_USER_CS, TRAP(TRAP_CS)
    movq    $2, TRAP(TRAP_VECTOR)
    movq    $0, TRAP(TRAP_STATUS)
    movq    $0, TRAP(TRAP_ADDRESS)
    jmp     to_kernel
    .size x86_syscall_entry, .-x86_syscall_entry

    /* void cpu_user_run(const struct user_context *): runs a user slice.
     * The flags keep only the arithmetic ones and the direction, with
     * interrupts on: no trap, alignment-check or I/O privilege state passes
     * between slices. */
    .global cpu_user_run
    .type cpu_user_run, @function
cpu_user_run:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    subq    $8, %rsp
    movq    %rsp, %gs:0             /* struct cpu_record's kernel_sp */
    pushq   $X86_USER_DS
    pushq   CONTEXT_SP(%rdi)
    movq    CONTEXT_FLAGS(%rdi), %rax
    andq    $0xcd5, %rax
    orq     $0x202, %rax
    pushq   %rax
    pushq   $X86_USER_CS
    pushq   CONTEXT_PC(%rdi)
    movq    8(%rdi), %rsi
    movq    16(%rdi), %rdx
    movq    24(%rdi), %r10
    movq    32(%rdi), %r8
    movq    40(%rdi), %r9
    movq    48(%rdi), %rax
    movq    56(%rdi), %rbx
    movq    64(%rdi), %rcx
    movq    72(%rdi), %rbp
    movq    80(%rdi), %r11
    movq    88(%rdi), %r12
    movq    96(%rdi), %r13
    movq    104(%rdi), %r14
    movq    112(%rdi), %r15
    movq    0(%rdi), %rdi
    swapgs
    iretq
    .size cpu_user_run, .-cpu_user_run

/* Cooperative kernel-only stack switch, with interrupts masked; kernel C is
 * built with general registers only. The frame matches arch_kernel_frame
 * (arch/x64/include/arch/kernel_switch.h): r15, r14, r13, r12, rbx and
 * rbp upwards from RSP, then the return address. */
    .global kernel_switch
    .type kernel_switch, @function
kernel_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size kernel_switch, .-kernel_switch

    /* jmp_buf (include/boot/setjmp.h): rbx, rbp, r12-r15, rsp, rip. */
    .global sos2_boot_setjmp, sos2_boot_longjmp
    .type sos2_boot_setjmp, @function
sos2_boot_setjmp:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax
    ret
    .size sos2_boot_setjmp, .-sos2_boot_setjmp
    .type sos2_boot_longjmp, @function
sos2_boot_longjmp:
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax
1:  jmpq    *56(%rdi)
    .size sos2_boot_longjmp, .-sos2_boot_longjmp
    .section .note.GNU-stack, "", @progbits
