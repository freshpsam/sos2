#include "pc.h"
#include "platform.h"

/* A PC's power button, ACPI's fixed one, polled: ACPI mode turned on (the
 * FADT's SMI command, as an ACPI system does at start) and the button's
 * event enabled (QEMU sets its status only then), then each press is
 * PWRBTN_STS in the PM1 status registers, cleared by writing it. The SCI
 * stays masked with every other interrupt. A button that only AML reports,
 * or a hardware-reduced machine's, is not seen.
 * https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html
 * (4.8.1, the PM1 event and control registers; 4.8.2.2.1, the power button) */
enum { PWRBTN_STS = 1 << 8, SCI_EN = 1 << 0 };

static uint16_t status_a, status_b;
static bool ready;

void acpi_power_start(const struct platform_devices *d) {
    ready = false;
    if (!d->fixed_power_button) return;
    if (d->pm1a_control && !(x86_inw(d->pm1a_control) & SCI_EN) && d->smi_command &&
        d->acpi_enable) {
        x86_outb(d->smi_command, d->acpi_enable);
        for (uint64_t deadline = time_us() + 3000000; !(x86_inw(d->pm1a_control) & SCI_EN);)
            if (time_us() > deadline) return;
    }
    status_a = d->pm1a_event;
    status_b = d->pm1b_event;
    /* PM1_EN follows PM1_STS, half the block on. */
    uint16_t enable = d->pm1_event_length / 2;
    x86_outw(status_a, PWRBTN_STS);
    x86_outw(status_a + enable, x86_inw(status_a + enable) | PWRBTN_STS);
    if (status_b) {
        x86_outw(status_b, PWRBTN_STS);
        x86_outw(status_b + enable, x86_inw(status_b + enable) | PWRBTN_STS);
    }
    ready = true;
}

bool acpi_power_pressed(void) {
    if (!ready) return false;
    bool pressed = x86_inw(status_a) & PWRBTN_STS;
    if (status_b && (x86_inw(status_b) & PWRBTN_STS)) pressed = true;
    if (pressed) {
        x86_outw(status_a, PWRBTN_STS);
        if (status_b) x86_outw(status_b, PWRBTN_STS);
    }
    return pressed;
}
