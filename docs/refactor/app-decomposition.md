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

### 6. `power/PowerManager` (+ `power/BatteryMonitor`)
- **BatteryMonitor owns:** `updateBatteryStatus`, `updateBatteryRuntimeLabel`,
  `nominalBatteryRuntimeMinutes`, `currentBatteryLabel`, `battery*Label`,
  `formatBatteryTimeRemaining`, `handleBatteryProtection`, `showLowBatteryWarning`,
  `updateBatteryWarningOverlay`. Deps: PMU (BoardConfig), `DisplayManager&`.
- **PowerManager owns:** `enter/exitStandby`, `enter/exitPowerSaving`,
  `updateIdleStandby`, `updateDeepStandbyIdle`, `handleAmoledStandbyWake`,
  `enterSleep`, `wakeFromSleep`, `enterPowerOff`, `updatePmuPowerKey`,
  `applyStateCpuFrequency`, `updateAutoDim`, `restoreFromAutoDim`, `noteActivity`.
- **Coupling note:** PowerManager drives `AppState` transitions → it needs a
  narrow interface to App: `requestState(AppState)`, `currentState()`,
  `returnStateAfterStandby()`. Define that interface explicitly; don't hand it
  the whole `App`.

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
1. StandbyScreensaver → 2. TextEntryController → 3. ClockController →
4. OtaController → 5. TimeEstimateEngine → 6. BatteryMonitor + PowerManager →
7. BookSession → 8. ReaderUI → 9. MenuController (multi-commit) → 10. Labels.
