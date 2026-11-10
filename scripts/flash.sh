#!/usr/bin/env bash
# Build, flash and monitor the firmware.
#   scripts/flash.sh [port]          default port /dev/ttyACM0
# IDF_EXPORT points at ESP-IDF 6.x's export.sh. Conda is stripped from the
# environment because ESP-IDF's Python venv breaks under an active conda env.
set -euo pipefail
PORT="${1:-/dev/ttyACM0}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v6.0.3/export.sh}"
cd "$(dirname "$0")/../firmware"
exec env -u CONDA_PREFIX PATH=/usr/bin:/bin bash -c \
  ". '$IDF_EXPORT' >/dev/null 2>&1 && idf.py -p '$PORT' build flash monitor"
