#!/usr/bin/env bash
# RSVP Nano installer / dev console.
#
#   ./dev.sh            interactive menu (build / flash / monitor / ...)
#   ./dev.sh flash      jump straight to the guided flash
#   ./dev.sh doctor     environment diagnosis
#
# The guided flash detects connected Espressif boards by USB identity, lets you
# pick when several are plugged in, probes the chip with esptool before writing,
# and walks you into BOOT download mode (with live wait) if the board does not
# answer. Everything here also exists as a plain make target.

set -uo pipefail
cd "$(dirname "$0")"

# ---- persisted prefs ------------------------------------------------------
CONF=".dev.local"
ENV_FROM_MAKE="${ENV-}"; PORT_FROM_MAKE="${PORT-}"; VERSION_FROM_MAKE="${VERSION-}"
ENV="amoled"; PORT="auto"; VERSION=""
# shellcheck disable=SC1090
[[ -f "$CONF" ]] && source "$CONF"
[[ -n "$ENV_FROM_MAKE" ]] && ENV="$ENV_FROM_MAKE"
[[ -n "$PORT_FROM_MAKE" ]] && PORT="$PORT_FROM_MAKE"
[[ -n "$VERSION_FROM_MAKE" ]] && VERSION="$VERSION_FROM_MAKE"
save_conf() { printf 'ENV=%q\nPORT=%q\nVERSION=%q\n' "$ENV" "$PORT" "$VERSION" >"$CONF"; }

VALID_ENVS=(amoled waveshare_esp32s3_usb_msc native_test)
ESPRESSIF_VID="0x303a"

# ---- palette --------------------------------------------------------------
r=$'\033[0m'; b=$'\033[1m'; dim=$'\033[2m'; rev=$'\033[7m'
grn=$'\033[32m'; yel=$'\033[33m'; red=$'\033[31m'; cyn=$'\033[36m'

info()  { printf "  %s\n" "$1"; }
ok()    { printf "  %s✓%s %s\n" "$grn" "$r" "$1"; }
warn()  { printf "  %s!%s %s\n" "$yel" "$r" "$1"; }
err()   { printf "  %s✗%s %s\n" "$red" "$r" "$1"; }
pause() { read -rsn1 -p $'\n'"  ${dim}press any key to continue...${r}" _ || true; printf "\n"; }

screen_clear() { printf '\033[2J\033[H'; }
section() { printf "\n%s%s%s\n" "$b" "$1" "$r"; }
status_row() { printf "  %s%-11s%s %s\n" "$dim" "$1" "$r" "$2"; }
action_row() {
  printf "  %s[%s]%s %-18s  %s[%s]%s %s\n" \
    "$cyn" "$1" "$r" "$2" "$cyn" "$3" "$r" "$4"
}

# ---- tool discovery -------------------------------------------------------
find_pio() {
  local p; p="$(command -v pio || true)"
  [[ -z "$p" ]] && p="$HOME/Library/Python/3.9/bin/pio"
  if [[ -x "$p" ]]; then echo "$p"; else echo "python3 -m platformio"; fi
}
find_esptool() {
  local p; p="$(command -v esptool.py || command -v esptool || true)"
  [[ -z "$p" ]] && p="$HOME/Library/Python/3.9/bin/esptool.py"
  if [[ -x "$p" ]]; then echo "$p"; else echo "python3 -m esptool"; fi
}
PIO="$(find_pio)"; ESPTOOL="$(find_esptool)"

# ---- USB device enumeration (macOS ioreg) ---------------------------------
# Emits, one per line:  <port>\t<vid_hex>\t<product name>
usb_list() {
  python3 - <<'PY' 2>/dev/null
import subprocess, glob, re
raw = subprocess.run(["ioreg","-p","IOUSB","-l","-w","0"],
                     capture_output=True, text=True).stdout
blocks=[]; cur={}
for line in raw.splitlines():
    if "+-o " in line:
        if cur: blocks.append(cur)
        cur={}
    for key,pat in (("vid",r'"idVendor"\s*=\s*(\d+)'),
                    ("pid",r'"idProduct"\s*=\s*(\d+)')):
        m=re.search(pat,line)
        if m: cur[key]=int(m.group(1))
    m=re.search(r'"USB Product Name"\s*=\s*"(.*)"',line)
    if m: cur["name"]=m.group(1)
    m=re.search(r'"USB Serial Number"\s*=\s*"(.*)"',line)
    if m: cur["serial"]=m.group(1)
if cur: blocks.append(cur)

ports=sorted(glob.glob("/dev/cu.usbmodem*")+glob.glob("/dev/cu.usbserial*"))
for p in ports:
    suf=p.rsplit("usbmodem",1)[-1].rsplit("usbserial",1)[-1]
    hit=next((x for x in blocks if x.get("serial") and suf.startswith(x["serial"])), None)
    if hit: print(f"{p}\t{hit.get('vid',0):#06x}\t{hit.get('name','?').strip()}")
    else:   print(f"{p}\t?\tunknown serial device")
PY
}

# Espressif ports only (port paths, newline separated).
esp_ports() { usb_list | awk -F'\t' -v v="$ESPRESSIF_VID" '$2==v {print $1}'; }

short() { local s="${1##*/}"; s="${s#cu.}"; [[ ${#s} -gt 22 ]] && s="…${s: -20}"; echo "$s"; }
name_for() { usb_list | awk -F'\t' -v p="$1" '$1==p {print $3; exit}'; }

# Product string -> compact label, e.g. "ESP32-S3 16M".
friendly() {
  local n="${1//_/ }"
  if [[ "$n" == *ESP32*S3* ]]; then
    local mem=""; [[ "$n" == *16M* ]] && mem=" 16M"; [[ "$n" == *8M* ]] && mem=" 8M"
    echo "ESP32-S3${mem}"
  elif [[ -z "$n" || "$n" == "?" ]]; then echo "unknown"
  else echo "$n"; fi
}

resolved_port() {
  if [[ "$PORT" == "auto" ]]; then serial_ports | head -n1; else echo "$PORT"; fi
}

serial_ports() {
  ls -1 /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | sort
}

port_present() {
  local port="$1"
  serial_ports | grep -Fxq "$port"
}

read_ports() {
  ports=()
  local port
  while IFS= read -r port; do
    [[ -n "$port" ]] && ports+=("$port")
  done < <(serial_ports)
}

# ---- esptool probe + guided wait ------------------------------------------
# Portable timeout (macOS has no coreutils `timeout`). Kills the command if it
# overruns and returns 124, so a hung esptool can never freeze the installer.
run_timeout() {  # run_timeout <secs> <cmd...>
  local secs="$1"; shift
  "$@" & local pid=$!
  ( sleep "$secs"; kill -9 "$pid" 2>/dev/null ) & local watcher=$!
  wait "$pid" 2>/dev/null; local rc=$?
  kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
  return $rc
}

probe() {  # probe <port> -> 0 if esptool can talk to an ESP32-S3 (bounded)
  local port="$1"
  run_timeout 12 $ESPTOOL --port "$port" --before default_reset \
    --connect-attempts 1 chip_id >/dev/null 2>&1
}

# Probe with a spinner so the user never sees a frozen "Probing..." line.
probe_spinner() {  # probe_spinner <port> -> 0 reachable
  local port="$1" i=0
  printf "  %schecking if the board is in flash mode...%s\n" "$dim" "$r"
  probe "$port" & local pid=$!
  while kill -0 "$pid" 2>/dev/null; do
    i=$(((i+1)%10))
    printf "\r  %s%s%s talking to board... " "$cyn" "${spin:$i:1}" "$r"
    sleep 0.2
  done
  wait "$pid"; local rc=$?
  if [[ $rc -eq 0 ]]; then printf "\r  %s✓%s board is in flash mode.       \n" "$grn" "$r"
  else printf "\r  %s!%s board is running its app, not flash mode.\n" "$yel" "$r"; fi
  return $rc
}

spin='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
wait_for_device() {  # guided BOOT-hold loop; echoes a reachable port or empty
  printf "\n  %s%sThis board can't be flashed while its app is running.%s\n" "$b" "$yel" "$r"
  printf "  %sYou need to put it into flash (download) mode by hand:%s\n\n" "$dim" "$r"
  printf "    %s1.%s Press and %sHOLD the BOOT button%s\n" "$b" "$r" "$b" "$r"
  printf "    %s2.%s Keep holding it, unplug then replug the USB cable\n" "$b" "$r"
  printf "    %s3.%s Now release BOOT\n\n" "$b" "$r"
  printf "  %sThe moment it reconnects I'll detect it and continue.%s\n" "$dim" "$r"
  printf "  %spress c to cancel%s\n\n" "$dim" "$r"
  local i=0 key port
  while true; do
    i=$(((i+1)%10))
    printf "\r  %s%s%s waiting for you to do the BOOT-replug... " "$cyn" "${spin:$i:1}" "$r"
    read -rsn1 -t 1 key && [[ "$key" == "c" ]] && { printf "\r%-60s\r" " "; return 1; }
    port="$(esp_ports | head -n1)"
    if [[ -n "$port" ]] && probe "$port"; then
      printf "\r  %s✓%s got it — board in flash mode: %s %-15s\n" "$grn" "$r" "$(short "$port")" " "
      echo "$port"; return 0
    fi
  done
}

wait_for_serial_device() {  # wait for any serial device to reappear
  printf "\n  %sWaiting for a USB serial device to come back...%s\n" "$dim" "$r" >&2
  printf "  %spress c to cancel%s\n\n" "$dim" "$r" >&2
  local i=0 key ports=()
  while true; do
    i=$(((i+1)%10))
    printf "\r  %s%s%s scanning for serial ports... " "$cyn" "${spin:$i:1}" "$r" >&2
    read -rsn1 -t 1 key && [[ "$key" == "c" ]] && { printf "\r%-60s\r" " " >&2; return 1; }
    read_ports
    if [[ ${#ports[@]} -gt 0 ]]; then
      printf "\r  %s✓%s found %s\n" "$grn" "$r" "$(short "${ports[0]}")" >&2
      echo "${ports[0]}"
      return 0
    fi
  done
}

# ---- device picker --------------------------------------------------------
pick_device() {  # echoes chosen port or empty; interactive on stderr
  local all=() labels=() p v n
  # Build candidate list (Espressif tagged); usb_list already lists them.
  while IFS=$'\t' read -r p v n; do
    [[ -z "$p" ]] && continue
    if [[ "$v" == "$ESPRESSIF_VID" ]]; then all+=("$p"); labels+=("$n  ${grn}[Espressif]${r}")
    else all+=("$p"); labels+=("$n  ${dim}[$v]${r}"); fi
  done < <(usb_list)

  if [[ ${#all[@]} -eq 0 ]]; then echo ""; return 0; fi
  if [[ ${#all[@]} -eq 1 ]]; then echo "${all[0]}"; return 0; fi

  {
    printf "\n  %sMultiple devices found:%s\n" "$b" "$r"
    local i
    for i in "${!all[@]}"; do
      printf "   %s%d%s) %s\n      %s%s%s\n" \
        "$b" $((i+1)) "$r" "${labels[$i]}" "$dim" "${all[$i]}" "$r"
    done
    printf "   %sr%s) rescan\n" "$b" "$r"
  } >&2
  local sel
  IFS= read -rsn1 -p "  choice: " sel >&2
  printf "\n" >&2
  if [[ "$sel" == "r" ]]; then pick_device; return; fi
  if [[ "$sel" =~ ^[0-9]+$ ]] && (( sel>=1 && sel<=${#all[@]} )); then
    echo "${all[$((sel-1))]}"
  else echo ""; fi
}

monitor_port() {
  local ports=() p choice i

  if [[ "$PORT" != "auto" ]]; then
    if port_present "$PORT"; then
      echo "$PORT"
      return 0
    fi
    warn "Saved monitor port is gone; rescanning." >&2
  fi

  read_ports
  if [[ ${#ports[@]} -eq 1 ]]; then
    echo "${ports[0]}"
    return 0
  fi

  while true; do
    read_ports
    if [[ ${#ports[@]} -eq 1 ]]; then
      echo "${ports[0]}"
      return 0
    fi

    if [[ ${#ports[@]} -gt 1 ]]; then
      printf "\n  %sSerial ports detected:%s\n" "$b" "$r" >&2
      for i in "${!ports[@]}"; do
        printf "   %s%d%s) %s\n" "$b" $((i+1)) "$r" "$(short "${ports[$i]}")" >&2
      done
      printf "   %sr%s) rescan\n" "$b" "$r" >&2
      printf "   %sc%s) cancel\n" "$b" "$r" >&2
      IFS= read -rsn1 -p "  pick: " choice >&2
      printf "\n" >&2
      if [[ "$choice" == "r" || "$choice" == "R" ]]; then
        continue
      fi
      if [[ "$choice" == "c" || "$choice" == "C" || "$choice" == "q" || "$choice" == "Q" ]]; then
        return 1
      fi
      if [[ "$choice" =~ ^[0-9]$ ]] && (( choice>=1 && choice<=${#ports[@]} )); then
        echo "${ports[$((choice-1))]}"
        return 0
      fi
      continue
    fi

    p="$(wait_for_serial_device)" || return 1
    if [[ -n "$p" ]]; then
      echo "$p"
      return 0
    fi
  done
}

# ---- actions --------------------------------------------------------------
do_flash() {  # do_flash [monitor]
  local mon="${1:-}"
  printf "\n  %sScanning USB...%s\n" "$dim" "$r"
  local port
  port="$(pick_device)"
  if [[ -z "$port" ]]; then
    warn "No serial device detected."
    port="$(wait_for_device)" || { err "Cancelled."; return 1; }
  fi
  ok "Target: $(name_for "$port" 2>/dev/null) — $(short "$port")"

  if ! probe_spinner "$port"; then
    port="$(wait_for_device)" || { err "Cancelled."; return 1; }
  fi

  IFS= read -rsn1 -p "  Flash this device? [Y/n] " yn
  printf "\n"
  [[ "$yn" =~ ^[Nn] ]] && { warn "Aborted."; return 1; }

  PORT="$port"; save_conf
  local args=(-e "$ENV" -p "$port")
  [[ -n "$VERSION" ]] && args+=(-v "$VERSION")
  [[ "$mon" == "monitor" ]] && args+=(-m)
  printf "\n  %s\$ ./flash.sh %s%s\n" "$dim" "${args[*]}" "$r"
  ./flash.sh "${args[@]}"
}

do_build() {
  printf "\n"
  if [[ -n "$VERSION" ]]; then RSVP_FIRMWARE_VERSION="$VERSION" $PIO run -e "$ENV"
  else $PIO run -e "$ENV"; fi
}
do_monitor() {
  local p
  printf "\n  %sRescanning USB for monitor target...%s\n" "$dim" "$r"
  p="$(monitor_port)" || { err "Cancelled."; return 1; }
  ok "Monitor target: $(friendly "$(name_for "$p" 2>/dev/null)") — $(short "$p")"
  printf "\n  %sCtrl-C to exit monitor%s\n\n" "$dim" "$r"
  $PIO device monitor -p "$p" -b 115200
}
do_test()   { printf "\n"; $PIO test -e native_test; }
do_clean()  { printf "\n"; $PIO run -e "$ENV" -t clean; }
do_export() {
  printf "\n"
  if [[ -n "$VERSION" ]]; then python3 tools/export_web_firmware.py --version "$VERSION"
  else python3 tools/export_web_firmware.py; fi
}

doctor() {
  printf "\n  %sEnvironment check%s\n\n" "$b" "$r"
  if [[ "$PIO" == python3* ]]; then
    if python3 -m platformio --version >/dev/null 2>&1; then ok "PlatformIO (via python3 -m)"; else err "PlatformIO not found — see https://platformio.org"; fi
  else ok "pio: $PIO"; fi
  if [[ "$ESPTOOL" == python3* ]]; then
    if python3 -m esptool version >/dev/null 2>&1; then ok "esptool (via python3 -m)"; else warn "esptool not found (flash probe degraded)"; fi
  else ok "esptool: $ESPTOOL"; fi
  command -v python3 >/dev/null && ok "python3: $(command -v python3)" || err "python3 missing"
  printf "\n  %sUSB serial devices%s\n\n" "$b" "$r"
  local any=0 p v n
  while IFS=$'\t' read -r p v n; do
    any=1
    if [[ "$v" == "$ESPRESSIF_VID" ]]; then ok "$n  [Espressif]  $(short "$p")"
    else info "$n  [$v]  $(short "$p")"; fi
  done < <(usb_list)
  [[ "$any" -eq 0 ]] && warn "none detected (plug in the board, hold BOOT if reflashing)"
}

# ---- menu -----------------------------------------------------------------
menu() {
  screen_clear
  local p status port_label
  p="$(resolved_port)"
  if [[ -n "$p" ]]; then status="$(friendly "$(name_for "$p")") - ready"
  else status="not detected"; fi
  if [[ "$PORT" == "auto" ]]; then port_label="$(short "${p:-none}")  ${dim}(auto)${r}"
  else port_label="$(short "$PORT")  ${dim}(fixed)${r}"; fi

  printf "%s RSVP Nano dev console %s%s\n" "$rev" "$r" "$dim"
  printf " firmware build, flash, monitor%s\n" "$r"

  section "STATUS"
  status_row "target" "$status"
  status_row "port" "$port_label"
  status_row "env" "$ENV"
  status_row "version" "${VERSION:-(none)}"

  section "WORKFLOW"
  action_row 1 "Flash guided" 2 "Build firmware"
  action_row 3 "Flash + monitor" 4 "Monitor serial"

  section "MAINTENANCE"
  action_row 5 "Run tests" 6 "Clean build"
  action_row 7 "Export web FW" 8 "Doctor"

  section "SETTINGS"
  action_row e "Environment" p "Port"
  action_row v "Version label" r "Rescan"
  printf "  %s[q]%s Quit\n" "$cyn" "$r"

  printf "\n%schoice%s > " "$b" "$r"
  IFS= read -rsn1 choice || { echo; exit 0; }
  printf "\n"
  case "$choice" in
    1) do_flash;          pause ;;
    2) do_build;          pause ;;
    3) do_flash monitor ;;            # monitor owns the terminal afterwards
    4) do_monitor || pause ;;
    5) do_test;           pause ;;
    6) do_clean;          pause ;;
    7) do_export;         pause ;;
    8) doctor;            pause ;;
    e|E) choose_env;      save_conf ;;
    p|P) choose_port;     save_conf ;;
    v|V) set_version;     save_conf ;;
    r|R) ;;                           # just redraw
    q|Q|"") printf "  bye\n"; exit 0 ;;
    *) err "unknown: $choice"; pause ;;
  esac
}

choose_env() {
  printf "\n  Build env:\n"
  local i=1 e
  for e in "${VALID_ENVS[@]}"; do printf "   %s%d%s) %s\n" "$b" "$i" "$r" "$e"; ((i++)); done
  IFS= read -rsn1 -p "  env [1-${#VALID_ENVS[@]}]: " sel
  printf "\n"
  [[ "$sel" =~ ^[0-9]+$ ]] && (( sel>=1 && sel<=${#VALID_ENVS[@]} )) && ENV="${VALID_ENVS[$((sel-1))]}"
}
choose_port() {
  local picked; picked="$(pick_device)"
  if [[ -n "$picked" ]]; then
    IFS= read -rsn1 -p "  set fixed port to $(short "$picked")? [Y/n] (n = auto) " yn
    printf "\n"
    if [[ "$yn" =~ ^[Nn] ]]; then PORT="auto"; else PORT="$picked"; fi
  else
    PORT="auto"; warn "no device; left on auto"; sleep 1
  fi
}
set_version() { IFS= read -r -p "  version label (blank = none): " VERSION; }

# ---- entry ----------------------------------------------------------------
# If sourced (e.g. for tests) stop here without launching the menu.
(return 0 2>/dev/null) && return 0

case "${1:-}" in
  flash)  do_flash; exit $? ;;
  build)  do_build; exit $? ;;
  monitor) do_monitor; exit $? ;;
  doctor) doctor; echo; exit 0 ;;
  menu|"") while true; do menu; done ;;
  *) echo "usage: $0 [menu|flash|build|monitor|doctor]"; exit 1 ;;
esac
