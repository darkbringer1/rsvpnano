#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <vector>

#include "app/AppState.h"
#include "app/Localization.h"
#include "audio/AudioManager.h"
#include "display/DisplayManager.h"
#include "app/ClockController.h"
#include "app/OtaController.h"
#include "book/BookLibraryStore.h"
#include "app/TextEntryController.h"
#include "display/StandbyScreensaver.h"
#include "reader/TimeEstimateEngine.h"
#include "input/ButtonHandler.h"
#include "input/TouchHandler.h"
#include "reader/ReadingLoop.h"
#include "rss/RssFeedManager.h"
#include "sensor/MotionSensor.h"
#include "storage/StorageManager.h"
#include "sync/CompanionSyncManager.h"
#include "timer/FocusTimer.h"
#include "update/OtaUpdater.h"
#include "usb/UsbMassStorageManager.h"

class App {
 public:
  enum class ReaderMode : uint8_t {
    Rsvp = 0,
    Scroll = 1,
  };

  enum class HandednessMode : uint8_t {
    Right = 0,
    Left = 1,
  };

  App();

  void begin();
  void update(uint32_t nowMs);
  // Set from setup() before begin(): records why this boot happened (reset +
  // wake cause) so it can be shown on the boot splash for power-off debugging.
  void setBootReason(int resetReason, int wakeCause);

 private:
  struct PausedTouchSession {
    bool active = false;
    uint16_t startX = 0;
    uint16_t startY = 0;
    uint16_t lastX = 0;
    uint16_t lastY = 0;
    uint32_t startMs = 0;
    uint32_t lastMs = 0;
    size_t startWordIndex = 0;
    int gestureStepsApplied = 0;
    int32_t browseOffsetPermille = 0;
  };

  enum class TouchIntent {
    None,
    PlayHold,
    Scrub,
    BrowseScroll,
    Wpm,
  };

  enum class MenuScreen {
    Main,
    SettingsHome,
    SettingsDisplay,
    SettingsPacing,
    SettingsBattery,
    SettingsClock,
    WifiSettings,
    WifiNetworks,
    TextEntry,
    TypographyTuning,
    BookPicker,
    ChapterPicker,
    ReadingStats,
    RestartConfirm,
    SdCardRepairConfirm,
    UpdateConfirm,
    PowerOffConfirm,
    FocusTimerPresets,
    FocusTimerSession,
  };

  enum class FooterMetricMode : uint8_t {
    Percentage = 0,
    ChapterTime = 1,
    BookTime = 2,
  };

  enum class BatteryLabelMode : uint8_t {
    Percent = 0,
    TimeRemaining = 1,
    Voltage = 2,
  };

  enum class PauseMode : uint8_t {
    SentenceEnd = 0,
    Instant = 1,
  };

  struct WifiNetworkInfo {
    String ssid;
    int32_t rssi = 0;
    uint8_t authMode = 0;
  };

  void setState(AppState nextState, uint32_t nowMs);
  void applyStateCpuFrequency();
  void updateState(uint32_t nowMs);
  void updateReader(uint32_t nowMs);
  void updateWpmFeedback(uint32_t nowMs);
  void updateBrightnessToast(uint32_t nowMs);
  void maybeSaveReadingPosition(uint32_t nowMs);
  // Single entry point for all hardware-button handling (see the button map
  // comment above App::dispatchButtons). Routes to the per-button handlers below.
  void dispatchButtons(uint32_t nowMs);
  void handleBootButton(uint32_t nowMs);
  void handlePowerButton(uint32_t nowMs);
#if defined(BOARD_AMOLED_18)
  void handleAmoledButton(uint32_t nowMs);
  void updatePmuPowerKey(uint32_t nowMs);
#endif
  bool handleStandbyCombo(uint32_t nowMs);
  void toggleMenuFromPowerButton(uint32_t nowMs);
  void menuBackOneLevel(uint32_t nowMs);  // pop one sub-menu page (mirrors the "Back" row)
  void openMainMenu(uint32_t nowMs);
  void cycleBrightness(uint32_t nowMs);
  void cycleThemeMode(uint32_t nowMs);
  void cycleUiLanguage(uint32_t nowMs);
  void cycleReaderMode(uint32_t nowMs);
  void cycleHandednessMode(uint32_t nowMs);
  void togglePhantomWords(uint32_t nowMs);
  void cycleReaderFontSize(uint32_t nowMs);
  void applyDisplayPreferences(uint32_t nowMs, bool rerender = true);
  void applyHandednessSettings(uint32_t nowMs, bool rerender = true);
  void applyTypographySettings(uint32_t nowMs, bool rerender = true);
  uint8_t currentBrightnessPercent() const;
  bool updateBatteryStatus(uint32_t nowMs, bool force = false);
  void handleBatteryProtection(uint32_t nowMs);
  void showLowBatteryWarning(uint32_t nowMs);
  void updateBatteryWarningOverlay(uint32_t nowMs);
  void updateAutoDim(uint32_t nowMs);
  void restoreFromAutoDim(uint32_t nowMs);
  void updateBatteryRuntimeLabel(uint32_t nowMs);
  void handleTouch(uint32_t nowMs);
  void applyPausedTouchGesture(const TouchEvent &event, uint32_t nowMs);
  void handleReaderTap(uint16_t x, uint16_t y, uint32_t nowMs);
  bool handleFooterMetricTap(uint16_t x, uint16_t y, uint32_t nowMs);
  bool handleBatteryBadgeTap(uint16_t x, uint16_t y, uint32_t nowMs);
  void applyScrollConfig();
  bool handlePreviousSentenceTap(uint16_t x, uint16_t y, uint32_t nowMs);
  void requestReaderPauseAtSentenceEnd(uint32_t nowMs);
  void finalizeReaderPause(uint32_t nowMs);
  bool shouldFinalizeReaderPause(uint32_t nowMs) const;
  void resetReaderTapTracking();
  bool isFooterMetricTap(uint16_t x, uint16_t y) const;
  bool isBatteryBadgeTap(uint16_t x, uint16_t y) const;
  bool isPreviousSentenceTap(uint16_t x, uint16_t y) const;
  bool isActivelyReading() const;
  bool shouldStayAwake() const;  // reading or running timer -> block all idle sleep/dim
  bool readerFooterVisible() const;
  DisplayManager::ReaderChrome readerChrome() const;
  String readerFooterStatusLabel() const;
  String onOffLabel(bool enabled) const;
  int scrubStepsForDrag(int deltaX) const;
  void applyScrubTarget(int targetSteps, uint32_t nowMs);
  int browseScrollRatePermille(uint16_t y) const;
  void applyBrowseHoldScroll(uint16_t y, uint32_t elapsedMs, uint32_t nowMs);
  void renderContextBrowsePreview(size_t currentIndex, uint16_t scrollProgressPermille);
  void applyMenuTouchGesture(const TouchEvent &event, uint32_t nowMs);
  void applyFocusTimerTouch(const TouchEvent &event, uint32_t nowMs);
  void moveMenuSelection(int direction);
  void selectMenuItem(uint32_t nowMs);
  void openFocusTimer();
  void updateFocusTimer(uint32_t nowMs);
  void resetFocusTimer();
  void openFocusTimerPresetPicker();
  void rebuildFocusTimerPresetMenuItems();
  void selectFocusTimerPreset(uint32_t nowMs);
  void loadFocusTimerPreferences();
  void saveFocusTimerPreset(FocusTimer::Preset preset);
  void openSettings();
  void selectSettingsItem(uint32_t nowMs);
  void openBatterySettings();
  void selectBatterySettingsItem(uint32_t nowMs);
  static String cpuMhzLabel(uint32_t mhz);
  String autoDimDelayLabel() const;
  String autoDimBrightnessLabel() const;
  uint32_t nominalBatteryRuntimeMinutes() const;
  void openWifiSettings();
  void selectWifiSettingsItem(uint32_t nowMs);
  void openTypographyTuning();
  void selectTypographyTuningItem(uint32_t nowMs);
  void cycleTypographyPreviewSample(int direction);
  void rebuildSettingsMenuItems();
  void applyPacingSettings();
  void maybeAutoCheckForUpdates(uint32_t nowMs);
  void maybeAutoSyncClock(uint32_t nowMs);
  void maybeOpenUpdateConfirm(uint32_t nowMs);
  bool updateConfirmCanOpen() const;
  void runRssFeedCheck(uint32_t nowMs);
  OtaUpdater::Config preferredOtaConfig();
  void scanWifiNetworks();
  void renderWifiNetworks();
  void selectWifiNetworkItem(uint32_t nowMs);
  void openWifiPasswordEntry(const String &ssid, const String &initialValue);
  void submitWifiPassword(uint32_t nowMs);
  String configuredWifiSsid();
  bool otaAutoCheckEnabled();
  String pacingDelayLabel(uint16_t delayMs) const;
  String firmwareUpdateMenuLabel() const;
  String themeModeLabel() const;
  String phantomWordsLabel() const;
  String focusHighlightLabel() const;
  String uiLanguageLabel() const;
  String readerModeLabel() const;
  String pauseModeLabel() const;
  String handednessLabel() const;
  String readerFontSizeLabel() const;
  String readerTypefaceLabel() const;
  String typographyTuningLabel() const;
  String typographyTuningValueLabel() const;
  String uiText(UiText key) const;
  void openBookPicker(bool articlesOnly = false);
  void selectBookPickerItem(uint32_t nowMs);
  void openChapterPicker();
  void selectChapterPickerItem(uint32_t nowMs);
  void openRestartConfirm();
  void selectRestartConfirmItem(uint32_t nowMs);
  void openSdCardRepairConfirm();
  void selectSdCardRepairConfirmItem(uint32_t nowMs);
  void runSdCardRepair(uint32_t nowMs);
  void runSdCardCheck(uint32_t nowMs);
  void openUpdateConfirm();
  void selectUpdateConfirmItem(uint32_t nowMs);
  void openPowerOffConfirm(uint32_t nowMs);
  void selectPowerOffConfirmItem(uint32_t nowMs);
  void enterCompanionSync(uint32_t nowMs);
  void updateCompanionSync(uint32_t nowMs);
  void exitCompanionSync(uint32_t nowMs);
  void enterUsbTransfer(uint32_t nowMs);
  void updateUsbTransfer(uint32_t nowMs);
  void exitUsbTransfer(uint32_t nowMs);
  void enterStandby(uint32_t nowMs);
  void exitStandby(uint32_t nowMs);
  void noteActivity(uint32_t nowMs);
#if defined(BOARD_AMOLED_18)
  void updateIdleStandby(uint32_t nowMs);
  void handleAmoledStandbyWake(uint32_t nowMs);
  void enterPowerSaving(uint32_t nowMs);
  void exitPowerSaving(uint32_t nowMs);
  void updateDeepStandbyIdle(uint32_t nowMs);
  String deepStandbyDelayLabel() const;
#endif
  void enterPowerOff(uint32_t nowMs);
  void enterSleep(uint32_t nowMs);
  void wakeFromSleep();
  bool restoreSavedBook(uint32_t nowMs);
  bool prepareBootBookLoad();
  void loadPendingBootBook(uint32_t nowMs);
  void saveReadingPosition(bool force = false);
  bool loadBookAtIndex(size_t index, uint32_t nowMs, bool allowLegacyPositionFallback = false,
                       bool allowIndexBuild = true, bool allowEpubConversion = true,
                       bool rebuildTimeEstimate = true);
  void enterBookFinished(uint32_t nowMs);
  void renderBookFinished();
  bool bookProgressPercent(size_t bookIndex, uint8_t &percent);
  void renderMenu();
  void renderMainMenu();
  void renderSettings();
  void renderTypographyTuning();
  void renderBookPicker();
  void renderChapterPicker();
  // Lifetime reading stats screen (words/time/books read).
  void openReadingStats();
  void renderReadingStats();
  void selectReadingStatsItem(uint32_t nowMs);
  void loadLifetimeStats();
  void saveLifetimeStats();
  // Reading streak (consecutive days), driven by the PCF85063 RTC.
  void updateStreakForToday();
  // Settings > Clock submenu.
  void openClockSettings();
  void selectClockSettingsItem(uint32_t nowMs);
  void renderRestartConfirm();
  void renderSdCardRepairConfirm();
  void renderUpdateConfirm();
  void renderPowerOffConfirm();
  void renderFocusTimerPresets();
  void renderFocusTimerSession();
  void renderActiveReader(uint32_t nowMs);
  bool updateChapterTransition(uint32_t nowMs);
  bool maybeStartChapterTransition(size_t previousWordIndex, size_t currentWordIndex,
                                   uint32_t nowMs);
  void renderChapterTransition();
  void renderScrollReader(uint32_t nowMs, const String &overlayText = "");
  DisplayManager::LibraryItem libraryItemForBook(size_t bookIndex);
  String chapterMenuLabel(size_t chapterIndex) const;
  size_t currentChapterIndex() const;
  String currentChapterLabel() const;
  String cleanedChapterTitle(const String &raw, const String &fallback) const;
  const char *chapterLabelPrefKey() const;
  static bool chapterLabelDefaultForMode(ReaderMode mode);
  String currentFooterMetricLabel() const;
  String currentBatteryLabel() const;
  String footerMetricModeLabel() const;
  String batteryLabelModeLabel() const;
  String batteryTimeRemainingLabel() const;
  String batteryVoltageLabel() const;
  String formatBatteryTimeRemaining(uint32_t minutes) const;
  String timeEstimateModeLabel() const;
  uint8_t readingProgressPercent() const;
  bool ensureCurrentBookWordAvailable(uint32_t nowMs);
  void handleCurrentBookReadFailure(uint32_t nowMs, const char *detail);
  void renderReaderWord();
  void renderContextPreview();
  void renderWpmFeedback(uint32_t nowMs);
  size_t phantomBeforeCharTarget() const;
  size_t phantomAfterCharTarget() const;
  String collectPhantomBeforeText(size_t currentIndex, size_t charTarget) const;
  String collectPhantomAfterText(size_t currentIndex, size_t charTarget) const;
  String phantomBeforeText() const;
  String phantomAfterText() const;
  bool isParagraphStart(size_t wordIndex) const;
  size_t paragraphStartAtOrBefore(size_t wordIndex) const;
  size_t contextPreviewAnchorIndex(size_t currentIndex) const;
  void updateContextPreviewWindow(size_t currentIndex);
  void invalidateContextPreviewWindow();
  void renderStorageStatus(const char *title, const char *line1, const char *line2,
                           int progressPercent);
  static void handleStorageStatus(void *context, const char *title, const char *line1,
                                  const char *line2, int progressPercent);
  const char *stateName(AppState state) const;
  const char *touchPhaseName(TouchPhase phase) const;
  bool isFocusTimerMenuScreen(MenuScreen screen) const;
  bool scrollModeEnabled() const;
  void applyUiOrientation(BoardConfig::UiOrientation orientation);
  void applyReaderUiOrientation();
  void reloadRuntimePreferences(uint32_t nowMs, bool rerender);
  BoardConfig::UiOrientation readerUiOrientation() const;
  bool uiRotated180() const;
  void updateAutoOrientation(uint32_t nowMs);
  void refreshCurrentScreen(uint32_t nowMs);
  void cycleOrientationLock(uint32_t nowMs);
  String orientationLockLabel() const;
  uint8_t effectiveAnchorPercent() const;
  DisplayManager::TypographyConfig effectiveTypographyConfig() const;
  uint32_t currentReaderContentToken() const;
  String formatFocusTimerRemaining(uint32_t nowMs) const;
  String formatFocusTimerDuration(uint32_t durationMs) const;
  void playFocusTimerCue(FocusTimer::Cue cue);

  AppState state_ = AppState::Booting;
  AppState standbyReturnState_ = AppState::Paused;
  AppState powerSaveReturnState_ = AppState::Paused;
  AppState powerOffConfirmReturnState_ = AppState::Paused;
  DisplayManager display_;
  AudioManager audio_;
  MotionSensor motion_;
  FocusTimer focusTimer_;
  ReadingLoop reader_;
  ButtonHandler button_;
  ButtonHandler powerButton_;
  TouchHandler touch_;
  StandbyScreensaver screensaver_;
  TextEntryController textEntry_;
  StorageManager storage_;
  IndexedBookStore activeBookStore_;
  RssFeedManager rssFeedManager_;
  CompanionSyncManager companionSync_;
  UsbMassStorageManager usbTransfer_;
  Preferences preferences_;
  ClockController clock_;
  OtaController ota_;
  TimeEstimateEngine timeEstimate_;
  BookLibraryStore library_;
  PausedTouchSession pausedTouch_;
  TouchIntent pausedTouchIntent_ = TouchIntent::None;

  uint32_t bootStartedMs_ = 0;
  uint32_t lastStateLogMs_ = 0;
  uint32_t wpmFeedbackUntilMs_ = 0;
  uint32_t brightnessToastUntilMs_ = 0;
  uint32_t lastProgressSaveMs_ = 0;
  uint32_t lastBatterySampleMs_ = 0;
  uint32_t lastBatteryPowerSourcePollMs_ = 0;
  uint32_t batteryRuntimeAnchorMs_ = 0;
  uint32_t lastScrollAnimationRenderMs_ = 0;
  uint32_t lastBatteryLabelRefreshMs_ = 0;
  uint32_t lastUserActivityMs_ = 0;
  uint32_t lastCompanionSyncRenderMs_ = 0;
  uint32_t lastFocusTimerActionMs_ = 0;
  uint32_t lastReaderTapMs_ = 0;
  uint32_t standbyComboStartedMs_ = 0;
  uint32_t standbyEnteredMs_ = 0;
  uint32_t powerSaveEnteredMs_ = 0;
  uint32_t deepStandbyDelayMs_ = 5UL * 60UL * 1000UL;  // 0 = off; PWR-tap deep standby auto-enter.
  uint32_t chapterTransitionUntilMs_ = 0;
  uint32_t lastLowBatteryWarningMs_ = 0;
  uint32_t batteryWarningRestoreAtMs_ = 0;
  size_t lastSavedWordIndex_ = static_cast<size_t>(-1);
  // Per-book reading-session stats, reset on book load, used by the completion screen.
  uint32_t readingSessionMs_ = 0;
  uint32_t playingStartedMs_ = 0;
  size_t wordsReadThisSession_ = 0;
  // Lifetime totals (persisted in NVS), shown on the Reading stats screen.
  uint32_t lifetimeWordsRead_ = 0;
  uint64_t lifetimeReadMs_ = 0;
  uint32_t lifetimeBooksFinished_ = 0;
  bool lifetimeStatsDirty_ = false;
  // Reading streak: consecutive calendar days with reading (needs a valid RTC).
  uint32_t streakDays_ = 0;
  int32_t streakLastDay_ = 0;  // day-number of the last reading day (0 = none)
  std::vector<String> readingStatsItems_;
  size_t readingStatsSelectedIndex_ = 0;
  size_t contextPreviewStartIndex_ = 0;
  size_t contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
  size_t currentBookIndex_ = 0;
  size_t pendingBootBookIndex_ = 0;
  size_t menuSelectedIndex_ = 0;
  size_t settingsSelectedIndex_ = 0;
  size_t wifiNetworkSelectedIndex_ = 0;
  size_t bookPickerSelectedIndex_ = 0;
  size_t chapterPickerSelectedIndex_ = 0;
  size_t chapterTransitionIndex_ = static_cast<size_t>(-1);
  size_t restartConfirmSelectedIndex_ = 0;
  size_t sdCardRepairConfirmSelectedIndex_ = 0;
  size_t updateConfirmSelectedIndex_ = 0;
  size_t powerOffConfirmSelectedIndex_ = 0;
  size_t focusTimerPresetSelectedIndex_ = 0;
  uint8_t brightnessLevelIndex_ = 4;
  uint8_t readerFontSizeIndex_ = 0;
  uint16_t pacingLongWordDelayMs_ = 200;
  uint16_t pacingComplexWordDelayMs_ = 200;
  uint16_t pacingPunctuationDelayMs_ = 200;
  size_t typographyTuningSelectedIndex_ = 1;
  size_t typographyPreviewSampleIndex_ = 0;
  MenuScreen menuScreen_ = MenuScreen::Main;
  MenuScreen textEntryReturnScreen_ = MenuScreen::Main;
  MenuScreen restartConfirmReturnScreen_ = MenuScreen::Main;
  MenuScreen powerOffConfirmReturnScreen_ = MenuScreen::Main;
  std::vector<String> settingsMenuItems_;
  std::vector<String> focusTimerPresetMenuItems_;
  std::vector<DisplayManager::LibraryItem> wifiNetworkMenuItems_;
  std::vector<DisplayManager::LibraryItem> bookMenuItems_;
  std::vector<size_t> bookPickerBookIndices_;
  std::vector<String> chapterMenuItems_;
  std::vector<ChapterMarker> chapterMarkers_;
  std::vector<size_t> paragraphStarts_;
  std::vector<DisplayManager::ContextWord> contextPreviewWords_;
  std::vector<WifiNetworkInfo> wifiNetworks_;
  String bootReason_;  // DEBUG: reset+wake cause shown on the boot splash
  String currentBookPath_;
  String currentBookTitle_;
  String batteryLabel_;
  float batteryFilteredVoltage_ = 0.0f;
  float batteryFilteredPercent_ = 0.0f;
  uint8_t batteryDisplayedPercent_ = 0;
  uint8_t batteryRuntimeAnchorPercent_ = 0;
  uint32_t batteryRuntimeMinutesRemaining_ = 0;
  uint16_t lastReaderTapX_ = 0;
  uint16_t lastReaderTapY_ = 0;
  uint32_t lastMenuTapMs_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint16_t lastMenuTapX_ = 0;
  uint16_t lastMenuTapY_ = 0;
  bool lastMenuTapValid_ = false;
  bool touchInitialized_ = false;
  bool touchPlayHeld_ = false;
  bool playLocked_ = false;
  bool pauseAtSentenceEndRequested_ = false;
  bool lastReaderTapValid_ = false;
  bool bootButtonReleasedSinceBoot_ = false;
  bool bootButtonLongPressHandled_ = false;
  bool powerButtonReleasedSinceBoot_ = false;
  bool powerButtonLongPressHandled_ = false;
  bool powerOffStarted_ = false;
  bool standbyComboActive_ = false;
  bool standbyComboHandled_ = false;
  bool standbyButtonsReleased_ = false;
  bool chapterTransitionVisible_ = false;
  bool batteryWarningOverlayVisible_ = false;
  bool focusTimerCancelHoldTriggered_ = false;
  bool focusTimerChimeEnabled_ = true;
  bool orientationLockEnabled_ = false;  // false = app-wide 180 auto-rotate active
  bool autoFlip180_ = false;             // current IMU-driven 180 flip state
  bool autoFlipCandidate_ = false;
  uint32_t autoFlipCandidateSinceMs_ = 0;
  uint32_t lastOrientLogMs_ = 0;
  bool contextViewVisible_ = false;
  bool contextPreviewWindowValid_ = false;
  bool wpmFeedbackVisible_ = false;
  bool brightnessToastVisible_ = false;
  bool autoDimActive_ = false;
  bool cachedOtaAutoCheck_ = false;
  bool otaCheckDeferredForClock_ = false;  // OTA auto-check waiting on a boot clock sync
  uint32_t cpuMhzPlay_ = 160;
  uint32_t cpuMhzScroll_ = 160;
  uint32_t cpuMhzPaused_ = 80;
  uint32_t cpuMhzMenu_ = 80;
  uint32_t cpuMhzStandby_ = 80;
  uint8_t autoDimBrightnessPercent_ = 10;
  uint32_t autoDimDelayMs_ = 60000;
  bool usingStorageBook_ = false;
  bool storageReady_ = false;
  bool pendingBootBookLoad_ = false;
  bool pendingBootBookLegacyFallback_ = false;
  bool batteryPresent_ = false;
  bool batteryCharging_ = false;
  bool batteryVbusPresent_ = false;
  bool batteryVbusSampleInitialized_ = false;
  bool batterySampleInitialized_ = false;
  bool batteryRuntimeEstimateReady_ = false;
  uint8_t batteryCriticalSampleCount_ = 0;
  bool phantomWordsEnabled_ = true;
  bool readerBatteryVisibleWhilePlaying_ = true;
  bool readerChapterVisibleWhilePlaying_ = false;
  bool readerProgressVisibleWhilePlaying_ = false;
  bool chapterLabelEnabled_ = true;
  FooterMetricMode footerMetricMode_ = FooterMetricMode::Percentage;
  BatteryLabelMode batteryLabelMode_ = BatteryLabelMode::Percent;
  PauseMode pauseMode_ = PauseMode::SentenceEnd;
  bool darkMode_ = true;
  bool nightMode_ = false;
  UiLanguage uiLanguage_ = UiLanguage::English;
  ReaderMode readerMode_ = ReaderMode::Rsvp;
  HandednessMode handednessMode_ = HandednessMode::Right;
  DisplayManager::TypographyConfig typographyConfig_;
};
