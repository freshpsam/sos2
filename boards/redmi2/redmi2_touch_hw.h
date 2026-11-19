#ifndef SOS2_REDMI2_TOUCH_HW_H
#define SOS2_REDMI2_TOUCH_HW_H

#include <stdbool.h>

/* WT88047 only: establish supplies, GPIOs and QUP5 clocks, reset and settle. */
bool redmi2_touch_hw_init(void);
/* Read-only, active-low IRQ sample; false until hardware initialization succeeds. */
bool redmi2_touch_irq_pending(void);
/* Drive the controller's reset line (GPIO12, active low) after a successful
 * initialization; false if it could not be set. */
bool redmi2_touch_hw_reset(bool asserted);
/* The interrupt line's pull (GPIO13): up while the controller runs, down in
 * standby, as the vendor's suspend pin state (ts_int_suspend) leaves it with
 * the controller in reset and its supply off while the chip sleeps; false
 * if not set. */
bool redmi2_touch_hw_interrupt_pull(bool up);

#endif
