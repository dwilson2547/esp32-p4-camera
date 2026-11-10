---
tier: project
domain: embedded
---

# ESP32-P4 Camera

An interchangeable-lens camera on the ESP32-P4: the Raspberry Pi HQ Camera (Sony IMX477, C/CS
mount) streaming into the P4's ISP, with a live preview on a Pi-protocol DSI touchscreen. Canon
EF lenses via an AF adapter are the next lens target.

The IMX477 driver here is new: no esp_cam_sensor driver for this sensor existed.

## Hardware

| Part | Detail | Status |
|---|---|---|
| Board | Waveshare ESP32-P4-Module-DEV-KIT (ESP32-P4 rev 3.2, 16 MB flash, 32 MB PSRAM) | ✅ verified |
| Camera | Raspberry Pi HQ Camera, board marked 2018 v1.0; IMX477 at I2C 0x1a, chip ID 0x0477 | ✅ verified |
| Display | OSOYOO 3.5" DSI V3 (Pi 7" touchscreen protocol: ATTINY-style MCU 0x45, FT5x06 touch 0x38), 800x480 | ✅ verified |
| Shared I2C | SDA GPIO 7, SCL GPIO 8; camera, display MCU and touch on one bus | ✅ verified |
| SD card | 8 GB card in the dev kit's slot | ⬜ not brought up |

## What works

- IMX477 mode: 1920x1520 RAW10, 2x2 binned full field, digitally cropped from 2028 wide to fit the P4
  datapath. 2 lanes at 900 Mbps, 38.5 fps measured from the sensor.
- Preview at 36.5 fps (measured): the PPA reads every 4th row (row stride trick) and runs
  non-blocking, overlapped with the next camera frame. A startup benchmark picks the row skip.
- Orientation: sensor-side horizontal mirror on by default (`CAMERA_IMX477_HMIRROR`), Bayer phase
  follows the flips. Vertical flip option off by default.
- ISP tuning (`components/esp_cam_sensor_imx477/cfg/imx477_default.json`): black level, AWB range,
  colour matrix and gain limit from Raspberry Pi's libcamera IMX477 tuning. AE uses the full 22.26x
  analog gain.
- Test patterns (colour bars, solid R/G/B/white) via `V4L2_CID_TEST_PATTERN`.

## Layout

```
components/esp_cam_sensor_imx477/   sensor driver (GPL-2.0), Kconfig, ISP tuning JSON
  tools/gen_settings.py             regenerates private_include/imx477_settings.h from the Linux driver
firmware/                           ESP-IDF app: camera -> ISP -> PPA -> display
scripts/flash.sh                    build, flash, monitor
```

## Build

ESP-IDF 6.x. `scripts/flash.sh [port]` builds, flashes and opens the monitor (default port
`/dev/ttyACM0`; set `IDF_EXPORT` to your IDF's `export.sh`). Dependencies come from the ESP
component registry on first build.

Regenerating the register tables needs the Raspberry Pi kernel's `drivers/media/i2c/imx477.c`:

```
python3 components/esp_cam_sensor_imx477/tools/gen_settings.py <path/to/imx477.c> \
    components/esp_cam_sensor_imx477/private_include/imx477_settings.h
```

## Licence

GPL-2.0-only (see `LICENSE`): the sensor tables derive from the Linux driver, and the display driver
dependency is GPL-2.0. Third-party sources and notices: `THIRD_PARTY.md`.

## Backlog

`TODO.md`.
