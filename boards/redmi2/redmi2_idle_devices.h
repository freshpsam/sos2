#ifndef SOS2_REDMI2_IDLE_DEVICES_H
#define SOS2_REDMI2_IDLE_DEVICES_H

/* Once at boot: devices SOS2 does not use, the motion sensor and USB among
 * them, put into their lowest-power state. */
void redmi2_idle_devices_init(void);
/* As each standby begins: the USB PHY reset and suspended again (with the
 * cable out). */
void redmi2_idle_devices_standby(void);
/* At each wake from standby, whose whole-chip sleeps may have cut the motion
 * sensor's supply (L17, rails.c): the poll, from the awake loop, puts it
 * back to sleep once it can answer, retrying a few times. */
void redmi2_idle_devices_wake(void);
void redmi2_idle_devices_poll(void);

#endif
