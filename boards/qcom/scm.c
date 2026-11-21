#include "qcom_scm.h"
#include "platform.h"
#include "kernel_io.h"

extern void arm_scm_invoke(uint32_t[8]);
#define invoke arm_scm_invoke
static unsigned convention;
/* A timed-out secure call may still own its buffer. Never reuse that memory
 * or re-enter PAS after an incomplete call, even if initialization is retried. */
static bool quarantined, ready;
/* A call in the secure world or yielded mid-call (interrupted, or polling a
 * legacy response): CPU power-down waits, as the vendor resumes an
 * interrupted call at once on the same CPU. */
static unsigned in_call; /* depth: a probe makes calls of its own */
static uint32_t legacy[1024] __attribute__((aligned(4096))), context_id;
static uint32_t extended[1024] __attribute__((aligned(4096)));
static char error[73];

const char *qcom_scm_error(void) { return error[0] ? error : 0; }

static void append(unsigned *n, const char *text) {
    while (*text && *n < sizeof(error) - 1) error[(*n)++] = *text++;
    error[*n] = 0;
}

static bool fail(const char *stage, const char *reason, uint32_t a, uint32_t b) {
    /* Preserve the original failure if best-effort PAS shutdown also fails. */
    if (!error[0]) {
        unsigned n = 0;
        append(&n, convention == 2 ? "SCM32 " : "SCM legacy ");
        append(&n, stage);
        append(&n, ": ");
        append(&n, reason);
        append(&n, " ");
        char values[18];
        for (unsigned i = 0; i < 8; ++i) {
            values[i] = "0123456789abcdef"[(a >> (28 - 4 * i)) & 15];
            values[9 + i] = "0123456789abcdef"[(b >> (28 - 4 * i)) & 15];
        }
        values[8] = '/';
        values[17] = 0;
        append(&n, values);
    }
    return false;
}

static bool exchange(const uint32_t original[8], uint32_t r[8], const char *stage, uint64_t start,
                     unsigned response) {
    unsigned interrupted = 0, busy = 0;
    memcpy(r, original, 8 * sizeof(*r));
    for (;;) {
        barrier();
        invoke(r);
        barrier();
        if (r[0] == 1) {
            /* Qualcomm SMCCC32 resumes with r0=1 and returned r6. Legacy SCM
             * instead resumes the original buffer/context-pointer request. */
            if (++interrupted == 64 || time_us() - start >= 1000000) {
                quarantined = true;
                return fail(stage, "interrupted", r[0], r[6]);
            }
            uint32_t resume = r[6];
            memcpy(r, original, 8 * sizeof(*r));
            r[0] = 1;
            if (convention == 2) r[6] = resume;
            kernel_io_wait();
            continue;
        }
        if ((int32_t)r[0] != -12 && (int32_t)r[0] != -55) return true;
        /* Busy is a completed refusal, not an in-flight operation. Retry the
         * original call with a fresh resume token, yielding during backoff. */
        if (++busy > 20 || time_us() - start >= 1000000) return fail(stage, "busy", r[0], r[1]);
        uint64_t wait = time_us();
        unsigned polls = 0;
        while (time_us() - wait < 30000 && polls++ < 1000000) kernel_io_wait();
        if (time_us() - wait < 30000) return fail(stage, "timer stalled", r[0], r[1]);
        /* Busy completed/refused the previous attempt. A new attempt must not
         * inherit its completion flag or status. Interrupted calls above are
         * still in flight: preserve their buffer and context while resuming. */
        if (response) {
            memset(legacy + response / 4, 0, 16);
            context_id = 0;
        }
        memcpy(r, original, 8 * sizeof(*r));
    }
}

static bool call_once(unsigned service, unsigned command, unsigned arguments, const uint32_t *args,
                      uint32_t *result, const char *stage) {
    unsigned count = arguments & 15;
    if (count > 6 || quarantined) return false;
    uint32_t original[8] = {0}, r[8];
    /* Legacy responses have a 12-byte header, plus an optional u32 result
     * for availability/PAS. Boot-address registration has no result payload.
     * See the wt88047 SCM ABI reference linked in docs/redmi2.md. */
    const unsigned header_size = 12, result_size = result ? sizeof(*result) : 0,
                   response_size = header_size + result_size;
    unsigned response = 16 + 4 * count;
    if (convention == 2) {
        original[0] = 0x02000000 | (service << 8) | command;
        original[1] = arguments;
        for (unsigned i = 0; i < count && i < 3; ++i) original[i + 2] = args[i];
        if (count == 4) original[5] = args[3];
        else if (count > 4) {
            /* SCM_ARGS(6): r5 points to a page-aligned SMC32 array beginning
             * at argument 3, not argument 4. Keep it alive across retries. */
            memset(extended, 0, sizeof(extended));
            for (unsigned i = 3; i < count; ++i) extended[i - 3] = args[i];
            original[5] = (uintptr_t)extended;
        }
    } else if (convention == 1) {
        memset(legacy, 0, sizeof(legacy));
        legacy[0] = response + response_size;
        legacy[1] = 16;
        legacy[2] = response;
        legacy[3] = (service << 10) | command;
        for (unsigned i = 0; i < count; ++i) legacy[4 + i] = args[i];
        /* Match the vendor and lk2nd: the entire reply starts at zero. The
         * handset can complete PAS with a 12/12 header and no status write.
         * A sentinel invents a failure in that case. Availability still needs
         * an explicit 1; PAS still needs a completed, valid reply and status 0. */
        context_id = 0;
        original[0] = 1;
        original[1] = (uintptr_t)&context_id;
        original[2] = (uintptr_t)legacy;
    } else return false;
    uint64_t start = time_us();
    if (!exchange(original, r, stage, start, convention == 1 ? response : 0)) return false;
    if (r[0]) return fail(stage, "call failed", r[0], r[1]);
    if (convention == 2) {
        if (result) *result = r[1];
        return true;
    }
    volatile uint32_t *header = legacy + response / 4;
    for (unsigned i = 0; i < 1000000; ++i) {
        barrier();
        if (header[2]) {
            barrier();
            /* scm_set_boot_addr[_mc] requests no response bytes. The stock
             * caller consumes only completion, not len/buf_offset, here. */
            if (!result) return true;
            unsigned offset = header[1], length = header[0];
            /* Redmi 2 firmware reports the header size (12) here; other
             * replies include the result (16). The vendor driver reads the
             * caller's requested size at buf_offset without using this len.
             * Bound that read by OUR declared response, never firmware len. */
            if (offset < header_size || (offset & 3) || offset > response_size - result_size ||
                (length != header_size && length != response_size)) {
                quarantined = true;
                return fail(stage, "invalid reply", length, offset);
            }
            if (result) *result = header[offset / 4];
            return true;
        }
        if (time_us() - start >= 1000000) break;
        kernel_io_wait();
    }
    quarantined = true;
    return fail(stage, "response timeout", header[0], header[1]);
}

static bool call(unsigned service, unsigned command, unsigned arguments, const uint32_t *args,
                 uint32_t *result, const char *stage) {
    ++in_call;
    bool ok = call_once(service, command, arguments, args, result, stage);
    --in_call;
    return ok;
}

static bool available(uint32_t function, const char *stage) {
    uint32_t result = 0;
    if (!call(6, 1, 1, &function, &result, stage)) return false;
    return result == 1 ||
           fail(stage, result ? "invalid result" : "not advertised", function, result);
}

static bool transport_probe(void);

static bool transport_init(void) {
    ++in_call;
    bool ok = transport_probe();
    --in_call;
    return ok;
}

static bool transport_probe(void) {
    if (quarantined) return false;
    convention = 2;
    ready = false;
    error[0] = 0;
    uint32_t probe[8] = {0x82000601, 1, 0x02000601, 0, 0, 0, 0, 0}, r[8];
    if (!exchange(probe, r, "probe", time_us(), 0)) return false;
    if (r[0] || r[1] != 1) {
        /* The upstream probe uses a standard-call target ID; the wt88047
         * vendor kernel probes the fast-call ID. Query both, using SMC32 only.
         * A failed/pending call never becomes evidence of PAS support. */
        probe[2] = 0x82000601;
        if (!exchange(probe, r, "fast probe", time_us(), 0)) return false;
    }
    if (r[0] || r[1] != 1) {
        convention = 1;
        if (!available((6u << 10) | 1, "probe")) return false;
    }
    return true;
}

bool qcom_scm_init(void) {
    if (!transport_init()) return false;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned commands[] = {1, 2, 5, 6};
        const char *const stages[] = {"metadata support", "memory support", "start support",
                                      "stop support"};
        uint32_t arg = convention == 2 ? 0x02000200 | commands[i] : (2u << 10) | commands[i];
        if (!available(arg, stages[i])) return false;
    }
    ready = true;
    return true;
}

/* Vendor scm.h/scm-boot.c: SCM_SVC_BOOT 1, SCM_BOOT_ADDR 1, SCM_BOOT_ADDR_MC 0x11.
 * Cold boot uses SCM_FLAG_COLDBOOT_MC (2) or the legacy SCM_FLAG_COLDBOOT_CPU1/2/3
 * (0x01, 0x08, 0x20); warm boot after a power collapse SCM_FLAG_WARMBOOT_MC (4)
 * or SCM_FLAG_WARMBOOT_CPU0/1/2/3 (0x04, 0x02, 0x10, 0x40), as the vendor's
 * platsmp.c and pm-boot.c register them. The HLOS flag is zero on ARM32. */
enum { SVC_BOOT = 1, BOOT_ADDR = 1, BOOT_ADDR_MC = 0x11, TERMINATE_PC = 2 };

static bool boot_address(uintptr_t entry, unsigned mask, bool warm) {
    if (!entry || entry > UINT32_MAX || (entry & 3) || !(mask & 14) || (mask & ~15u) ||
        !transport_init())
        return false;
    const char *stage = warm ? "CPU warm boot address" : "CPU boot address";
    uint32_t query = convention == 2 ? 0x02000000u | (SVC_BOOT << 8) | BOOT_ADDR_MC
                                     : (SVC_BOOT << 10) | BOOT_ADDR_MC,
             result = 0;
    if (!call(6, 1, 1, &query, &result, "CPU boot probe")) return false;
    if (result == 1) {
        /* ARM32 HLOS flag is zero; affinity masks describe this one cluster.
         * Boot registration has NO result payload, unlike PAS commands. */
        uint32_t args[] = {(uint32_t)entry, mask, 1, 1, UINT32_MAX, warm ? 4u : 2u};
        return call(SVC_BOOT, BOOT_ADDR_MC, 6, args, 0, stage);
    }
    if (result) return fail("CPU boot probe", "invalid result", query, result);
    /* The vendor's older boot-address command always uses the legacy ABI.
     * Restore the probed convention for later secure calls (GPU restore). */
    unsigned probed = convention;
    convention = 1;
    bool ok = available((SVC_BOOT << 10) | BOOT_ADDR, "CPU boot support");
    if (ok) {
        uint32_t flags =
            warm ? ((mask & 1) ? 0x04u : 0) | ((mask & 2) ? 0x02u : 0) | ((mask & 4) ? 0x10u : 0) |
                       ((mask & 8) ? 0x40u : 0)
                 : ((mask & 2) ? 0x01u : 0) | ((mask & 4) ? 0x08u : 0) | ((mask & 8) ? 0x20u : 0);
        uint32_t args[] = {flags, (uint32_t)entry};
        ok = call(SVC_BOOT, BOOT_ADDR, 2, args, 0, stage);
    }
    convention = probed;
    return ok;
}

bool qcom_scm_cpu_boot(uintptr_t entry, unsigned mask) { return boot_address(entry, mask, false); }

bool qcom_scm_cpu_warm_boot(uintptr_t entry, unsigned mask) {
    return boot_address(entry, mask, true);
}

/* Vendor msm-pm.c msm_pm_pc_hotplug and scm.c: SCM_CMD_TERMINATE_PC as an
 * atomic call, SCM_ATOMIC(1, 2, 1) = 0x402221 with a context pointer on the
 * legacy interface, or the fast SMC32 ID SCM_SIP_FNID(1, 2) | BIT(31) =
 * 0x82000102 with one argument on the ARM32 one. Registers only: no buffer, so
 * cores may call it together. Returns only if the core was not powered down. */
void qcom_scm_cpu_power_down(unsigned cpu, uint32_t flags) {
    static uint32_t context[4];
    if (!convention || in_call || quarantined) return;
    uint32_t r[8] = {0};
    if (convention == 2) {
        r[0] = 0x82000000u | (SVC_BOOT << 8) | TERMINATE_PC;
        r[1] = 1;
        r[2] = flags;
    } else {
        r[0] = ((((uint32_t)SVC_BOOT << 10) | TERMINATE_PC) << 12) | 0x200u | 0x20u | 1u;
        r[1] = (uint32_t)(uintptr_t)&context[cpu & 3];
        r[2] = flags;
    }
    barrier();
    invoke(r);
    barrier();
}

/* lk2nd's reboot_device (platform/msm_shared/reboot.c) before PS_HOLD drops:
 * SCM_IO_DISABLE_PMIC_ARBITER (service 9, command 1), if advertised, as an
 * atomic call with one zero argument: the fast SMC32 ID 0x82000901, or the
 * legacy SCM_ATOMIC(9, 1, 1) with a context pointer. Registers only. */
void qcom_scm_halt_pmic_arbiter(void) {
    static uint32_t context;
    if (in_call || quarantined || (!convention && !transport_init())) return;
    if (!available(convention == 2 ? 0x02000901u : (9u << 10) | 1u, "PMIC arbiter support")) return;
    uint32_t r[8] = {0};
    if (convention == 2) {
        r[0] = 0x82000901u;
        r[1] = 1;
    } else {
        r[0] = (((9u << 10) | 1u) << 12) | 0x200u | 0x20u | 1u;
        r[1] = (uint32_t)(uintptr_t)&context;
    }
    barrier();
    invoke(r);
    barrier();
}

static bool pas(unsigned command, unsigned info, uint32_t second, uint32_t third) {
    if (!ready || quarantined) return false;
    uint32_t args[] = {6, second, third}, result = UINT32_MAX;
    const char *stage = command == 1   ? "metadata"
                        : command == 2 ? "memory"
                        : command == 5 ? "start"
                                       : "stop";
    if (!call(2, command, info, args, &result, stage)) return false;
    return !result || fail(stage, "rejected", 0, result);
}

bool qcom_scm_gpu_restore(unsigned context_bank) {
    if (context_bank < 1 || context_bank > 2 || quarantined) return false;
    /* Reuse the serialized transport without resetting WCNSS PAS readiness. */
    if (!convention && !transport_init()) return false;
    uint32_t args[] = {18, context_bank}, result = UINT32_MAX;
    return call(0x0c, 2, 2, args, &result, "GPU memory protection") && result == 0;
}

bool qcom_scm_pas_init(uintptr_t metadata) {
    /* PAS may protect the metadata pages. Match the vendor's page-aligned,
     * uncached allocation and SCM_ARGS(2, SCM_VAL, SCM_RW), not SCM_RO. */
    return metadata && !(metadata & 4095) && metadata <= UINT32_MAX &&
           pas(1, 2 | (2u << 6), (uint32_t)metadata, 0);
}

bool qcom_scm_pas_memory(uintptr_t memory, unsigned size) {
    return memory && !(memory & 0xfffff) && size && memory <= UINT32_MAX - size &&
           pas(2, 3, (uint32_t)memory, size);
}

bool qcom_scm_pas_start(void) { return pas(5, 1, 0, 0); }

bool qcom_scm_pas_stop(void) { return pas(6, 1, 0, 0); }
