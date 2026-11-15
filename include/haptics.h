#ifndef SOS2_HAPTICS_H
#define SOS2_HAPTICS_H
#include <stdbool.h>

/* Touch feedback: one short buzz of the phone's vibrator as a finger lands on
 * a button or key (QEMU has no vibrator: nothing happens). init follows the
 * phone's power driver. tap asks for a buzz; poll starts it and ends it on
 * time, and is true while it runs: the main loop waits a buzz out before
 * process_step. stop, for standby, ends one at once and drops a request;
 * quiet is false while a buzz runs or a failed stop waits to be tried again.
 * panic, from CPU 0 with a working timer, makes a last stop. */
void haptics_init(void);
void haptics_tap(void);
bool haptics_poll(void);
void haptics_stop(void);
bool haptics_quiet(void);
void haptics_panic(void);
#endif
