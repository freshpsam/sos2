#include "arm_smp.h"
#include "arm_machine.h"
#include "arm_cache.h"
#include "boot_progress.h"
#include "platform.h"
#include <stddef.h>
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_smp.h"
#include "redmi2_cpu_power.h"
#endif
#ifdef SOS2_BOARD_GTA7LITE
#include "gta7lite.h"
#endif
#ifdef SOS2_BOARD_NEXUS5
#include "nexus5.h"
#endif
/* The phones start their other cores themselves, through their firmware's
 * boot address and their power controllers, without PSCI. */
#ifdef SOS2_BOARD_REDMI2
#define native_prepare(boot) \
    redmi2_smp_prepare(boot, (uintptr_t)arm_secondary_entry, (uintptr_t)arm_warm_entry)
#define native_release redmi2_smp_release
#elif defined(SOS2_BOARD_NEXUS5)
#define native_prepare(boot) nexus5_smp_prepare(boot, (uintptr_t)arm_secondary_entry)
#define native_release       nexus5_smp_release
#endif

struct arm_cpu arm_cpus[ARM_CPU_MAX];
unsigned char arm_cpu_stacks[ARM_CPU_MAX - 1][ARM_CPU_STACK_BYTES] __attribute__((aligned(4096)));

struct cpu_boot_record arm_cpu_boot[ARM_CPU_MAX];
_Static_assert(sizeof(struct cpu_boot_record) == 32, "secondary boot assembly record");
_Static_assert(offsetof(struct arm_cpu, kernel_sp) == 0, "per-CPU continuation ABI");
#ifdef __aarch64__
unsigned char arm_exception_stacks[ARM_CPU_MAX][ARCH_EXCEPTION_STACK_BYTES]
    __attribute__((aligned(4096)));
_Static_assert(offsetof(struct arm_cpu, exception_sp) == ARCH_CPU_EXCEPTION_SP,
               "exception stack ABI");
#define EXCEPTION_SP(cpu) ((uintptr_t)(arm_exception_stacks[cpu] + ARCH_EXCEPTION_STACK_BYTES))
#endif
static unsigned online_mask = 1;

/* The ARMv7 barrier form serves every board: the ARM32 QEMU kernel targets the
 * Cortex-A15, which has no ARMv8 LDA/STL instructions, and AArch64 has it too. */
static uint32_t load(const uint32_t *p) {
    uint32_t value = __atomic_load_n(p, __ATOMIC_RELAXED);
    __asm__ volatile("dmb ish" ::: "memory");
    return value;
}

static void publish(uint32_t *p, uint32_t value) {
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(p, value, __ATOMIC_RELAXED);
    __asm__ volatile("dsb sy\n sev" ::: "memory");
}

unsigned arm_smp_online(void) { return online_mask; }

bool arm_cpu_idle(unsigned cpu) {
    return cpu < ARM_CPU_MAX && (online_mask & (1u << cpu)) &&
           load(&arm_cpus[cpu].state) == CPU_IDLE;
}

bool arm_cpu_failed(unsigned cpu) {
    return cpu < ARM_CPU_MAX && (online_mask & (1u << cpu)) &&
           load(&arm_cpus[cpu].state) == CPU_FAILED;
}

const struct arm_cpu *arm_cpu_result(unsigned cpu) {
    return cpu < ARM_CPU_MAX && load(&arm_cpus[cpu].state) == CPU_DONE ? &arm_cpus[cpu] : 0;
}

void arm_cpu_release(unsigned cpu) {
    if (!arm_cpu_result(cpu)) panic("CPU result released before completion");
    publish(&arm_cpus[cpu].state, CPU_IDLE);
}
/* Each CPU's last user space and the mapping epoch it ran at. */
static const struct vm_space *last_space[ARM_CPU_MAX];
static uint64_t last_epoch[ARM_CPU_MAX];

static void execute(struct arm_cpu *cpu) {
    unsigned id = (unsigned)(cpu - arm_cpus);
    uint64_t epoch = vm_epoch();
    /* Code can be replaced after a Close, or made executable by mprotect on
     * another core (both advance the epoch). Re-entering the same space at the
     * same epoch keeps its TLB entries, I-cache and branch history; anything
     * else flushes them, which also separates pages' branch predictor state. */
    bool flush = cpu->space != last_space[id] || epoch != last_epoch[id];
    vm_enter(cpu->space, flush);
    if (flush) arm_instruction_sync();
    arm_runtime_restore(&cpu->runtime);
    arm_timer_arm_us(cpu->budget);
    uint64_t start = time_us();
    cpu->active = true;
    arm_user_run(&cpu->context);
    cpu->active = false;
    cpu->elapsed = time_us() - start;
    vm_leave();
    last_space[id] = cpu->space;
    last_epoch[id] = epoch;
    /* DONE is also the acknowledgement that this CPU no longer uses the
     * process root. Only then may the boot CPU unmap or reclaim its pages. */
    publish(&cpu->state, CPU_DONE);
}

void arm_cpu_submit(unsigned id, const struct vm_space *space, const struct arm_context *context,
                    const struct arm_runtime_context *runtime, unsigned budget) {
    if (!arm_cpu_idle(id) || !space || !space->root || !budget || budget > 5000)
        panic("invalid CPU dispatch");
    struct arm_cpu *cpu = &arm_cpus[id];
    cpu->space = space;
    cpu->context = *context;
    cpu->runtime = *runtime;
    cpu->budget = budget;
    publish(&cpu->state, CPU_BUSY);
    if (!id) execute(cpu);
}

#ifndef __aarch64__
static char *hex(char *out, uint32_t value) {
    *out++ = '0';
    *out++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 15];
    *out = 0;
    return out;
}

/* The boot error screen's account of a kernel trap: its kind (process.S's
 * vector numbers), the trapped instruction, the caller's return address when
 * it came from SVC mode, the mode, and for an abort the faulting address and
 * the fault status, which locate it in kernel.elf. */
static const char *kernel_trap_text(const struct arm_trap *trap) {
    static const char *const kinds[] = {"reset", "undefined instruction", "SVC", "prefetch abort",
                                        "data abort", "reserved", "IRQ", "FIQ"};
    static char text[160];
    uint32_t lr = 0;
    if ((trap->context.cpsr & 31) == 0x13)
        __asm__ volatile("mrs r12, cpsr\n cps #0x13\n mov %0, lr\n msr cpsr_c, r12"
                         : "=r"(lr)
                         :
                         : "r12", "memory");
    char *p = stpcpy(text, "exception in protected kernel: ");
    p = stpcpy(p, trap->vector < 8 ? kinds[trap->vector] : "trap");
    p = hex(stpcpy(p, " at PC "), trap->context.pc);
    p = hex(stpcpy(p, ", LR "), lr);
    p = hex(stpcpy(p, ", CPSR "), trap->context.cpsr);
    if (trap->vector == 3 || trap->vector == 4) {
        p = hex(stpcpy(p, ", address "), trap->address);
        hex(stpcpy(p, ", status "), trap->status);
    }
    return text;
}
#endif

bool arm_trap_dispatch(struct arm_trap *trap) {
    struct arm_cpu *cpu = arch_cpu_local();
    if (!cpu || !cpu->active || !arch_trap_from_user(trap)) {
        if (cpu && cpu->index) {
            cpu->trap = *trap;
            publish(&cpu->state, CPU_FAILED);
            for (;;) __asm__ volatile("wfe");
        }
#ifdef __aarch64__
        panic("exception in protected kernel");
#else
        panic(kernel_trap_text(trap));
#endif
    }
#ifndef __aarch64__
    /* The JIT's instruction-cache sync needs this CPU's view of the
     * process (vm_icache_sync), and the JIT calls it for every code patch,
     * so it returns to user code without ending the slice. */
    if (trap->vector == 2 && trap->context.r[7] == SYS_ICACHE_SYNC) {
        trap->context.r[0] = (uint32_t)vm_icache_sync(cpu->space, trap->context.r[0],
                                                       trap->context.r[1]);
        return true;
    }
#endif
    arm_timer_stop(trap->vector == 6);
    arm_runtime_save(&cpu->runtime);
    cpu->trap = *trap;
    return false;
}
#if defined(SOS2_BOARD_REDMI2) || defined(SOS2_BOARD_GTA7LITE)
/* Standby power-down of idle secondary CPUs. A CPU the firmware keeps on
 * waits instead; either way it is ASLEEP meanwhile, STARTING while it
 * restarts and IDLE once awake. On the phone the wake re-sends SGI 0 to CPUs
 * still ASLEEP: one may have cleared the first just before powering down.
 * On the tablet a CPU leaves through PSCI's CPU_OFF and CPU 0 cuts its
 * power once PSCI reports it off, then restarts it with CPU_ON at the wake.
 * A CPU not back within 0.2 s leaves service, so CPU 0 runs pages once no
 * worker remains. */
enum { SLEEP_ATTEMPTS = 8, WAKE_US = 200000, WAKE_RETRY_US = 10000 };

static uint32_t sleep_request;
static unsigned sleepers;

static void cpu_sleep(struct arm_cpu *cpu) {
    publish(&cpu->state, CPU_ASLEEP);
    arm_timer_stop(false);
#ifdef SOS2_BOARD_REDMI2
    for (unsigned i = 0; i < SLEEP_ATTEMPTS && load(&sleep_request); ++i)
        redmi2_cpu_power_off(cpu->index);
#else
    gta7lite_cpu_off(); /* returns only when the firmware refuses */
#endif
    while (load(&sleep_request)) __asm__ volatile("wfe" ::: "memory");
    publish(&cpu->state, CPU_IDLE);
}

void arm_smp_sleep(void) {
    if (sleepers) publish(&sleep_request, 1);
}

static unsigned in_state(uint32_t a, uint32_t b) {
    unsigned mask = 0;
    for (unsigned i = 1; i < ARM_CPU_MAX; ++i) {
        uint32_t state = load(&arm_cpus[i].state);
        if ((online_mask & (1u << i)) && (state == a || state == b)) mask |= 1u << i;
    }
    return mask;
}

#ifdef SOS2_BOARD_GTA7LITE
void arm_smp_standby_poll(void) {
    if (!load(&sleep_request)) return;
    unsigned asleep = in_state(CPU_ASLEEP, CPU_ASLEEP) & sleepers;
    for (unsigned i = 1; i < ARM_CPU_MAX; ++i)
        if (asleep & (1u << i)) (void)gta7lite_cpu_cut(arm_cpus[i].mpidr);
}
#endif

void arm_smp_wake(void) {
    if (!load(&sleep_request)) return;
    publish(&sleep_request, 0);
    uint64_t start = time_us();
#ifdef SOS2_BOARD_REDMI2
    uint64_t sent = 0;
#else
    unsigned restarted = 0;
#endif
    for (unsigned waiting = in_state(CPU_ASLEEP, CPU_STARTING); waiting;
         waiting = in_state(CPU_ASLEEP, CPU_STARTING)) {
        uint64_t now = time_us();
        if (now - start >= WAKE_US) {
            online_mask &= ~waiting;
            return;
        }
        unsigned asleep = in_state(CPU_ASLEEP, CPU_ASLEEP);
#ifdef SOS2_BOARD_REDMI2
        if (asleep && (!sent || now - sent >= WAKE_RETRY_US)) {
            redmi2_cpu_power_wake(asleep);
            sent = now;
        }
#else
        /* A CPU the firmware refused to turn off wakes from its wait by
         * itself; one still on its way off is restarted once it is off. */
        for (unsigned i = 1; i < ARM_CPU_MAX; ++i)
            if ((asleep & ~restarted & (1u << i)) &&
                gta7lite_cpu_on(arm_cpus[i].mpidr, (uintptr_t)arm_secondary_entry))
                restarted |= 1u << i;
#endif
    }
}
#endif
_Noreturn void arm_cpu_secondary(struct arm_cpu *cpu) {
    /* Also the warm entry after a standby power-down. A failed restart then
     * leaves the CPU asleep, out of service, rather than failing the system. */
    uint32_t failed = cpu->mmu_enabled ? CPU_ASLEEP : CPU_FAILED;
#if defined(SOS2_BOARD_REDMI2) || defined(SOS2_BOARD_GTA7LITE)
    if (cpu->mmu_enabled) {
        publish(&cpu->state, CPU_STARTING);
#ifdef SOS2_BOARD_REDMI2
        redmi2_cpu_power_resumed(cpu->index);
#endif
    }
#endif
    if (!vm_secondary_init()) {
        publish(&cpu->state, failed);
        goto park;
    }
    cpu->mmu_enabled = true;
    if (!arm_timer_secondary_init()) {
        publish(&cpu->state, failed);
        goto park;
    }
    /* TLBs, caches and branch history start empty after power-up. */
    last_space[cpu->index] = 0;
    arm_runtime_init();
    publish(&cpu->state, CPU_IDLE);
    for (;;) {
        uint32_t state = load(&cpu->state);
        if (state == CPU_BUSY) execute(cpu);
#if defined(SOS2_BOARD_REDMI2) || defined(SOS2_BOARD_GTA7LITE)
        else if (state == CPU_IDLE && load(&sleep_request) && (sleepers & (1u << cpu->index)))
            cpu_sleep(cpu);
#endif
        else __asm__ volatile("wfe" ::: "memory");
    }
park:
    for (;;) __asm__ volatile("wfe");
}

void arm_smp_init(const struct platform_devices *devices) {
    uint32_t boot = arch_cpu_affinity();
    arm_cpus[0] =
        (struct arm_cpu){.index = 0, .mpidr = boot, .state = CPU_IDLE, .mmu_enabled = true};
#ifdef __aarch64__
    arm_cpus[0].exception_sp = EXCEPTION_SP(0);
#endif
    for (unsigned i = 0; i < ARM_CPU_MAX; ++i) arm_cpu_boot[i].mpidr = UINT32_MAX;
    unsigned next = 1;
    unsigned count = devices->psci_method && devices->psci_cpu_on ? devices->cpu_count : 0;
#ifdef native_prepare
    bool native = !count;
    if (native) count = native_prepare(boot);
#endif
    for (unsigned i = 0; i < count && next < ARM_CPU_MAX; ++i) {
        uint32_t id = devices->cpu_mpidr[i];
#ifdef native_prepare
        if (native) id = i;
#endif
        if (id == boot) continue;
        struct arm_cpu *cpu = &arm_cpus[next];
        *cpu = (struct arm_cpu){.index = next, .mpidr = id, .state = CPU_STARTING};
#ifdef __aarch64__
        cpu->exception_sp = EXCEPTION_SP(next);
#endif
        uintptr_t stack = (uintptr_t)arm_cpu_stacks[next - 1];
        arm_cpu_boot[next] = CPU_BOOT_RECORD(id, cpu, stack);
        barrier();
        int32_t result;
#ifdef native_prepare
        if (native) result = native_release(id) ? 0 : -1;
        else
#endif
            result = arm_psci_call(devices->psci_method, devices->psci_cpu_on, id,
                                   (uintptr_t)arm_secondary_entry);
        if (!result) {
            uint64_t start = time_us();
            unsigned polls = 0;
            while (load(&cpu->state) == CPU_STARTING && time_us() - start < 1000000 &&
                   polls++ < 10000000) {
                /* Up to 1 s per CPU: the tablet's watchdog is fed meanwhile. */
                if (!(polls & 1023)) boot_progress_poll();
                __asm__ volatile("yield");
            }
            if (load(&cpu->state) == CPU_IDLE) online_mask |= 1u << next;
        }
        ++next; /* Late arrivals retain private stacks and are never reused. */
    }
#ifdef SOS2_BOARD_REDMI2
    if (native) {
        unsigned ready = redmi2_cpu_power_init(online_mask & redmi2_smp_warm_mask());
        sleepers = ready & ~1u;
        if (ready & 1) (void)redmi2_cpu0_power_init();
    }
#endif
#ifdef SOS2_BOARD_GTA7LITE
    if (devices->psci_method == CPU_BOOT_SMC) sleepers = online_mask & ~1u;
#endif
}
