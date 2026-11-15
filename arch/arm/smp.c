#include "arm_smp.h"
#include "arm_machine.h"
#include "arm_cache.h"
#include "platform.h"
#include <stddef.h>
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_smp.h"
#include "redmi2_cpu_power.h"
#endif

struct arm_cpu arm_cpus[ARM_CPU_MAX];
unsigned char arm_cpu_stacks[ARM_CPU_MAX - 1][ARM_CPU_STACK_BYTES] __attribute__((aligned(4096)));

/* Fixed 32-byte assembly records, read with translation off on first entry. */
struct cpu_boot_record {
    uint32_t mpidr;
    struct arm_cpu *cpu;
    uintptr_t svc, irq, abt, und;
    uint32_t reserved[2];
};
struct cpu_boot_record arm_cpu_boot[ARM_CPU_MAX];
_Static_assert(sizeof(struct cpu_boot_record) == 32, "secondary boot assembly record");
_Static_assert(offsetof(struct arm_cpu, kernel_sp) == 0, "per-CPU continuation ABI");
_Static_assert(ARM_CPU_MAX == MAX_BOOT_CPUS && ARM_CPU_MAX == 4, "boot assembly CPU limit");
static unsigned online_mask = 1;

/* The ARMv7 barrier form serves both boards: the QEMU kernel targets the
 * Cortex-A15, which has no ARMv8 LDA/STL instructions. */
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

static struct arm_cpu *local(void) {
    struct arm_cpu *p;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(p));
    return p;
}

static uint32_t mpidr(void) {
    uint32_t n;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(n));
    return n & 0xffffff;
}

extern int32_t arm_psci_cpu_on(unsigned method, uint32_t function, uint32_t affinity,
                               uintptr_t entry);

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
    ++cpu->runs;
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

void arm_trap_dispatch(struct arm_trap *trap) {
    struct arm_cpu *cpu = local();
    if (!cpu || !cpu->active || (trap->context.cpsr & 31) != 16) {
        if (cpu && cpu->index) {
            cpu->trap = *trap;
            publish(&cpu->state, CPU_FAILED);
            for (;;) __asm__ volatile("wfe");
        }
        panic("exception in protected kernel");
    }
    arm_timer_stop(trap->vector == 6);
    arm_runtime_save(&cpu->runtime);
    cpu->trap = *trap;
}
#ifdef SOS2_BOARD_REDMI2
/* Standby power-down of idle secondary CPUs. A CPU the firmware keeps on
 * waits instead; either way it is ASLEEP meanwhile, STARTING while it
 * restarts and IDLE once awake. The wake re-sends SGI 0 to CPUs still ASLEEP:
 * one may have cleared the first just before powering down. A CPU not back
 * within 0.2 s leaves service, so CPU 0 runs pages once no worker remains. */
enum { SLEEP_ATTEMPTS = 8, WAKE_US = 200000, WAKE_RETRY_US = 10000 };

static uint32_t sleep_request;
static unsigned sleepers;

static void cpu_sleep(struct arm_cpu *cpu) {
    publish(&cpu->state, CPU_ASLEEP);
    arm_timer_stop(false);
    for (unsigned i = 0; i < SLEEP_ATTEMPTS && load(&sleep_request); ++i)
        redmi2_cpu_power_off(cpu->index);
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

void arm_smp_wake(void) {
    if (!load(&sleep_request)) return;
    publish(&sleep_request, 0);
    uint64_t start = time_us(), sent = 0;
    for (unsigned waiting = in_state(CPU_ASLEEP, CPU_STARTING); waiting;
         waiting = in_state(CPU_ASLEEP, CPU_STARTING)) {
        uint64_t now = time_us();
        if (now - start >= WAKE_US) {
            online_mask &= ~waiting;
            return;
        }
        unsigned asleep = in_state(CPU_ASLEEP, CPU_ASLEEP);
        if (asleep && (!sent || now - sent >= WAKE_RETRY_US)) {
            redmi2_cpu_power_wake(asleep);
            sent = now;
        }
    }
}
#endif
_Noreturn void arm_cpu_secondary(struct arm_cpu *cpu) {
    /* Also the warm entry after a standby power-down. A failed restart then
     * leaves the CPU asleep, out of service, rather than failing the system. */
    uint32_t failed = cpu->mmu_enabled ? CPU_ASLEEP : CPU_FAILED;
#ifdef SOS2_BOARD_REDMI2
    if (cpu->mmu_enabled) {
        publish(&cpu->state, CPU_STARTING);
        redmi2_cpu_power_resumed(cpu->index);
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
#ifdef SOS2_BOARD_REDMI2
        else if (state == CPU_IDLE && load(&sleep_request) && (sleepers & (1u << cpu->index)))
            cpu_sleep(cpu);
#endif
        else __asm__ volatile("wfe" ::: "memory");
    }
park:
    for (;;) __asm__ volatile("wfe");
}

void arm_smp_init(const struct platform_devices *devices) {
    uint32_t boot = mpidr();
    arm_cpus[0] =
        (struct arm_cpu){.index = 0, .mpidr = boot, .state = CPU_IDLE, .mmu_enabled = true};
    for (unsigned i = 0; i < ARM_CPU_MAX; ++i) arm_cpu_boot[i].mpidr = UINT32_MAX;
    unsigned next = 1;
    unsigned count = devices->psci_method && devices->psci_cpu_on ? devices->cpu_count : 0;
#ifdef SOS2_BOARD_REDMI2
    bool native = !count;
    if (native)
        count = redmi2_smp_prepare(boot, (uintptr_t)arm_secondary_entry, (uintptr_t)arm_warm_entry);
#endif
    for (unsigned i = 0; i < count && next < ARM_CPU_MAX; ++i) {
        uint32_t id = devices->cpu_mpidr[i];
#ifdef SOS2_BOARD_REDMI2
        if (native) id = i;
#endif
        if (id == boot) continue;
        struct arm_cpu *cpu = &arm_cpus[next];
        *cpu = (struct arm_cpu){.index = next, .mpidr = id, .state = CPU_STARTING};
        uintptr_t stack = (uintptr_t)arm_cpu_stacks[next - 1];
        arm_cpu_boot[next] = (struct cpu_boot_record){
            id,    cpu, stack + 2 * 4096, stack + 4 * 4096, stack + 6 * 4096, stack + 8 * 4096,
            {0, 0}};
        barrier();
        int32_t result;
#ifdef SOS2_BOARD_REDMI2
        if (native) result = redmi2_smp_release(id) ? 0 : -1;
        else
#endif
            result = arm_psci_cpu_on(devices->psci_method, devices->psci_cpu_on, id,
                                     (uintptr_t)arm_secondary_entry);
        if (!result) {
            uint64_t start = time_us();
            unsigned polls = 0;
            while (load(&cpu->state) == CPU_STARTING && time_us() - start < 1000000 &&
                   polls++ < 10000000)
                __asm__ volatile("yield");
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
}
