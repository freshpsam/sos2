#ifndef SOS2_QCOM_SCM_H
#define SOS2_QCOM_SCM_H
#include <stdint.h>
#include <stdbool.h>
/* Qualcomm's secure monitor calls (scm.c), the Redmi 2's and the Nexus 5's:
 * the SMC32 convention where the firmware has it, else the legacy one.
 * Secure monitor calls are privileged and serialized. An individual SMC is
 * trusted firmware execution; software cannot impose a deadline inside it.
 * init probes the convention and the PAS (peripheral image) calls, which
 * the Redmi 2's Wi-Fi and the Nexus 5's audio DSP use. */
bool qcom_scm_init(void);
/* Cold-boot entry for the CPUs of the single cluster (cpu_mask: bit n is
 * CPU n). Independent of WCNSS PAS. */
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

/* A peripheral's image, by its PAS ID (scm-pas.h): the audio DSP's
 * (QDSP6 in LPASS) or WCNSS's. Metadata must occupy dedicated, contiguous,
 * uncached 4 KiB-aligned pages. */
enum { QCOM_PAS_ADSP = 1, QCOM_PAS_WCNSS = 6 };

bool qcom_scm_pas_init(unsigned peripheral, uintptr_t metadata);
bool qcom_scm_pas_memory(unsigned peripheral, uintptr_t memory, unsigned size);
bool qcom_scm_pas_start(unsigned peripheral);
bool qcom_scm_pas_stop(unsigned peripheral);
#endif
