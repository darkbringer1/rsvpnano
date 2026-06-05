#!/usr/bin/env bash
# Build + flash the RSVP Nano AMOLED firmware.
#
# Usage:
#   ./flash.sh                 # auto-detect port, build + upload amoled env
#   ./flash.sh -p /dev/cu.usbmodem1101   # explicit port
#   ./flash.sh -e amoled       # pick env (default: amoled)
#   ./flash.sh -m              # build + upload, then open serial monitor
#
# NOTE: the amoled build uses the TinyUSB stack, so once it is running the
# device enumerates as a USB DRIVE, not a serial port. To re-flash, HOLD BOOT
# while plugging in to enter ROM download mode, then run this script.

set -euo pipefail

ENV="amoled"
PORT=""
MONITOR=0

while getopts "e:p:mh" opt; do
  case "$opt" in
    e) ENV="$OPTARG" ;;
    p) PORT="$OPTARG" ;;
    m) MONITOR=1 ;;
    h) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option" >&2; exit 1 ;;
  esac
done

# Locate pio (PATH, then the known pip install location).
PIO="$(command -v pio || true)"
if [[ -z "$PIO" ]]; then
  PIO="$HOME/Library/Python/3.9/bin/pio"
fi
if [[ ! -x "$PIO" ]]; then
  echo "error: pio not found (install PlatformIO or add it to PATH)" >&2
  exit 1
fi

# Auto-detect the device port if not given (first usbmodem/usbserial).
if [[ -z "$PORT" ]]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -n1 || true)"
fi

UPLOAD_ARGS=()
if [[ -n "$PORT" ]]; then
  echo ">> port: $PORT"
  UPLOAD_ARGS=(--upload-port "$PORT")
else
  echo ">> port: auto (none detected; let esptool find it)"
fi

echo ">> building + flashing env '$ENV'..."
"$PIO" run -e "$ENV" -t upload "${UPLOAD_ARGS[@]}"

echo ">> done. Cold-replug the device (unplug + replug) before judging the screen."

if [[ "$MONITOR" -eq 1 ]]; then
  echo ">> opening serial monitor (Ctrl-C to exit)..."
  if [[ -n "$PORT" ]]; then
    "$PIO" device monitor -p "$PORT" -b 115200
  else
    "$PIO" device monitor -b 115200
  fi
fi
