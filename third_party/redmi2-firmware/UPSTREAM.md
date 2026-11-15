# Redmi 2 Wi-Fi firmware

Qualcomm WCNSS firmware for this project's Redmi 2 (WT88047), copied unmodified
from the handset: the `wcnss.mdt` image and its `wcnss.bXX` segments from the
`modem` partition (`/firmware/image`), and `WCNSS_qcom_wlan_nv.bin` from
`persist`. These are proprietary Qualcomm binaries, not covered by SOS2's
licenses. They are tracked so phone images can be rebuilt after `make clean`.

`make phone` packages this directory by default (`REDMI2_WIFI_FIRMWARE`). The
packager checks the file structure; the phone's secure monitor checks the
firmware signatures.

| File | SHA-256 |
| --- | --- |
| `wcnss.mdt` | `1271ae57fb0308a2633b2a357d12e1241d03c464281acbf992dc1c6cf4f71b34` |
| `wcnss.b00` | `6f0e3a923acc24ce94cdd6fabde1f513a5ea70c6d6ee6c52e51cee9c0b33cb57` |
| `wcnss.b01` | `1fa85efc0ae84e6fd8236fcb7bbb4e4ee86d0379bb5cadd9b5dae9d5adb6c67b` |
| `wcnss.b02` | `512fdefc4475525fa3e1a441e7074a10f18d9ca440ace0d529a422916ba269c4` |
| `wcnss.b04` | `88002d745b78835c85bcf736266e0fdc0e1b52d0ed870406118eaa41a5cce12b` |
| `wcnss.b06` | `778f292e6142a7c472bfdb8278824d6cb310852b2f793a31c43df3b0fdd0af92` |
| `wcnss.b09` | `7955cb2de90dd9efc6df9fdbf5f5d10c114f4135a9a6b52db1003be749e32f7a` |
| `wcnss.b10` | `ff6335069b6e140eb47149d847aea80bf7e2b06bd80ae9708aa382efb3ae21ee` |
| `wcnss.b11` | `6cf748ae0621e1a429062ce8eafc46dfc9d3c70e5f2664202f9d06bd398fc8f8` |
| `WCNSS_qcom_wlan_nv.bin` | `2ff8d78b3343aca845880107ea8812354f688d802681d5ab614cfffa5bfd5db4` |
