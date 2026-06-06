# App.cpp decomposition plan

Status: **proposed** — no code moved yet. This is the target architecture and the
order we'll get there. Approve / adjust the module boundaries before extraction
starts.

## Problem

`src/app/App.cpp` is **7811 lines / 267 methods / 45 members** — a God Object.
Every other subsystem is already a clean class (`DisplayManager`,
`StorageManager`, `ReadingLoop`, `FocusTimer`, `CompanionSyncManager`,
`OtaUpdater`, …). `App` is the only file that grew unbounded, because it owns the
state machine **and** every screen's render/select logic **and** power, clock,
OTA, book, reader, and label code.

Goal: `App` shrinks to a **coordinator** — owns the subsystems, runs the
`update()` loop and `setState()` machine, and routes input/ticks to the right
controller. Each domain becomes its own `.h`/`.cpp` with explicit collaborators.

## Principles

1. **Incremental.** One module per step. Never a big-bang rewrite.
2. **Build-gated.** After each extraction: `pio run -e amoled` **and**
   `-e waveshare_esp32s3_usb_msc` must pass. Behavioral modules also get a
   flash-test before the next step.
3. **Commit per module.** Each step is one reviewable commit. Easy to bisect.
4. **Composition, not inheritance.** `App` owns each controller by value and
   passes collaborators (`DisplayManager&`, `Preferences&`, `ReadingLoop&`, …)
   in via the constructor or an `attach()` call. No globals.
5. **Behavior-preserving.** Pure move + re-wire. No logic changes inside a step;
   any real fix is a separate follow-up commit.
6. **Pull enums/structs with their owner.** e.g. `ScreensaverMode` moves into
   `StandbyScreensaver`, `TextEntrySession`/`KeyboardMode` into `TextEntryController`.

## Target modules

Listed low-coupling → high-coupling (also the extraction order).

### 1. `display/StandbyScreensaver` — proof of concept
- **Owns:** `ScreensaverMode` enum; all `seed/step/render` for Life/Maze/Voronoi/
  ScreenOff; `updateStandbyScreensaver`; `advanceStandbyRng`.
- **State:** `standbyLifeCells_`, `standbyLifeNextCells_`, `standbyMaze{Visited,Stack}_`,
  `standbyVoronoi{X,Y,Dx,Dy}_`, `standbyScreensaverDimCells_`, `standbyLifeGeneration_`,
  `standbyScreensaverRng_`, `lastStandbyFrameMs_`, mode.
- **Deps:** `DisplayManager&`.
- **App keeps:** when to start/stop it (tied to `AppState::Standby`).
- **Why first:** zero coupling to the state machine internals; ~500 lines out.

### 2. `app/TextEntryController` — isolated screen
- **Owns:** `TextEntrySession`, `KeyboardMode`, `TextEntryAction`, `TextEntryButton`;
  `openTextEntry`, `commitTextEntry`, `handleTextEntryTap`, `rebuildTextEntryButtons`,
  `renderTextEntry`, `activateTextEntryButton`.
- **Deps:** `DisplayManager&`; a completion callback (e.g. wifi password → caller).
- **Why early:** self-contained modal; only the Wi-Fi flow consumes its result.

### 3. `app/ClockController`
- **Owns:** `localNow`, `writeLocalToRtc`, `syncClockFromNetwork`,
  `fetchTimezoneOffsetMinutes`, `timezoneLabel`, clock-edit state, `localNow` helpers.
- **Deps:** RTC (BoardConfig), Wi-Fi/network, `Preferences&`.
- **Note:** `updateStreakForToday` reads the clock but writes reading stats →
  it goes with **BookSession/stats** (module 7), calling `ClockController::localNow`.

### 4. `app/OtaController`
- **Owns:** `otaCheckTask`, `startBackgroundOtaCheck`, `pollOtaCheckResult`,
  `maybeAutoCheckForUpdates`, `runFirmwareUpdate`, `preferredOtaConfig`,
  `otaAutoCheckEnabled`, `blockNetworkActionForOtaCheck`, OTA queue/task members.
- **Deps:** `OtaUpdater&`, network, `DisplayManager&`, `Preferences&`.
- **App keeps:** the `UpdateConfirm` screen render/select (menu layer) calls into it.

### 5. `reader/TimeEstimateEngine`
- **Owns:** `rebuildTimeEstimateCache`, `updateTimeEstimateBuild`,
  `cancelTimeEstimateBuild`, `flushPendingTimeEstimateRebuild`,
  `estimatedReadingTimeRemainingMs`, `estimatedPacingBonusMs`,
  `invalidateTimeEstimateCache`, `timeEstimateBuildMatchesCurrentBook`,
  `formatReadingTimeRemaining`.
- **State:** `timeEstimateBuild*`, `wordBonusBlockPrefixSumMs_`.
- **Deps:** `ReadingLoop&` / current-book word data, pacing config.

### 6. `power/PowerManager` (+ `power/BatteryMonitor`) — **KEPT IN APP (decided 2026-06-06)**
**Decision: not extracted.** Power/battery transitions are App state-machine
core, and the surface is too entangled to extract behavior-preserving with a
narrow interface:
- `wakeFromSleep` replays half of `begin()`; the power methods touch ~20 App
  session members (state, touch, reader, display, screensaver, storage, menu).
- `handlePowerButton` stays in App but calls 4 power methods; `handleBatteryProtection`
  calls `enterPowerOff`; `showLowBatteryWarning` resets touch/reader session state
  and calls `setState`.
- BatteryMonitor's surface is inflated by reader-UI + menu code: `batteryLabelMode_`
  is cycled (with `currentBatteryLabel()`+`setBatteryLabel()`) in the badge tap, the
  reader tap path, and Settings>Battery — all of which live in modules 8/9.
- `nominalBatteryRuntimeMinutes` reads CPU-freq config + scrollMode + OTA cache.

Extracting now would mean a ~10–15 callback/accessor "interface" (App-with-
indirection, not a real module) in the highest-risk code (power/wake/charge).
Net negative. Power stays in App; `applyStateCpuFrequency`/`noteActivity` etc.
remain alongside `setState`/`update()` as legitimate coordinator responsibilities.

(Reconsider only if a future need arises; modules 8/9 would first have to absorb
the label-cycle/tap/menu coupling, after which a BatteryMonitor data+protection
unit could be peeled more cleanly.)

### 7. `book/BookSession`
- **Owns:** `loadBookAtIndex`, `loadPendingBootBook`, `prepareBootBookLoad`,
  `restoreSavedBook`, `saveReadingPosition`, `maybeSaveReadingPosition`,
  `setBookFinished`, `bookIsFinished`, `markBookRecent`, `findBookIndexByPath`,
  all `book*Key`/`savedWordIndexForBook` pref-key helpers, lifetime stats
  (`load/saveLifetimeStats`, `updateStreakForToday`), `bookProgressPercent`,
  `readingProgressPercent`, `currentChapter*`.
- **Deps:** `StorageManager&`, `IndexedBookStore&`, `ReadingLoop&`, `Preferences&`,
  `ClockController&` (for streak day).

### 8. `reader/ReaderUI`
- **Owns:** `renderActiveReader`, `renderReaderWord`, `renderScrollReader`,
  `readerChrome`, `readerFooter*`, `renderContextPreview(/Browse)`,
  `updateContextPreviewWindow`, context/scrub state, `handleReaderTap` and its
  tap helpers (`handlePreviousSentenceTap`, `handleFooterMetricTap`,
  `handleBatteryBadgeTap`, `applyScrubTarget`, `scrubStepsForDrag`),
  `renderChapterTransition`/`updateChapterTransition`, `renderWpmFeedback`.
- **Deps:** `ReadingLoop&`, `DisplayManager&`, `TimeEstimateEngine&`, typography.

### 9. `app/MenuController` (+ per-screen renderers) — the hard core
- **Owns:** `menuScreen_` + every menu state vector (`settingsMenuItems_`,
  `bookMenuItems_`, `chapterMenuItems_`, `wifiNetworkMenuItems_`,
  `readingStatsItems_`, `focusTimerGenreMenuItems_`, …); `open*`, `select*`,
  `render*`, `rebuild*MenuItems`, `moveMenuSelection` for: Main, Settings{Home,
  Display,Pacing,Battery,Clock}, Wifi{Settings,Networks}, TypographyTuning,
  BookPicker, ChapterPicker, ReadingStats, {Restart,SdCardRepair,Update,
  PowerOff}Confirm, FocusTimer{Genres,Session}.
- **Deps:** nearly everything (it's the UI hub) → inject the other controllers.
- **Strategy:** extract `MenuController` as a facade first (move `menuScreen_` +
  routing), then peel individual screens into `screens/XxxScreen` sub-files in
  follow-up steps. This is multiple commits on its own.

### 10. Labels cleanup
- The ~40 pure `*Label()` / `*ModeLabel()` formatters move next to their owning
  module (battery labels → BatteryMonitor, reader labels → ReaderUI, etc.) or a
  small `app/SettingsLabels` namespace for the leftovers.

## Resulting `App`
After all steps, `App` holds: the subsystem/controller members, `begin()`,
`update()`, `setState()`/`updateState()`, `dispatchButtons()` routing, and
`stateName()`. Target: **under ~800 lines.**

## Final outcome (2026-06-06) — STOPPED at "coordinator core"
Extracted: 1 StandbyScreensaver, 2 TextEntryController, 3 ClockController,
4 OtaController, 5 TimeEstimateEngine, 7 BookLibraryStore (per-book NVS slice).
Not extracted: 6 PowerManager/BatteryMonitor (§6), 8 ReaderUI, 9 MenuController,
10 Labels.

**App.cpp: 7811 → 6262 lines.** Base hardware-verified at commit `5ed6d17`
(after step 5); subsequent steps build-green on both envs.

**Why we stopped (decided with the user):** the ~800-line target assumed every
domain peels cleanly. It does not. The clean *peripheral* subsystems are now out.
What remains is App's genuine, largely irreducible coordinator core:
- **ReaderUI** — `renderActiveReader` and its render tree read 25+ App
  members (reader, chapters/paragraphs, current book, state, time estimate,
  typography, footer mode, battery, scroll/context/wpm flags); `handleReaderTap`
  (~300 lines) *is* the reading-input state machine. Extracting it
  behavior-preserving needs a shared reader view-model (an MVC rewrite), not a
  move — net negative, in the most-touched path.
- **Power/Battery (§6)** — state-machine + PMU + wake/charge; `wakeFromSleep`
  replays half of `begin()`.
- **MenuController** — separable in principle (per-screen items/render/select on
  `menuScreen_`), the best remaining *clean* win if the work resumes, but it was
  deprioritised once "accept core" was chosen.

The remaining peelable-clean candidates, if ever wanted: a ContextPreview window
model (~100 lines, deps `reader_` + `paragraphStarts_`) and MenuController
(facade-first). Neither is required; App is now a coordinator with its core
intact and all genuinely-separable subsystems extracted.

## Extraction protocol (per module)
1. New `.h`/`.cpp`; move enum/struct/state/methods verbatim.
2. Replace member access with the injected collaborator references.
3. Add the controller as an `App` member; forward the old `App::foo()` call sites
   to `controller_.foo()` (or delete the call sites if fully owned).
4. `pio run -e amoled` + `-e waveshare_esp32s3_usb_msc` green.
5. Flash-test if behavioral.
6. Commit: `Refactor: extract <Module> from App`.

## Risks / watch-items
- **Shared I2C bus**: PowerManager/BatteryMonitor both talk to the AXP2101 PMU on
  the same `Wire` as touch. Keep bus ownership rules intact (never `Wire.end()`
  for one peripheral). See the standby bug history.
- **State-machine coupling**: PowerManager and MenuController must not get a raw
  `App&`. Give them narrow interfaces (`requestState`, `currentState`).
- **Build flags**: AMOLED-only code (`#if BOARD_AMOLED_18`) must stay guarded
  after moving (e.g. PowerSaving lives only on AMOLED).
- **No behavior change per step** — keeps each diff bisectable against the
  v0.2.0 baseline.

## Suggested order recap
1. StandbyScreensaver ✅ → 2. TextEntryController ✅ → 3. ClockController ✅ →
4. OtaController ✅ → 5. TimeEstimateEngine ✅ → 6. BatteryMonitor + PowerManager
**(kept in App — §6)** → 7. BookLibraryStore ✅ (NVS slice of BookSession) →
8. ReaderUI **(kept in App — core)** → 9. MenuController **(not done)** →
10. Labels **(not done)**. **Refactor stopped at the coordinator core — see
"Final outcome".**
