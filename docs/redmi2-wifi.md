# Redmi 2 Wi-Fi

The native WCN3620 (Pronto v2) driver passes Ethernet frames to lwIP.
Firmware startup, scanning, association, DHCP and browsing work on the
handset; router interoperability and long-running recovery still need
qualification (see the [roadmap](roadmap.md)).

## Firmware

`make phone` packages `wcnss.mdt`, its `wcnss.bXX` segments and the board
calibration `WCNSS_qcom_wlan_nv.bin` from `REDMI2_WIFI_FIRMWARE`
([packaging options](redmi2.md#packaging-options)). The packager checks the
signed MDT's layout; the secure monitor verifies its signatures at boot.

## Networks and profiles

**WI-FI → Scan Networks** lists up to 16 networks; pick one, enter its
password and tap **Connect**. The keyboard is ASCII, and without a saved
profile the radio stays idle until you scan.

- Supported: open, WPA2-Personal (CCMP) and WPA3-SAE, with no silent
  downgrade, on 2.4 GHz channels 1–11.
- Not supported: Enterprise, OWE, TKIP, hidden networks and roaming.
- A profile is saved only after it connects; a wrong password keeps the
  previous one. The saved network's page shows its password (**Show**) and
  offers **Forget**.
- A RAM session keeps the profile until reboot; persistent storage, an
  [installed](redmi2.md#installing) phone's included, keeps it in
  `/settings/wifi`, unencrypted, outside every profile.

### Power save

Standby leaves the network and puts the firmware in idle-mode power save, as
Linux's `wcn36xx` does; waking rejoins within a few seconds.

### Provisioning

An optional bootstrap profile can be built into the image. The tool prompts
for the password without echo and never overwrites a file:

```sh
python3 tools/redmi2_wifi.py --ssid 'browser lab' --security wpa3-sae \
  --output "$HOME/sos2-wifi.json"
make phone REDMI2_WIFI_PROFILE="$HOME/sos2-wifi.json"
```

The profile is imported once it connects and only when no desktop record
exists, so **Forget** survives reboot. Keep the profile and any image
containing it private.

## Driver

The stock
[WT88047 kernel](https://github.com/LineageOS/android_kernel_wingtech_msm8916/tree/beb5692ed25808aebce7b57e318cf8bd33e645d8)
(its WCNSS, prima/DXE and SCM sources) defines the board contract, and Linux
v6.12's [wcn36xx driver](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/ath/wcn36xx)
adds wire definitions; where they differ, the stock driver wins. The driver
loads the firmware through the secure monitor, talks to it over SMEM, SMP2P
and SMD, and drops its start-up power requests once the firmware is ready.
Radio work is polled on the kernel service worker, with no interrupts.
Authentication uses vendored hostap 2.12 and Mbed TLS 3.6.7 with no Linux
daemon.

Failures show their reason on the Wi-Fi screen; transport, DMA or entropy
failures need a reboot. When the firmware rejects a calibration upload
fragment after accepting the first (`status=00000005`), SOS2 restarts the
upload once.
