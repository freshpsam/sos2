/* Wi-Fi on the Galaxy Tab A7 Lite: the connectivity subsystem (connsys.c)
 * and MediaTek's Wi-Fi firmware (wifi_radio.c) as the radio of the shared
 * Wi-Fi device (kernel/wifi/wifi_device.c). */
#include "gta7lite.h"
#include "wifi_radio.h"
#include "wifi_device.h"
#include "platform.h"
#include "kernel_io.h"

static bool radio_ready;

static const char *start(const unsigned char mac[6]) {
    (void)mac;
    return gta7lite_connsys_reserved() ? 0 : "Wi-Fi memory unavailable";
}

static void radio_poll(void) {
    if (!wifi_device_seed() || !wifi_device_begin(time_us())) return;
    if (gta7lite_wifi_radio_beacon_lost())
        wifi_station_beacon_missed(wifi_device_station(), time_us());
    gta7lite_wifi_radio_poll(wifi_device_receive);
    wifi_device_poll();
}

/* In standby, once the station has left its network or has none, the chip
 * goes to its firmware, which sleeps it, as the phone's radio idles; awake
 * otherwise. False while it sleeps: nothing polls it then. */
static bool radio_awake(void) {
    int state = gta7lite_wifi_radio_sleep(wifi_device_quiet());
    if (state < 0) wifi_device_fail("Wi-Fi did not wake");
    return !state;
}

static void yield(void) { kernel_io_wait(); }

static uint64_t service(void) {
    if (!radio_ready) {
        const char *error = gta7lite_connsys_start(yield);
        if (!error) error = gta7lite_wifi_radio_start(wifi_device_mac(), yield);
        if (error) wifi_device_fail(error);
        else radio_ready = true;
        return 2000;
    }
    if (!radio_awake()) return 250000;
    radio_poll();
    return 2000;
}

static bool ready(void) { return radio_ready; }

static const struct wifi_radio radio = {.start = start,
                                        .service = service,
                                        .ready = ready,
                                        .configure = gta7lite_wifi_radio_configure,
                                        .send = gta7lite_wifi_radio_send,
                                        .drained = gta7lite_wifi_radio_drained};

void gta7lite_wifi_init(struct net_device *out) {
    radio_ready = false;
    wifi_device_init(&radio, out);
}
