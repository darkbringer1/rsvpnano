# Copilot Instructions — rsvpnano AMOLED fork

## Repository context

This is a **fork** of [ionutdecebal/rsvpnano](https://github.com/ionutdecebal/rsvpnano), maintained by **darkbringer1** as `amoled-port`. Upstream `main` is the original bar-board firmware; AMOLED-specific work lives on `amoled-port`.

---

## Build & test

**Build firmware (PlatformIO):**
```sh
pio run -e waveshare_esp32s3_usb_msc
pio run -e amoled
```

**Run native unit tests (pacing logic only):**
```sh
pio test -e native_test
```
Tests live in `test/` and use the Unity framework. The only test suite is `test_pacing`.

**Release a new version (Windows, from `amoled-port`):**
```powershell
.\tools\release.ps1 -Version v0.1.X-amoled
```
This creates an annotated tag and pushes it, which triggers `.github/workflows/release.yml` to build and publish the firmware. Version must match `v\d+\.\d+\.\d+` (suffix like `-amoled` is allowed).

**Firmware version macro:** injected at build time by `tools/pio_set_version.py` as `RSVP_FIRMWARE_VERSION`. Available via `firmwareVersionLabel()` in `App.cpp`.

---

## Architecture

The firmware is a single-threaded Arduino loop on ESP32-S3. Entry point is `src/main.cpp` → `App::begin()` + `App::update()`.

### Module overview

| Module | Purpose |
|---|---|
| `src/app/App.cpp/.h` | Central orchestrator — all state machines, settings, touch handling, menu navigation |
| `src/display/DisplayManager` | All rendering: RSVP word, scroll view, menus, settings, footer chrome |
| `src/reader/ReadingLoop` | Word timing engine — WPM, pacing delays, scrub, seek |
| `src/storage/StorageManager` | SD card, book loading, `.rsvp` index format, chapter markers |
| `src/storage/EpubConverter` | EPUB → `.rsvp` conversion on-device |
| `src/timer/FocusTimer` | Focus timer with per-genre duration memory |
| `src/update/OtaUpdater` | OTA firmware updates via GitHub Releases |
| `src/sync/CompanionSyncManager` | Browser/iOS companion sync over WiFi |

### Settings persistence

All user preferences are stored in ESP32 NVS via `Preferences`. Keys are defined as `constexpr const char *kPrefXxx` at the top of `App.cpp`. **NVS keys must be ≤ 15 characters.**

### Settings menu structure

Settings menus are driven by index constants (`kSettingsXxxIndex`) + a `rebuildSettingsMenuItems()` builder + `selectSettingsItem()` handler. When adding a new setting:
1. Add a `constexpr size_t kSettingsXxxIndex = N;` constant
2. Add a `push_back(...)` in `rebuildSettingsMenuItems()` in the correct menu block
3. Add a `case kSettingsXxxIndex:` in the Display/Pacing handler inside `selectSettingsItem()`
4. Add the NVS key constant and load it in the boot pref-loading block (after `readerMode_` is loaded if it depends on the reader mode)

### Reader modes

`App::ReaderMode::Rsvp` (default) and `App::ReaderMode::Scroll`. `scrollModeEnabled()` returns `readerMode_ == ReaderMode::Scroll`. Chapter label visibility and other per-mode defaults use separate NVS keys (`kPrefChapterLabelRsvp`, `kPrefChapterLabelScroll`).

### Chapter labels

`currentChapterLabel()` → `cleanedChapterTitle()` strips leading `N.` prefixes from EPUB spine IDs. If no heading was found during EPUB conversion, the raw spine filename is used as title — this is expected behavior.

### `readerChrome()`

Controls footer visibility flags (`showBattery`, `showChapter`, `showProgress`, `showPreviousSentenceHint`). Chapter label is hidden when `!chapterLabelEnabled_ || scrollModeEnabled()`.

---

## Branch conventions

| Branch | Purpose |
|---|---|
| `main` | Upstream-compatible baseline |
| `amoled-port` | Integration branch for the Waveshare ESP32-S3-Touch-AMOLED-1.8 port |
| `hot-feature/*` | Feature branches for focused work before merging into `amoled-port` |

Keep upstream PR imports compatible with the AMOLED board. Preserve `BOARD_AMOLED_18` branches, the SH8601 display path, FT3168 touch, AXP2101 battery reads, and the single-BOOT-button control mapping.

**Commits:** no `Co-authored-by: Copilot` trailer (user preference).

---

## OTA / fork config

`src/update/OtaUpdater.h` is hard-locked to `darkbringer1/rsvpnano`. Do not make OTA owner/repo configurable or point it at upstream; a bar-board OTA image can black-screen the AMOLED board.

---

## Key build flags

| Flag | Meaning |
|---|---|
| `RSVP_ON_DEVICE_EPUB_CONVERSION=1` | Enable on-device EPUB → .rsvp conversion |
| `RSVP_USB_TRANSFER_ENABLED=1` | Enable USB MSC mode (drag-and-drop) |
| `RSVP_FIRMWARE_VERSION` | Injected at build time from git tag |
| `BOARD_AMOLED_18=1` | Enable the SH8601/FT3168/AXP2101 AMOLED board port |
