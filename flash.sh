#!/usr/bin/env bash
# Build + flash the RSVP Nano AMOLED firmware.
#
# Usage:
#   ./flash.sh                 # auto-detect port, build + upload amoled env
#   ./flash.sh -p /dev/cu.usbmodem1101   # explicit port
#   ./flash.sh -e amoled       # pick env (default: amoled)
#   ./flash.sh -v v0.1.0-amoled # stamp a clean firmware version
#   ./flash.sh -m              # build + upload, then open serial monitor
#
# NOTE: the amoled build uses the TinyUSB stack, so once it is running the
# device enumerates as a USB DRIVE, not a serial port. To re-flash, HOLD BOOT
# while plugging in to enter ROM download mode, then run this script.

set -euo pipefail

ENV="amoled"
PORT=""
MONITOR=0
VERSION=""

while getopts "e:p:v:mh" opt; do
  case "$opt" in
    e) ENV="$OPTARG" ;;
    p) PORT="$OPTARG" ;;
    v) VERSION="$OPTARG" ;;
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

serial_ports() {
  local ports=()
  shopt -s nullglob
  ports=(/dev/cu.usbmodem* /dev/cu.usbserial*)
  shopt -u nullglob
  if ((${#ports[@]})); then
    printf '%s\n' "${ports[@]}" | sort
  fi
}

wait_for_port() {
  local preferred="$1"
  local deadline=$((SECONDS + 12))
  local port=""

  while (( SECONDS < deadline )); do
    if [[ -n "$preferred" && -e "$preferred" ]]; then
      echo "$preferred"
      return 0
    fi

    port="$(serial_ports | head -n1 || true)"
    if [[ -n "$port" ]]; then
      echo "$port"
      return 0
    fi
    sleep 0.25
  done

  return 1
}

# Auto-detect the device port if not given (first usbmodem/usbserial).
if [[ -z "$PORT" ]]; then
  PORT="$(serial_ports | head -n1 || true)"
fi

UPLOAD_ARGS=()
if [[ -n "$PORT" ]]; then
  echo ">> port: $PORT"
else
  echo ">> port: auto (none detected; let esptool find it)"
fi

if [[ -n "$VERSION" ]]; then
  echo ">> version: $VERSION"
fi

echo ">> building env '$ENV'..."
if [[ -n "$VERSION" ]]; then
  RSVP_FIRMWARE_VERSION="$VERSION" "$PIO" run -e "$ENV"
else
  "$PIO" run -e "$ENV"
fi

UPLOAD_ARGS=()
if [[ -n "$PORT" ]]; then
  echo ">> rescanning port before upload..."
  refreshed_port="$(wait_for_port "$PORT" || true)"
  if [[ -n "$refreshed_port" ]]; then
    if [[ "$refreshed_port" != "$PORT" ]]; then
      echo ">> port changed: $PORT -> $refreshed_port"
      PORT="$refreshed_port"
    fi
    UPLOAD_ARGS=(--upload-port "$PORT")
  else
    echo ">> port disappeared; letting esptool auto-detect"
  fi
fi

echo ">> flashing env '$ENV'..."
if [[ -n "$VERSION" ]]; then
  RSVP_FIRMWARE_VERSION="$VERSION" "$PIO" run -e "$ENV" -t upload --disable-auto-clean "${UPLOAD_ARGS[@]}"
else
  "$PIO" run -e "$ENV" -t upload --disable-auto-clean "${UPLOAD_ARGS[@]}"
fi

echo ">> done. Cold-replug the device (unplug + replug) before judging the screen."

if [[ "$MONITOR" -eq 1 ]]; then
  echo ">> opening serial monitor (Ctrl-C to exit)..."
  if [[ -n "$PORT" ]]; then
    "$PIO" device monitor -p "$PORT" -b 115200
  else
    "$PIO" device monitor -b 115200
  fi
fi
