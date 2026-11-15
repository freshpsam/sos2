#ifndef SOS2_PHONE_INPUT_H
#define SOS2_PHONE_INPUT_H
#include "graphics.h"
/* Call only after validating the board and framebuffer. */
bool phone_input_init(uint32_t width, uint32_t height);

/* PHONE_INPUT_BACK_PRESS: a finger landed on the Back key below the display,
 * which is only for feedback, since the tap may not follow; PHONE_INPUT_BACK:
 * a tap of that key. The pointer event is then left unchanged. */
enum phone_input_source { PHONE_INPUT_TOUCH, PHONE_INPUT_BACK_PRESS, PHONE_INPUT_BACK };

bool phone_input_poll(struct pointer_event *, enum phone_input_source *);
/* Requires a fresh all-up report before the next touch, without touching the
 * controller. */
void phone_input_cancel(void);
/* For the screen going off and on: cancellation reaches the browser before
 * polling pauses and the touch controller hibernates; resume resets it and
 * requires a fresh all-up report (see redmi2_touch.h). */
void phone_input_suspend(struct pointer_event *cancel);
void phone_input_resume(void);
#endif
