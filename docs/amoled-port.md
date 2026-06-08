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

## Controls (two buttons + touch)

This board has BOOT and PWR. BOOT is a normal GPIO; PWR is the **AXP2101 PMU
PWRKEY** (not a GPIO) and is polled from the PMU IRQ. All physical-button
handling is routed through one place — `App::dispatchButtons()`, with a
canonical button-map comment above it (the single source of truth).

| Input | Action |
|-------|--------|
| Touch & hold (reader) | Read (RSVP advances while held) |
| Release | Pause |
| Vertical swipe (menu) | Scroll the list |
| Horizontal swipe right (menu) | Go back one level / resume |
| **Double-tap (menu)** | Select the highlighted item |
| BOOT short press | Open / back out of the menu |
| BOOT long press | Cycle brightness |
| PWR short press | Deep-sleep standby (screen/touch off) |
| PWR long press (hold) | Deep-sleep standby ("Goodbye", screen/touch off) |
| BOOT in deep-sleep standby | Wake / reboot back into app |

AXP2101 rail-cut is intentionally not used on this board right now: battery
testing showed that it shuts down but does not wake from PWRKEY unless USB is
inserted. Standby uses ESP deep sleep with BOOT/GPIO0 as the verified wake
source.

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

The touch driver **self-heals** across the screen-off transition. Sleeping the
panel can disturb the shared touch I2C bus (FT3168 + XCA9554 expander), which
used to make `TouchHandler::poll` disable itself after a burst of read failures
and stay dead until reboot — so wake-on-touch deadlocked. On repeated failures
it now restarts the bus and re-probes the controller, then keeps polling
(`reinitialize` in `src/input/TouchHandler.cpp`).

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

## Power management (AXP2101 PMU)

The **AXP2101 PMU** (`Wire` bus, 0x34) owns the main rail and the PWR button. It
is driven through **XPowersLib** (`src/board/BoardConfig.cpp`, guarded by
`BOARD_AMOLED_18`).

- **PWR button = PWRKEY.** `pmuPollPowerKey()` reads the PMU's PWRKEY IRQ (short
  vs long press); `App::updatePmuPowerKey()` maps it to deep-sleep standby.
- **Deep-sleep standby instead of rail-cut.** `enterPowerOff()` saves state,
  turns the display/touch path dark, turns Wi-Fi off, closes storage, and enters
  ESP deep sleep with BOOT/GPIO0 as the wake source. AXP2101 rail-cut is disabled
  because this hardware did not restart from PWRKEY on battery without USB.
- **Battery telemetry** comes from the same PMU: `getBatteryPercent()`,
  `getBattVoltage()`, `isCharging()`, `isBatteryConnect()`, `isVbusIn()`.

> **Battery rail-cut failed wake testing.** The board cut power, but PWR hold did
> not restart it until USB was inserted. Keep PMU shutdown disabled unless the
> board-level wake source is identified and verified.

---

## Clock + reading streak (PCF85063 RTC)

The board's **PCF85063 RTC** (`Wire` bus, 0x51) keeps wall-clock time, used for the
reading streak. Hand-rolled BCD driver in `BoardConfig.cpp`
(`rtcRead`/`rtcWrite`/`rtcPresent`).

- **RTC stores UTC**; the timezone offset is applied on read (`App::localNow`), so
  changing the zone is instant — no clock rewrite.
- **Settings > Clock**: sync over Wi-Fi (NTP + geo-IP timezone auto-detect via
  `ip-api.com`), manual timezone (1h steps), or manual date/time (field-cycle
  rows that write the RTC live, leap-year aware).
- **Streak**: entering Playing marks today (local day); consecutive days increment,
  a gap resets. Shown on the Reading stats screen with the clock + zone.

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
| Touch (FT3168) | ✅ Working | Self-heals after screen-off; no longer dies until reboot |
| Menu (BOOT button) + double-tap select | ✅ Working | |
| Swipe-right back (menu) | ✅ New | |
| Brightness (BOOT long-press) | ✅ Working | |
| PWR button + deep-sleep standby | ✅ Working | BOOT wakes; rail-cut disabled because AXP2101 shutdown did not PWR-wake |
| Clock / timezone (PCF85063 RTC) | ✅ New | NTP + geo-IP auto-tz; manual set in Settings > Clock |
| Book completion summary | ✅ New | |
| Reading stats + streak | ✅ New | Streak needs day rollover to fully verify |
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

- `platformio.ini` — AMOLED env: TinyUSB + USB MSC enabled; XPowersLib dependency.
- `src/update/OtaUpdater.{h,cpp}` — OTA source hard-locked to the fork.
- `docs/ota.conf.example` — documents the lock.
- `src/board/BoardConfig.{h,cpp}` — AXP2101 PMU via XPowersLib (PWR button,
  real power-off, battery); PCF85063 RTC driver.
- `src/app/App.{h,cpp}` — double-tap menu select; idle auto-standby + wake;
  central `dispatchButtons()` + button map; swipe-right back; book completion
  screen; reading stats + streak; Settings > Clock (NTP + auto-timezone).
- `src/input/TouchHandler.{h,cpp}` — touch self-heal (bus restart + re-probe) instead of permanent disable after read failures.
