#!/usr/bin/env bash
# Build, flash, then monitor the firmware for a while.
#   scripts/flash.sh [port]          default port /dev/ttyACM0
#   MONITOR_SECS=60 scripts/flash.sh monitor for 60 s (default 30; 0 = until Ctrl-]).
# The monitor output is also saved to firmware/build/monitor.log.
# IDF_EXPORT points at ESP-IDF 6.x's export.sh. Conda is stripped from the
# environment because ESP-IDF's Python venv breaks under an active conda env.
set -euo pipefail
PORT="${1:-/dev/ttyACM0}"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v6.0.3/export.sh}"
MONITOR_SECS="${MONITOR_SECS:-30}"
cd "$(dirname "$0")/../firmware"
exec env -u CONDA_PREFIX PATH=/usr/bin:/bin MONITOR_SECS="$MONITOR_SECS" bash -c "
  . '$IDF_EXPORT' >/dev/null 2>&1 || exit 1
  idf.py -p '$PORT' build flash || exit 1
  if [ \"\$MONITOR_SECS\" = 0 ]; then
    idf.py -p '$PORT' monitor 2>&1 | tee build/monitor.log
  else
    echo \"--- monitoring for \$MONITOR_SECS s (log: firmware/build/monitor.log) ---\"
    timeout --foreground \"\$MONITOR_SECS\" idf.py -p '$PORT' monitor 2>&1 | tee build/monitor.log
    echo '--- monitor stopped ---'
  fi"
