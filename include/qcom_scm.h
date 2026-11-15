#ifndef SOS2_QCOM_SCM_H
#define SOS2_QCOM_SCM_H
#include <stdint.h>
#include <stdbool.h>
/* Secure monitor calls are privileged and serialized. An individual SMC is
 * trusted firmware execution; software cannot impose a deadline inside it. */
bool qcom_scm_init(void);
/* Cold-boot entry for the MSM8916 single cluster. Independent of WCNSS PAS. */
bool qcom_scm_cpu_boot(uintptr_t entry, unsigned cpu_mask);
/* The entry the firmware takes when a CPU in cpu_mask powers up after a power
 * collapse. Register it with the cold-boot entry, before the PAS setup. */
bool qcom_scm_cpu_warm_boot(uintptr_t entry, unsigned cpu_mask);
/* On the calling CPU: power it down (TERMINATE_PC with these flags). Returns
 * only if the firmware kept it on, e.g. for a pending interrupt, or at once
 * while another call is under way or after one that did not complete. */
void qcom_scm_cpu_power_down(unsigned cpu, uint32_t flags);
/* Restore the MSM8916 GPU's two nonsecure SMMU context banks only. */
bool qcom_scm_gpu_restore(unsigned context_bank);
/* Just before PS_HOLD drops for a restart: the firmware stops the PMIC
 * arbiter, if it offers that, so no PMIC transfer is cut off. No PMIC access
 * works afterwards. */
void qcom_scm_halt_pmic_arbiter(void);
/* First failure is retained across cleanup calls, until the next safe init. */
const char *qcom_scm_error(void);
/* Metadata must occupy dedicated, contiguous, uncached 4 KiB-aligned pages. */
bool qcom_scm_pas_init(uintptr_t metadata);
bool qcom_scm_pas_memory(uintptr_t memory, unsigned size);
bool qcom_scm_pas_start(void);
bool qcom_scm_pas_stop(void);
#endif
