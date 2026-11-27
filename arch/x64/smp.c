#include "x86.h"
#include "machine.h"
#include "boot_progress.h"
#include "platform.h"
#include <stddef.h>

/* The CPUs' records and mailboxes, as arch/arm/smp.c keeps them: the boot
 * CPU submits a bounded user slice to an idle CPU (BUSY), which runs it and
 * reports DONE, and the boot CPU collects it. x86 orders ordinary stores and
 * loads itself, so publishing is a release store. An idle CPU polls its
 * mailbox for a short while, then halts with interrupts on, and the boot CPU
 * sends a wake IPI only to a CPU halted (halted[]), so that idle CPUs leave
 * their host cores to others under KVM. Secondary CPUs start through
 * INIT-SIPI-SIPI at the real-mode trampoline (boot.S), with APIC IDs from
 * ACPI's MADT. */
struct cpu_record cpu_records[CPU_MAX];
unsigned char cpu_stacks[CPU_MAX - 1][CPU_STACK_BYTES] __attribute__((aligned(4096)));
struct cpu_boot_record x86_cpu_boot[CPU_MAX];

_Static_assert(sizeof(struct cpu_boot_record) == 32, "secondary boot assembly record");
_Static_assert(offsetof(struct cpu_record, kernel_sp) == 0, "per-CPU continuation ABI");
_Static_assert(offsetof(struct cpu_record, exception_sp) == ARCH_CPU_EXCEPTION_SP,
               "exception stack ABI");
_Static_assert(offsetof(struct cpu_record, self) == ARCH_CPU_SELF, "record ABI");
_Static_assert(offsetof(struct cpu_record, index) == ARCH_CPU_INDEX, "index ABI");
_Static_assert(offsetof(struct cpu_record, trap) == ARCH_CPU_TRAP, "trap ABI");
_Static_assert(offsetof(struct cpu_record, runtime) % 16 == 0, "FXSAVE alignment");

static unsigned online_mask = 1;
static uint32_t halted[CPU_MAX];

static uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void publish(uint32_t *p, uint32_t value) { __atomic_store_n(p, value, __ATOMIC_RELEASE); }

unsigned smp_online(void) { return online_mask; }

bool cpu_idle(unsigned cpu) {
    return cpu < CPU_MAX && (online_mask & (1u << cpu)) &&
           load(&cpu_records[cpu].state) == CPU_IDLE;
}

bool cpu_failed(unsigned cpu) {
    return cpu < CPU_MAX && (online_mask & (1u << cpu)) &&
           load(&cpu_records[cpu].state) == CPU_FAILED;
}

const struct cpu_record *cpu_result(unsigned cpu) {
    return cpu < CPU_MAX && load(&cpu_records[cpu].state) == CPU_DONE ? &cpu_records[cpu] : 0;
}

void cpu_release(unsigned cpu) {
    if (!cpu_result(cpu)) panic("CPU result released before completion");
    publish(&cpu_records[cpu].state, CPU_IDLE);
}

void cpu_user_run(const struct user_context *context); /* entry.S */

/* Each CPU's last user space and the mapping epoch it ran at. */
static const struct vm_space *last_space[CPU_MAX];
static uint64_t last_epoch[CPU_MAX];

static void execute(struct cpu_record *cpu) {
    unsigned id = cpu->index;
    uint64_t epoch = vm_epoch();
    /* As on ARM: code can be replaced after a Close, or made executable on
     * another core (both advance the epoch); anything but the same space at
     * the same epoch flushes its translations. */
    bool flush = cpu->space != last_space[id] || epoch != last_epoch[id];
    vm_enter(cpu->space, flush);
    user_runtime_restore(&cpu->runtime);
    /* A stack the user left non-canonical faults in user mode, not at
     * IRETQ. */
    if (cpu->context.sp >> 47) cpu->context.sp = 0;
    timer_arm_us(cpu->budget);
    uint64_t start = time_us();
    cpu->active = true;
    cpu_user_run(&cpu->context);
    cpu->active = false;
    cpu->elapsed = time_us() - start;
    vm_leave();
    last_space[id] = cpu->space;
    last_epoch[id] = epoch;
    /* DONE is also the acknowledgement that this CPU no longer uses the
     * process root. Only then may the boot CPU unmap or reclaim its pages. */
    publish(&cpu->state, CPU_DONE);
}

void cpu_submit(unsigned id, const struct vm_space *space, const struct user_context *context,
                const struct user_runtime *runtime, unsigned budget) {
    if (!cpu_idle(id) || !space || !space->root || !budget || budget > 5000)
        panic("invalid CPU dispatch");
    struct cpu_record *cpu = &cpu_records[id];
    cpu->space = space;
    cpu->context = *context;
    cpu->runtime = *runtime;
    cpu->budget = budget;
    publish(&cpu->state, CPU_BUSY);
    if (!id) {
        execute(cpu);
        return;
    }
    /* The store above, then halted[]: a CPU either sees BUSY before it
     * halts, or is seen halted and woken. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&halted[id], __ATOMIC_RELAXED))
        x86_send_ipi(cpu->mpidr, 0x4000 | X86_WAKE_VECTOR);
}

/* A trap from user mode, which entry.S saved in cpu->trap: the slice ends.
 * The kernel takes no external interrupt but its timer. */
void x86_trap_dispatch(struct cpu_trap *trap) {
    struct cpu_record *cpu = arch_cpu_local();
    unsigned vector = (unsigned)(trap->status >> 32);
    if (!cpu->active || !arch_trap_from_user(trap) ||
        (vector >= 32 && vector != X86_TIMER_VECTOR)) {
        if (cpu->index) x86_cpu_failed(cpu);
        static char text[48];
        *decimal(stpcpy(text, "unexpected trap, vector "), vector) = 0;
        panic(text);
    }
    timer_stop(vector == X86_TIMER_VECTOR);
    user_runtime_save(&cpu->runtime);
}

_Noreturn void x86_cpu_failed(struct cpu_record *cpu) {
    publish(&cpu->state, CPU_FAILED);
    for (;;) __asm__ volatile("cli\n hlt");
}

/* Poll the mailbox for about 50 us, then halt until the wake IPI. */
static void wait_for_work(struct cpu_record *cpu) {
    uint64_t until = x86_rdtsc() + x86_tsc_frequency / 20000;
    while (x86_rdtsc() < until) {
        if (load(&cpu->state) != CPU_IDLE) return;
        __asm__ volatile("pause");
    }
    __atomic_store_n(&halted[cpu->index], 1, __ATOMIC_SEQ_CST);
    if (load(&cpu->state) == CPU_IDLE) __asm__ volatile("sti\n hlt\n cli" ::: "memory");
    __atomic_store_n(&halted[cpu->index], 0, __ATOMIC_RELAXED);
}

/* A secondary CPU, from boot.S on its own stack and the kernel's tables. */
_Noreturn void x86_cpu_secondary(struct cpu_record *cpu) {
    x86_cpu_setup(cpu);
    if (!vm_secondary_init()) x86_cpu_failed(cpu);
    cpu->mmu_enabled = true;
    if (!timer_secondary_init()) x86_cpu_failed(cpu);
    last_space[cpu->index] = 0;
    user_runtime_init();
    publish(&cpu->state, CPU_IDLE);
    for (;;) {
        if (load(&cpu->state) == CPU_BUSY) execute(cpu);
        else wait_for_work(cpu);
    }
}

void smp_init(const struct platform_devices *devices) {
    struct cpu_record *boot = &cpu_records[0];
    uint32_t boot_id = x86_apic_id();
    boot->mpidr = boot_id;
    boot->state = CPU_IDLE;
    boot->mmu_enabled = true;
    for (unsigned i = 0; i < CPU_MAX; ++i) x86_cpu_boot[i].mpidr = UINT64_MAX;
    if (devices->cpu_count < 2) return;
    /* The trampoline, with the kernel's root in its last word. */
    x86_trampoline_page(true);
    memcpy((void *)X86_TRAMPOLINE, x86_trampoline, (size_t)(x86_trampoline_end - x86_trampoline));
    uint32_t root = (uint32_t)x86_kernel_root();
    memcpy((unsigned char *)X86_TRAMPOLINE + (x86_trampoline_root - x86_trampoline), &root,
           sizeof(root));
    x86_trampoline_page(false);
    unsigned next = 1;
    for (unsigned i = 0; i < devices->cpu_count && next < CPU_MAX; ++i) {
        uint32_t id = devices->cpu_mpidr[i];
        if (id == boot_id || id > 255) continue; /* boot.S finds records by an 8-bit ID */
        struct cpu_record *cpu = &cpu_records[next];
        *cpu = (struct cpu_record){
            .index = next,
            .mpidr = id,
            .state = CPU_STARTING,
            .self = cpu,
            .exception_sp = (uintptr_t)(x86_exception_stacks[next] + ARCH_EXCEPTION_STACK_BYTES)};
        x86_cpu_boot[next] = CPU_BOOT_RECORD(id, cpu, (uintptr_t)cpu_stacks[next - 1]);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        /* The MP specification's start: INIT, 10 ms, then up to two
         * STARTUPs 200 us apart. */
        x86_send_ipi(id, X86_IPI_INIT);
        delay_us(10000);
        for (unsigned attempt = 0; attempt < 2 && load(&cpu->state) == CPU_STARTING; ++attempt) {
            x86_send_ipi(id, X86_IPI_STARTUP(X86_TRAMPOLINE >> 12));
            delay_us(200);
        }
        uint64_t start = time_us();
        for (unsigned polls = 0; load(&cpu->state) == CPU_STARTING && time_us() - start < 1000000;)
            if (!(++polls & 1023)) boot_progress_poll();
        if (load(&cpu->state) == CPU_IDLE) online_mask |= 1u << next;
        ++next; /* Late arrivals retain private stacks and are never reused. */
    }
}
