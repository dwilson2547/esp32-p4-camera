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
| SD card (in use) | External SPI microSD breakout on the 40-pin header: CS GPIO23 (pin 7), CLK GPIO21 (pin 11), MOSI GPIO22 (pin 12), MISO GPIO20 (pin 13), 3V3 pin 1, GND pin 9. `SD_EXTERNAL_SPI 1` in `sd_card.h`. SA08G SDHC 8 GB at 20 MHz: 3x 8 MB multi-block write tests passed, 1.2-1.3 MB/s write, 1.7 MB/s read+verify | ✅ verified |
| Onboard TF slot | SDMMC slot 0 4-bit (CLK 43, CMD 44, D0-D3 39-42), card powered via P4 LDO VO4; no card-detect. Multi-block writes hang intermittently (card left waiting for data) on two cards, at 20 and 40 MHz; single-block writes work at 0.4 MB/s; reads 10.5 MB/s. Same signature as esp-idf #19361. Not used | ✅ verified (fault) |

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
- Shutter button (bottom right) takes a photo: hardware JPEG (1920x1520, q90, about 480 KB) saved
  as `/sdcard/DCIM/IMG_nnnn.JPG`, numbering continued from the card, white flash to confirm. Saved
  JPEG colours checked on a PC by the user: accurate.
- Shots queue in PSRAM (free PSRAM minus 2 MB, max 16 MB; 9.4 MB on this build) and are written in
  the background, so the shutter is free after the encode. Measured: 15 shots in bursts of 4-5 taps,
  ready again after 57-73 ms each, none dropped, about 0.42 s per photo to drain to the card; all
  files open on a PC. Button: white = ready, dark dot = still writing, grey = buffer full.
- SD card mounted at `/sdcard` at boot (never formats the card). Tap the top-left corner for an
  8 MB write/read-back test at 4, 16 and 64 KB write sizes.
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
