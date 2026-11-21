#ifndef SOS2_WIFI_DEVICE_H
#define SOS2_WIFI_DEVICE_H
#include "net_device.h"
#include "wifi_station.h"

/* The network device over a board's Wi-Fi radio (kernel/wifi/wifi_device.c),
 * on both boards: the desktop's Wi-Fi setup chooses networks and keeps
 * credentials, the station builds and parses 802.11 and runs WPA2/WPA3, and
 * lwIP stays on the main loop. The radio and the station run on the
 * serialized service worker; only bounded frame copies cross to lwIP. */
struct wifi_radio {
    /* Takes the hardware for this MAC: NULL, or why Wi-Fi is unavailable. */
    const char *(*start)(const unsigned char mac[6]);
    /* A service step while Wi-Fi runs: the radio brought up, then polled
     * around the station's steps below. Returns the microseconds to the next
     * step. */
    uint64_t (*service)(void);
    /* After a failure, on the worker: the hardware stopped and its buffers
     * wiped, before the station's state is dropped. */
    void (*stop)(void);
    /* Whether the radio passes frames, which the network waits for. */
    bool (*ready)(void);
    /* The status text while it is not ready; NULL for "Wi-Fi starting". */
    const char *(*starting)(void);
    /* The station's steps into the radio (struct wifi_ops). */
    int (*configure)(enum wifi_action, const struct wifi_station *, uint64_t now);
    bool (*send)(const void *frame, unsigned size, bool data);
    bool (*drained)(void);
    /* The radio cannot protect management frames (wifi_rsn_pmf). */
    bool no_pmf;
};

/* Resets the device with this radio and starts the desktop's Wi-Fi setup,
 * with the image's bootstrap profile when it carries one. */
void wifi_device_init(const struct wifi_radio *, struct net_device *out);

/* The board's radio code, during its service step. */
struct wifi_station *wifi_device_station(void);
const unsigned char *wifi_device_mac(void);
void wifi_device_fail(const char *why);
/* The station's randomness, seeded once a secured network is chosen: false
 * when that failed. */
bool wifi_device_seed(void);
/* With no station running, starts the one asked for: false when none runs. */
bool wifi_device_begin(uint64_t now);
/* A received 802.11 frame for the station. */
void wifi_device_receive(const unsigned char *frame, unsigned size,
                         const struct wifi_rx_status *status);
/* After the radio's own polling: network changes, standby, the station's
 * step, and the link. */
void wifi_device_poll(void);
/* In standby, whether the radio may sleep: the station has left its network
 * or has none, and every data frame has gone. */
bool wifi_device_quiet(void);

/* The device as the board's other code sees it. enabled: the radio taken,
 * failed or not; stopping: a failure's teardown still to run. */
struct wifi_device_view {
    bool enabled, failed, stopping, startup_complete;
    unsigned random_bytes; /* the network's randomness pool */
};
const struct wifi_device_view *wifi_device_view(void);

/* Standby keeps the station off its network and lets the radio sleep. */
void wifi_device_standby(bool on);
/* The network's randomness, from the pool the worker fills. */
size_t wifi_device_random(void *out, size_t size);
/* The status the Wi-Fi screen and the phone's loading screen show. */
const char *wifi_device_status(void);
#endif
