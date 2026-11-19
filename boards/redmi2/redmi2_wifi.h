#ifndef SOS2_REDMI2_WIFI_H
#define SOS2_REDMI2_WIFI_H
#include "net_device.h"
bool redmi2_wifi_init(const struct platform_devices *, struct net_device *);
/* Main-loop cancellation only flags failure; the worker owns cleanup. */
void redmi2_wifi_startup_timeout(void);
size_t redmi2_wifi_random(void *, size_t);
const char *redmi2_wifi_status(void);
/* How far the start the loading screen waits for has come: firmware start,
 * HAL, DMA and network randomness. Complete once it no longer waits. */
void redmi2_wifi_startup_progress(uint32_t *done, uint32_t *total);
/* Standby turns the radio off (idle-mode power save): the station leaves its
 * network first and rejoins it on waking; reconnection scans wait until then. */
void redmi2_wifi_standby(bool on);
/* Whole-chip sleep: request the sleep set that releases the crystal and core
 * rails Wi-Fi's start-up voted for, then the PMIC rails standby leaves idle
 * (rails.c); its state, 1 sent (or nothing voted:
 * Wi-Fi not started), 0 under way, -1 failed; and whether no RPM request is
 * waiting for its reply (one never answered keeps waiting). */
void redmi2_wifi_sleep_set(void);
int redmi2_wifi_sleep_set_state(void);
bool redmi2_wifi_rpm_idle(void);
/* Whether Wi-Fi's start-up sent this processor's bus-clock votes and turned
 * the RPM's clock scaling on (wcnss_boot.c): without it the RPM brings the
 * memory clock back from a whole-chip sleep at a third of its speed, so the
 * chip does not sleep that way. */
bool redmi2_wifi_clock_scaling(void);
/* Whether Wi-Fi needs no further prompt service in standby: not started,
 * failed, or its radio off with no change under way and no sleep set (with
 * the rails' requests, rails.c) going out. */
bool redmi2_wifi_standby_settled(void);
#endif
