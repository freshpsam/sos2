#include "redmi2_rtc.h"
#include "platform.h"

/* PM8916 uses the PM8941 RTC layout: control 0x6046, little-endian seconds
 * 0x6048..0x604b. Android may maintain a separate epoch offset, so the raw
 * counter is never assumed to be Unix time. Read-only SPMI v2 / EE0.
 * Hardware descriptions: upstream drivers/rtc/rtc-pm8xxx.c and
 * drivers/spmi/spmi-pmic-arb.c, plus qcom/pm8916.dtsi. */
static uint32_t read32(uintptr_t p) {
    uint32_t v = mmio_read32(p);
    barrier();
    return v;
}

static void write32(uintptr_t p, uint32_t v) {
    barrier();
    mmio_write32(p, v);
    barrier();
}

enum { CORE = 0x0200f000, READ = 0x02c00000, STRIDE = 0x8000, APIDS = 128, PPID = 0x060 };

static bool get(unsigned apid, unsigned reg, unsigned bytes, uint32_t *value) {
    uintptr_t base = READ + apid * STRIDE;
    write32(base + 4, 0);
    write32(base, (1u << 27) | (reg << 4) | (bytes - 1));
    uint64_t start = time_us();
    for (unsigned n = 0; n < 100000; ++n) {
        uint32_t status = read32(base + 8);
        if (status & 0xe) return false;
        if (status & 1) {
            *value = read32(base + 0x18);
            return true;
        }
        if (time_us() - start >= 1000) break;
    }
    return false;
}

bool redmi2_rtc_read(uint32_t *counter) {
    if (!counter) return false;
    uint32_t version = read32(CORE);
    if (version < 0x20010000 || version >= 0x30000000) return false;
    unsigned apid = APIDS;
    for (unsigned n = 0; n < APIDS; ++n) {
        uint32_t entry = read32(CORE + 0x800 + 4 * n);
        if (entry && ((entry >> 8) & 0xfff) == PPID) {
            if (apid != APIDS) return false;
            apid = n;
        }
    }
    if (apid == APIDS) return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t control, seconds, lsb;
        if (!get(apid, 0x46, 1, &control)) return false;
        if (!(control & 0x80)) return false;
        if (!get(apid, 0x48, 4, &seconds) || !get(apid, 0x48, 1, &lsb)) return false;
        if ((lsb & 255) < (seconds & 255)) continue;
        if (!get(apid, 0x46, 1, &control)) return false;
        if (!(control & 0x80)) return false;
        *counter = seconds;
        return true;
    }
    return false;
}
