#ifndef SOS2_SMP_H
#define SOS2_SMP_H
#include "process.h"
#include "vm.h"
#include "arch/smp.h"
#define CPU_MAX         ARCH_CPU_MAX
#define CPU_STACK_BYTES (8 * 4096)

enum cpu_state { CPU_OFF, CPU_STARTING, CPU_IDLE, CPU_BUSY, CPU_DONE, CPU_FAILED, CPU_ASLEEP };

/* kernel_sp is the assembly ABI at offset zero, and on arm64 and x64
 * exception_sp, the top of the CPU's exception stack, at
 * ARCH_CPU_EXCEPTION_SP; on x64 self, the record's own address, follows at
 * ARCH_CPU_SELF (arch/x64/include/arch/smp.h). Mutable CPU/mailbox data is
 * shareable, uncached RAM on ARM. Only the boot CPU owns scheduler/driver
 * objects. */
struct cpu_record {
    uintptr_t kernel_sp;
#if defined(__aarch64__) || defined(__x86_64__)
    uintptr_t exception_sp;
#endif
#ifdef __x86_64__
    struct cpu_record *self;
#endif
    unsigned index, mpidr;
    uint32_t state;
    bool active, mmu_enabled;
    const struct vm_space *space;
    struct user_context context;
    struct user_runtime runtime;
    struct cpu_trap trap;
    uint32_t budget;
    uint64_t elapsed;
};
extern struct cpu_record cpu_records[CPU_MAX];
extern unsigned char cpu_stacks[CPU_MAX - 1][CPU_STACK_BYTES];
#ifdef __aarch64__
extern unsigned char arm_exception_stacks[CPU_MAX][ARCH_EXCEPTION_STACK_BYTES];
#endif
void smp_init(const struct platform_devices *);
unsigned smp_online(void); /* Bit mask; bit zero is always the boot CPU. */
bool cpu_idle(unsigned cpu);
void cpu_submit(unsigned cpu, const struct vm_space *, const struct user_context *,
                const struct user_runtime *, unsigned budget);
const struct cpu_record *cpu_result(unsigned cpu);
void cpu_release(unsigned cpu);
bool cpu_failed(unsigned cpu);
void arm_secondary_entry(void);
/* The firmware's warm-boot entry for every CPU (arch/arm/boot.S). */
void arm_warm_entry(void);
/* Saves CPU 0's context and calls power_down: 1 if that returns (the CPU
 * stayed on), 0 once the CPU powered down and resumed with its context. */
int arm_cpu0_suspend(void (*power_down)(void));
#ifdef SOS2_CPU_SLEEP
/* Standby: idle secondary CPUs power down (ASLEEP, never given work) until
 * arm_smp_wake, which waits up to 0.2 s for them to be IDLE again. A CPU not
 * back by then leaves service (CPU 0 runs pages once no worker remains). */
void arm_smp_sleep(void);
void arm_smp_wake(void);
#endif
#ifdef SOS2_BOARD_GTA7LITE
/* In standby, from CPU 0's loop: cuts the power of CPUs that have gone to
 * sleep since (boards/gta7lite/cpu_power.c). */
void arm_smp_standby_poll(void);
#endif
_Noreturn void arm_cpu_secondary(struct cpu_record *);
#endif
