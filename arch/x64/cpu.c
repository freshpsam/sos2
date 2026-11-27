#include "x86.h"
#include "platform.h"

/* Each CPU's descriptor tables and system registers, the boot CPU's start
 * and the kernel's faults. The GDT holds the kernel's code and data, the
 * user's data and code (SYSCALL and SYSRET's STAR layout) and the CPU's TSS,
 * whose RSP0 and IST1 are its exception stack and IST2 its fatal stack. One
 * IDT serves every CPU: its exceptions run on IST1, NMI, double fault and
 * machine check on IST2, interrupts on the stack they arrive on (RSP0's from
 * user mode). x64's syscall state: SYSCALL to x86_syscall_entry with
 * interrupts, traps, direction and alignment checks masked.
 * https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html */
enum {
    MSR_PAT = 0x277,
    MSR_EFER = 0xc0000080,
    MSR_STAR = 0xc0000081,
    MSR_LSTAR = 0xc0000082,
    MSR_SFMASK = 0xc0000084,
    MSR_FS_BASE = 0xc0000100,
    MSR_GS_BASE = 0xc0000101,
    MSR_KERNEL_GS_BASE = 0xc0000102,
    EFER_SCE = 1,
    EFER_LME = 1 << 8,
    EFER_NXE = 1 << 11,
    CR0_MP = 1 << 1,
    CR0_EM = 1 << 2,
    CR0_TS = 1 << 3,
    CR0_NE = 1 << 5,
    CR0_WP = 1 << 16,
    CR4_PGE = 1 << 7,
    CR4_OSFXSR = 1 << 9,
    CR4_OSXMMEXCPT = 1 << 10,
    CR4_SMEP = 1 << 20,
    CR4_SMAP = 1 << 21
};

struct x86_features x86_features;
unsigned char x86_smap;
unsigned char x86_exception_stacks[CPU_MAX][ARCH_EXCEPTION_STACK_BYTES]
    __attribute__((aligned(4096)));
unsigned char x86_fatal_stacks[CPU_MAX][X86_FATAL_STACK_BYTES] __attribute__((aligned(4096)));

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
};

_Static_assert(sizeof(struct tss) == 104, "TSS");

struct gate {
    uint16_t offset0, selector;
    uint8_t ist, type;
    uint16_t offset1;
    uint32_t offset2, reserved;
};

_Static_assert(sizeof(struct gate) == 16, "IDT gate");

static struct tss tss[CPU_MAX] __attribute__((aligned(64)));
static uint64_t gdt[CPU_MAX][7] __attribute__((aligned(64)));
static struct gate idt[256] __attribute__((aligned(4096)));

static void features(void) {
    uint32_t r[4];
    x86_cpuid(0, 0, r);
    uint32_t highest = r[0];
    x86_cpuid(1, 0, r);
    x86_features.x2apic = r[2] & (1u << 21);
    x86_features.tsc_deadline = r[2] & (1u << 24);
    if (highest >= 7) {
        x86_cpuid(7, 0, r);
        x86_features.smep = r[1] & (1u << 7);
        x86_features.smap = r[1] & (1u << 20);
    }
    x86_cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000007) {
        x86_cpuid(0x80000007, 0, r);
        x86_features.invariant_tsc = r[3] & (1u << 8);
    }
}

static void idt_init(void) {
    for (unsigned v = 0; v < 256; ++v) {
        uintptr_t entry = (uintptr_t)(x86_trap_stubs + 16 * v);
        uint8_t ist = v == 2 || v == 8 || v == 18 ? 2 : v < 32 ? 1 : 0;
        idt[v] = (struct gate){.offset0 = (uint16_t)entry,
                               .selector = X86_KERNEL_CS,
                               .ist = ist,
                               .type = 0x8e,
                               .offset1 = (uint16_t)(entry >> 16),
                               .offset2 = (uint32_t)(entry >> 32)};
    }
}

static uint64_t read_cr0(void) {
    uint64_t value;
    __asm__ volatile("movq %%cr0, %0" : "=r"(value));
    return value;
}

static uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile("movq %%cr4, %0" : "=r"(value));
    return value;
}

void x86_cpu_setup(struct cpu_record *cpu) {
    unsigned i = cpu->index;
    tss[i] = (struct tss){
        .rsp = {cpu->exception_sp},
        .ist = {cpu->exception_sp, (uintptr_t)(x86_fatal_stacks[i] + X86_FATAL_STACK_BYTES)},
        .iomap = sizeof(struct tss)};
    uintptr_t base = (uintptr_t)&tss[i];
    gdt[i][0] = 0;
    gdt[i][1] = 0x00209a0000000000; /* kernel code, 64-bit */
    gdt[i][2] = 0x0000920000000000; /* kernel data */
    gdt[i][3] = 0x0000f20000000000; /* user data */
    gdt[i][4] = 0x0020fa0000000000; /* user code, 64-bit */
    gdt[i][5] = (sizeof(struct tss) - 1) | (base & 0xffffff) << 16 | 0x89ull << 40 |
                (uint64_t)(base >> 24 & 0xff) << 56;
    gdt[i][6] = base >> 32;

    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } gdtr = {sizeof(gdt[i]) - 1, (uintptr_t)gdt[i]}, idtr = {sizeof(idt) - 1, (uintptr_t)idt};

    /* The new GDT, its code segment through a far return, data segments
     * and the TSS; FS and GS stay null, their bases in MSRs. */
    __asm__ volatile("lgdt %0\n"
                     "pushq %1\n"
                     "leaq 1f(%%rip), %%rax\n"
                     "pushq %%rax\n"
                     "lretq\n"
                     "1: movw %w2, %%ds\n"
                     "movw %w2, %%es\n"
                     "movw %w2, %%ss\n"
                     "movw %w3, %%fs\n"
                     "movw %w3, %%gs\n"
                     "ltr %w4\n"
                     "lidt %5"
                     :
                     : "m"(gdtr), "i"(X86_KERNEL_CS), "r"(X86_KERNEL_DS), "r"(0), "r"(X86_TSS),
                       "m"(idtr)
                     : "rax", "memory");
    x86_wrmsr(MSR_GS_BASE, (uintptr_t)cpu);
    x86_wrmsr(MSR_KERNEL_GS_BASE, 0);
    x86_wrmsr(MSR_FS_BASE, 0);
    /* Write-back, write-combining, uncached-minus and uncached, twice:
     * PWT alone selects write-combining, PCD and PWT uncached. */
    x86_wrmsr(MSR_PAT, 0x0007010600070106ull);
    x86_wrmsr(MSR_EFER, x86_rdmsr(MSR_EFER) | EFER_SCE | EFER_LME | EFER_NXE);
    x86_wrmsr(MSR_STAR, (uint64_t)(X86_KERNEL_DS) << 48 | (uint64_t)X86_KERNEL_CS << 32);
    extern char x86_syscall_entry[];
    x86_wrmsr(MSR_LSTAR, (uintptr_t)x86_syscall_entry);
    x86_wrmsr(MSR_SFMASK, 0x44700); /* IF, TF, DF, NT, AC */
    uint64_t cr0 = (read_cr0() | CR0_MP | CR0_NE | CR0_WP) & ~(uint64_t)(CR0_EM | CR0_TS);
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
    uint64_t cr4 = read_cr4() | CR4_PGE | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (x86_features.smep) cr4 |= CR4_SMEP;
    if (x86_features.smap) cr4 |= CR4_SMAP;
    __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");
}

void kernel_main(const void *dtb);

/* The boot CPU, from boot.S: its record, descriptor tables and the TSC's
 * frequency (which time_us needs from its first call), then the kernel. */
_Noreturn void x86_boot(const void *start_info) {
    features();
    x86_smap = x86_features.smap;
    idt_init();
    struct cpu_record *cpu = &cpu_records[0];
    cpu->self = cpu;
    cpu->index = 0;
    cpu->exception_sp = (uintptr_t)(x86_exception_stacks[0] + ARCH_EXCEPTION_STACK_BYTES);
    x86_cpu_setup(cpu);
    (void)x86_tsc_calibrate();
    kernel_main(start_info);
    for (;;) __asm__ volatile("cli\n hlt");
}

/* The registers entry.S pushed, then the vector, the error code and the
 * CPU's frame. */
struct x86_fault {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
};

static char *hex(char *out, uint64_t value) {
    *out++ = '0';
    *out++ = 'x';
    int shift = 60;
    while (shift > 0 && !(value >> shift)) shift -= 4;
    for (; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 15];
    *out = 0;
    return out;
}

/* A kernel-mode exception: fatal. A secondary CPU leaves it to CPU 0, as on
 * ARM; on CPU 0 the boot error screen shows the vector, where it happened
 * and, for a page fault, the address (locate it in kernel.elf). */
_Noreturn void x86_kernel_fault(const struct x86_fault *f) {
    /* The fault may have hit cpu_user_run after its SWAPGS. */
    uintptr_t gs = (uintptr_t)x86_rdmsr(MSR_GS_BASE);
    if (gs < (uintptr_t)cpu_records || gs >= (uintptr_t)(cpu_records + CPU_MAX))
        __asm__ volatile("swapgs" ::: "memory");
    struct cpu_record *cpu = arch_cpu_local();
    uint64_t address;
    __asm__ volatile("movq %%cr2, %0" : "=r"(address));
    if (cpu->index) {
        cpu->trap = (struct cpu_trap){.context = {.sp = f->rsp, .pc = f->rip, .flags = f->rflags},
                                      .vector = 1,
                                      .status = f->vector << 32 | (uint32_t)f->error,
                                      .address = address,
                                      .cs = f->cs};
        x86_cpu_failed(cpu);
    }
    static char text[160];
    char *p = stpcpy(text, "exception in protected kernel: vector ");
    p = decimal(p, (unsigned)f->vector);
    p = hex(stpcpy(p, " at RIP "), f->rip);
    p = hex(stpcpy(p, ", error "), f->error);
    p = hex(stpcpy(p, ", RSP "), f->rsp);
    if (f->vector == 14) hex(stpcpy(p, ", address "), address);
    panic(text);
}

/* User SSE state: SSE needs no enabling past x86_cpu_setup's CR0 and CR4,
 * and the kernel never uses its registers, so they hold the user's between
 * restore and save. */
void user_runtime_init(void) { }

void user_runtime_save(struct user_runtime *r) {
    __asm__ volatile("fxsave64 %0" : "=m"(r->fxsave));
    r->valid = 1;
}

/* The reset state for a thread never saved: x87 control word 0x37F, MXCSR
 * 0x1F80 and its mask, every register zero. */
static const unsigned char reset_state[512] __attribute__((aligned(16))) = {
    [0] = 0x7f, [1] = 0x03, [24] = 0x80, [25] = 0x1f, [28] = 0xff, [29] = 0xff};

void user_runtime_restore(const struct user_runtime *r) {
    const unsigned char *image = r->valid ? r->fxsave : reset_state;
    __asm__ volatile("fxrstor64 %0" : : "m"(*(const unsigned char (*)[512])image));
    x86_wrmsr(MSR_FS_BASE, r->thread_pointer);
}
