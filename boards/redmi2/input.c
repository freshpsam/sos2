#include "phone_input.h"
#include "redmi2_touch.h"
static bool ready, suspended;

bool phone_input_init(uint32_t width, uint32_t height) {
    suspended = false;
    ready = redmi2_touch_init(width, height);
    return ready;
}

void phone_input_suspend(struct pointer_event *cancel) {
    if (cancel) *cancel = (struct pointer_event){.cancelled = true};
    if (suspended) return;
    suspended = true;
    if (ready) redmi2_touch_suspend();
}

void phone_input_cancel(void) {
    if (ready && !suspended) redmi2_touch_cancel();
}

void phone_input_resume(void) {
    if (!suspended) return;
    if (ready) redmi2_touch_resume();
    suspended = false;
}

bool phone_input_poll(struct pointer_event *event, enum phone_input_source *source) {
    /* A failed driver may still owe the browser a gesture cancellation. */
    if (!event || !ready || suspended) return false;
    enum phone_input_source found = PHONE_INPUT_TOUCH;
    if (!redmi2_touch_poll(event)) {
        enum redmi2_touch_key tapped;
        if (redmi2_touch_key_pressed() != REDMI2_TOUCH_NO_KEY) found = PHONE_INPUT_KEY_PRESS;
        else if ((tapped = redmi2_touch_key_tapped()) != REDMI2_TOUCH_NO_KEY)
            found = tapped == REDMI2_TOUCH_HOME ? PHONE_INPUT_HOME : PHONE_INPUT_BACK;
        else return false;
    }
    if (source) *source = found;
    return true;
}
