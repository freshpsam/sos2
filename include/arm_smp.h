#ifndef SOS2_ARM_SMP_H
#define SOS2_ARM_SMP_H
#include "process.h"
#include "vm.h"
#define ARM_CPU_MAX         4
#define ARM_CPU_STACK_BYTES (8 * 4096)

enum arm_cpu_state { CPU_OFF, CPU_STARTING, CPU_IDLE, CPU_BUSY, CPU_DONE, CPU_FAILED, CPU_ASLEEP };

/* kernel_sp is the assembly ABI at offset zero. Mutable CPU/mailbox data is
 * shareable, uncached RAM. Only the boot CPU owns scheduler/driver objects. */
struct arm_cpu {
    uintptr_t kernel_sp;
    unsigned index, mpidr;
    uint32_t state;
    bool active, mmu_enabled;
    const struct vm_space *space;
    struct arm_context context;
    struct arm_runtime_context runtime;
    struct arm_trap trap;
    uint32_t budget;
    uint64_t elapsed, runs;
};
extern struct arm_cpu arm_cpus[ARM_CPU_MAX];
extern unsigned char arm_cpu_stacks[ARM_CPU_MAX - 1][ARM_CPU_STACK_BYTES];
void arm_smp_init(const struct platform_devices *);
unsigned arm_smp_online(void); /* Bit mask; bit zero is always the boot CPU. */
bool arm_cpu_idle(unsigned cpu);
void arm_cpu_submit(unsigned cpu, const struct vm_space *, const struct arm_context *,
                    const struct arm_runtime_context *, unsigned budget);
const struct arm_cpu *arm_cpu_result(unsigned cpu);
void arm_cpu_release(unsigned cpu);
bool arm_cpu_failed(unsigned cpu);
void arm_secondary_entry(void);
/* The firmware's warm-boot entry for every CPU (arch/arm/boot.S). */
void arm_warm_entry(void);
/* Saves CPU 0's context and calls power_down: 1 if that returns (the CPU
 * stayed on), 0 once the CPU powered down and resumed with its context. */
int arm_cpu0_suspend(void (*power_down)(void));
#ifdef SOS2_BOARD_REDMI2
/* Standby: idle secondary CPUs power down (ASLEEP, never given work) until
 * arm_smp_wake, which waits up to 0.2 s for them to be IDLE again. A CPU not
 * back by then leaves service (CPU 0 runs pages once no worker remains). */
void arm_smp_sleep(void);
void arm_smp_wake(void);
#endif
_Noreturn void arm_cpu_secondary(struct arm_cpu *);
#endif
