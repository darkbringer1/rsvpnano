# RSVP Nano — AMOLED Port (Waveshare ESP32-S3-Touch-AMOLED-1.8)

This document covers the fork's port of RSVP Nano to the **Waveshare
ESP32-S3-Touch-AMOLED-1.8** board and the changes made on top of the upstream
`ionutdecebal/rsvpnano` firmware. The fork lives at
**https://github.com/darkbringer1/rsvpnano**.

The original firmware targets a "bar" board (AXS15231B display). This port runs
on a square 368×448 AMOLED panel with capacitive touch and a single button. The
two builds are **not** firmware-compatible — flashing a bar-board image onto the
AMOLED board re-bricks the display, which is why OTA is hard-locked (see below).

---

## Hardware

| Part | Device | Bus / Pins |
|------|--------|-----------|
| Display | SH8601 368×448 QSPI AMOLED (self-emissive, no backlight) | QSPI: CS 12, SCLK 11, D0–D3 4/5/6/7; reset via expander |
| Touch | FT3168 capacitive | `Wire` (SDA 15, SCL 14), addr 0x38 |
| IO expander | XCA9554 (drives panel reset/power) | `Wire` (15/14), addr 0x20 |
| PMU / battery | AXP2101 | `Wire` (15/14), addr 0x34 |
| SD card | microSD (1-bit SDMMC via GPIO matrix) | CLK 2, CMD 1, D0 3 |
| Button | BOOT (GPIO0) — the only usable button | — |

The logical UI runs in landscape (448×368); the panel is addressed in its native
368×448 portrait and swapped in software.

---

## Build & Flash

Build the AMOLED environment:

```
pio run -e amoled
```

Flash over USB:

```
pio run -e amoled -t upload
```

> **Flashing caveat (USB MSC):** the AMOLED build enables SD-over-USB mass
> storage, which requires the TinyUSB stack (`ARDUINO_USB_MODE=0`). When the
> firmware is running the device enumerates as a **USB drive**, not a serial
> port. To re-flash, **hold BOOT while plugging in** to enter the ROM download
> mode, then run upload. Serial logging still works via the composite USB-CDC
> device (`ARDUINO_USB_CDC_ON_BOOT=1`).

---

## Controls (single button + touch)

The bar board had BOOT + PWR. This board has only BOOT, so the control scheme is
remapped:

| Input | Action |
|-------|--------|
| Touch & hold (reader) | Read (RSVP advances while held) |
| Release | Pause |
| Vertical swipe (menu) | Scroll the list |
| **Double-tap (menu)** | Select the highlighted item |
| BOOT short press | Open / back out of the menu |
| BOOT long press | Cycle brightness |

### Double-tap to select

Menu selection requires a **double tap**, not a single tap. A short swipe that
lands like a tap used to open the wrong menu item; requiring a deliberate
double-tap (same position, within the reader double-tap window) fixes accidental
opens while scrolling. The keyboard (text entry) still uses single taps.

Implementation: `App::applyMenuTouchGesture` in `src/app/App.cpp`.

---

## Idle auto-standby + screensavers

The upstream standby/screensaver system is entered with a **PWR+BOOT combo**,
which is unreachable on a one-button board. The port adds **idle auto-standby**
for the AMOLED build instead:

- After **3 minutes** of inactivity in `Paused` or `Menu`, the device enters the
  standby screensaver (`kIdleStandbyTimeoutMs` in `src/app/App.cpp`).
- It **wakes on any screen touch or a BOOT press** (`handleAmoledStandbyWake`),
  with a short grace window so the entering gesture can't immediately bounce it
  back awake.
- Activity (touch or BOOT) resets the idle timer via `noteActivity`.
- Reading (`Playing`) never idles out — hold-to-read keeps activity alive.

The existing screensaver visuals (Life, maze, Voronoi, screen-off) are reused
unchanged.

---

## USB transfer (SD-over-USB)

`Menu → USB transfer` exposes the microSD card to a host computer as a USB mass
storage device, so books/articles can be copied without removing the card.

- Enabled via `RSVP_USB_TRANSFER_ENABLED=1` + TinyUSB (`ARDUINO_USB_MODE=0`) in
  the `amoled` env (`platformio.ini`).
- **Exit:** eject/unmount the drive from the computer, or press BOOT. (There is
  no PWR button, so the upstream PWR-hold exit does not apply.)
- Implementation: `src/usb/UsbMassStorageManager.*`.

---

## Battery (AXP2101)

Battery telemetry is read from the **AXP2101 PMU** over the `Wire` bus at 0x34
(`readBatteryStatusAxp2101` in `src/board/BoardConfig.cpp`).

- **Read-only and safe:** only ADC/status registers are read, and at most a
  read-modify-write to the ADC-enable register (0x30). Power-rail (DCDC/LDO)
  registers are never written — a bad write there would brown out the board.
- Battery voltage is read from registers 0x34/0x35 (14-bit mV) and converted to a
  percentage with the existing discharge curve (`batteryPercentForVoltage`).
- If no PMU ACKs or no battery is attached, `readBatteryStatus` returns `false`
  and the UI simply shows no battery — identical to previous behavior.

> **UNVERIFIED ON HARDWARE.** This was written without a battery attached. When a
> battery is connected, verify the reported voltage in the serial log
> (`[power] battery x.xx V ...`). If it reads wrong, the suspect registers
> (0x34/0x35 format, ADC-enable bit) are documented inline in `BoardConfig.cpp`.

**Known limitation:** the critical-battery auto-shutoff (`handleBatteryProtection`
→ `releaseBatteryPowerHold`) cuts power via the TCA9554 on the bar board's
`Wire1`. On the AMOLED board power is held by the AXP2101, so the protective
shutdown won't actually cut power. Harmless without a battery; revisit when audio
/ PMU power management is ported.

---

## OTA — hard-locked to this fork

OTA updates are **hard-locked** to `darkbringer1/rsvpnano`:

- `OtaUpdater::kLockedOwner` / `kLockedRepo` (`src/update/OtaUpdater.h`).
- `github_owner`, `github_repo`, and `asset_name` keys in `/config/ota.conf` are
  **ignored** (a warning is logged), and `loadConfig` force-overrides them after
  parsing.
- Release tags must use a simple `v...` format, the asset URL must match this
  repo and tag, and redirects are only accepted from GitHub's release asset CDN.
- Only Wi-Fi credentials and `auto_check` remain configurable.

**Why:** the AMOLED port runs different hardware than upstream. Pulling a foreign
firmware image over OTA would flash a bar-board build and re-brick the display.
Locking the source to the fork makes that mistake impossible. Cut releases on the
fork for OTA to pick them up.

---

## Feature status

| Feature | Status | Notes |
|---------|--------|-------|
| Reader (RSVP, hold-to-read, WPM, scrub) | ✅ Working | |
| Touch (FT3168) | ✅ Working | |
| Menu (BOOT button) + double-tap select | ✅ Working | |
| Brightness (BOOT long-press) | ✅ Working | |
| Focus Timer + IMU (tap/hold/flip) | ✅ Working | |
| Companion sync (exit → menu) | ✅ Working | |
| Idle auto-standby + screensavers | ✅ New (this port) | Needs on-device confirmation |
| USB transfer (SD-over-USB) | ✅ Enabled (this port) | Verify host enumerates the SD card |
| Battery % (AXP2101) | ⚠️ Implemented, UNVERIFIED | No battery on hand yet |
| OTA | 🔒 Locked to fork | Source restricted to `darkbringer1/rsvpnano` |
| Books / Articles / Chapters (SD) | ⚠️ Untested | Needs a FAT32 SD card |
| Settings / RSS / Wi-Fi | ⚠️ Untested | |
| Audio (ES8311) | ❌ Not ported | Planned later |

---

## Changed files (this round)

- `platformio.ini` — AMOLED env: TinyUSB + USB MSC enabled.
- `src/update/OtaUpdater.{h,cpp}` — OTA source hard-locked to the fork.
- `docs/ota.conf.example` — documents the lock.
- `src/board/BoardConfig.cpp` — AXP2101 read-only battery path.
- `src/app/App.{h,cpp}` — double-tap menu select; idle auto-standby + wake.
