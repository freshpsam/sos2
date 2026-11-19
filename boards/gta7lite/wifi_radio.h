#ifndef SOS2_GTA7LITE_WIFI_RADIO_H
#define SOS2_GTA7LITE_WIFI_RADIO_H
#include "wifi_station.h"

/* MediaTek's Wi-Fi firmware (CONNAC1 "soc1_0") over its DMA rings, as the
 * gen4m driver runs it on AXI (wifi_radio.c). The connectivity subsystem must
 * be up first (connsys.c). */
const char *gta7lite_wifi_radio_start(const unsigned char mac[6], void (*yield)(void));
/* The station's steps (wifi_ops.configure): 0 pending, 1 done, -1 failed. */
int gta7lite_wifi_radio_configure(enum wifi_action, const struct wifi_station *, uint64_t now);
/* An 802.11 frame from the station: management frames as they are, data
 * frames (a 24-byte header and LLC/SNAP) in the hardware's Ethernet form. */
bool gta7lite_wifi_radio_send(const void *frame, unsigned size, bool data);
/* Received frames for the station: management frames as they arrive, data
 * frames rebuilt from the hardware's Ethernet form. */
void gta7lite_wifi_radio_poll(void (*deliver)(const unsigned char *, unsigned, const struct wifi_rx_status *));
/* No data frame left to send (the station installs keys only then). */
bool gta7lite_wifi_radio_drained(void);
/* The firmware's beacon-timeout report since the last call. */
bool gta7lite_wifi_radio_beacon_lost(void);
/* The station whose rates EAPOL frames go at. */
void gta7lite_wifi_radio_station(const struct wifi_station *);
/* The chip handed to its firmware, which sleeps it, or taken back: 1 asleep,
 * 0 awake, -1 when it did not come back within 2 s. Nothing else touches the
 * chip while it sleeps. */
int gta7lite_wifi_radio_sleep(bool sleep);
#endif
