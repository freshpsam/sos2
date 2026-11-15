#ifndef SOS2_REDMI2_TOUCH_H
#define SOS2_REDMI2_TOUCH_H
#include "graphics.h"

/* WT88047's standard FT5x06-style report layout, I2C address 0x38. Hardware
 * identity must be checked before calling init. No firmware is programmed.
 * Initialization discards an inherited held touch until an all-up report.
 */
bool redmi2_touch_init(uint32_t width, uint32_t height);
bool redmi2_touch_poll(struct pointer_event *event);
/* True once per tap of Back, the right-hand capacitive key below the display,
 * after the redmi2_touch_poll call that saw it lift. A tap starts on Back with
 * nothing on the display and lifts there; one that slides away, meets a finger
 * on the display or loses reports is no tap. */
bool redmi2_touch_back(void);
/* True once as a finger lands on Back with nothing on the display, after the
 * redmi2_touch_poll call that saw it: the start of what may become a tap. */
bool redmi2_touch_back_pressed(void);
/* Ends any gesture and requires new valid all-up contact data before the next
 * one; the controller keeps scanning. */
void redmi2_touch_cancel(void);
/* Pause polling, hibernate the controller, then hold it in reset with its
 * interrupt line pulled down; its supply goes off while the chip sleeps
 * (rails.c). The caller must cancel its UI gesture. Resume pulls the line up
 * again, wakes the controller with a 5 ms reset pulse and reads nothing for
 * its 300 ms start, then requires new valid all-up contact data;
 * availability is retained. */
void redmi2_touch_suspend(void);
void redmi2_touch_resume(void);
#endif
