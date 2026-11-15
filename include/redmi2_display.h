#ifndef SOS2_REDMI2_DISPLAY_H
#define SOS2_REDMI2_DISPLAY_H
#include <stdbool.h>

/* Standby for the panel lk2nd leaves running in DSI video mode: MIPI DCS
 * commands put it to sleep and wake it, and the display pipeline below it
 * (timing generator, DSI controller, link clocks, PLL, PHY, MDSS clocks) is
 * turned off and rebuilt on waking. Init inspects the inherited setup once;
 * anything else (command mode, no software command trigger) leaves the panel
 * on, as does a panel that does not take the sleep commands, and a pipeline
 * that is not as lk2nd leaves it keeps running. Each call blocks for about a
 * third of a second. */
bool redmi2_display_init(void);
void redmi2_display_standby(bool off);
/* Whether the whole pipeline is off (the panel asleep, or a failed rebuild
 * torn down): only then may the chip sleep, which resets the DSI PHY. */
bool redmi2_display_off(void);
/* Whether the next wake restarts the panel from reset: this phone's R61308
 * is held there in standby, or a restart has yet to succeed. Only then may
 * its supply (L17) go off while the chip sleeps (rails.c). */
bool redmi2_display_reset_held(void);

#endif
