#include "app/App.h"

#include <esp32-hal-tinyusb.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_log.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include "board/BoardConfig.h"

#ifndef RSVP_USB_TRANSFER_ENABLED
#define RSVP_USB_TRANSFER_ENABLED 0
#endif

#ifndef RSVP_USB_TRANSFER_AUTO_START
#define RSVP_USB_TRANSFER_AUTO_START 0
#endif

static const char *kAppTag = "app";
constexpr uint32_t kBootSplashMs = 750;
constexpr uint32_t kWpmFeedbackMs = 900;
constexpr uint32_t kBrightnessToastMs = 1500;
constexpr uint32_t kPowerOffHoldMs = 1600;
constexpr uint32_t kPowerOffReleaseWaitMs = 4000;
constexpr uint32_t kBatterySampleIntervalMs = 180000;
constexpr uint32_t kTouchPlayHoldMs = 420;
constexpr uint32_t kPreviewBrowseHoldMs = 240;
constexpr uint32_t kReaderDoubleTapWindowMs = 520;
constexpr uint32_t kThemeToggleHoldMs = 900;
constexpr uint32_t kScrollAnimationFrameMs = 16;
constexpr uint16_t kSwipeThresholdPx = 40;
constexpr uint16_t kAxisBiasPx = 12;
constexpr uint16_t kTapSlopPx = 26;
constexpr uint16_t kReaderDoubleTapSlopPx = 92;
constexpr uint16_t kPreviousSentenceTapWidthPx = 96;
constexpr uint16_t kPreviousSentenceTapHeightPx = 60;
constexpr uint16_t kFooterMetricTapWidthPx = 220;
constexpr uint16_t kFooterMetricTapHeightPx = 32;
constexpr uint16_t kBatteryBadgeTapWidthPx = 160;
constexpr uint16_t kBatteryBadgeTapHeightPx = 40;
constexpr uint16_t kScrubStepPx = 22;
constexpr uint16_t kBrowseNeutralZonePx = 14;
constexpr uint16_t kFocusTimerCancelHoldMaxDriftPx = 20;
constexpr int kMaxScrubStepsPerGesture = 96;
constexpr uint32_t kBrowseMinWordsPerSecondPermille = 4000;
constexpr uint32_t kBrowseMaxWordsPerSecondPermille = 72000;
constexpr uint32_t kFocusTimerCancelHoldMs = 850;
constexpr uint32_t kFocusTimerActionCooldownMs = 1400;
constexpr uint32_t kPowerButtonActionCooldownMs = 1100;
constexpr size_t kContextPreviewWindowWords = 288;
constexpr size_t kContextPreviewAnchorLeadWords = 112;
constexpr size_t kContextPreviewMaxParagraphSnapWords = 48;
constexpr uint32_t kProgressSaveIntervalMs = 15000;
constexpr uint32_t kUsbTransferExitHoldMs = 1200;
constexpr uint32_t kNominalBatteryRuntimeMinutes = 450;  // ~7.5h with CPU frequency scaling (Balanced)
constexpr uint8_t kBatteryDisplayHysteresisPercent = 2;
constexpr uint8_t kBatteryRuntimeMinDropPercent = 3;
constexpr uint32_t kBatteryRuntimeMinElapsedMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kBatteryPowerSourcePollIntervalMs = 1000;
constexpr uint32_t kBatteryPlayingSampleIntervalMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kBatteryLowSampleIntervalMs = 60UL * 1000UL;
constexpr uint32_t kBatteryLowWarningRepeatMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kBatteryWarningVisibleMs = 2500;
constexpr uint32_t kBatteryShutdownNoticeMs = 1500;
constexpr float kBatteryLowWarningVoltage = 3.50f;
constexpr float kBatteryCriticalVoltage = 3.30f;
constexpr uint8_t kBatteryLowWarningPercent = 5;
constexpr uint8_t kBatteryCriticalPercent = 1;
constexpr uint8_t kBatteryCriticalConsecutiveSamples = 2;
constexpr uint32_t kStandbyWakeGraceMs = 900;
// Idle timeout that drops the AMOLED board into the standby screensaver. The
// board has a single button, so there is no PWR+BOOT combo to enter standby;
// it auto-enters after this much inactivity from Paused/Menu and wakes on any
// touch or BOOT press.
constexpr uint32_t kIdleStandbyTimeoutMs = 3UL * 60UL * 1000UL;  // 3 minutes
constexpr uint32_t kChapterTransitionMs = 1400;
constexpr uint32_t kBatteryLabelRefreshIntervalMs = 60000;  // re-display runtime every 60s
constexpr uint8_t kBrightnessLevels[] = {40, 55, 70, 85, 100};
constexpr uint8_t kNightBrightnessLevels[] = {35, 40, 45, 50, 55};
constexpr size_t kBrightnessLevelCount = sizeof(kBrightnessLevels) / sizeof(kBrightnessLevels[0]);

namespace {

enum MenuItem : size_t {
  MenuResume,
  MenuChapters,
  MenuBooks,
  MenuArticles,
  MenuReadingStats,
  MenuFocusTimer,
  MenuSettings,
  MenuSdCardCheck,
  MenuRssFeeds,
  MenuCompanionSync,
#if RSVP_USB_TRANSFER_ENABLED
  MenuUsbTransfer,
#endif
  MenuPowerOff,
  MenuItemCount,
};

enum SettingsItem : size_t {
  SettingsBack,
  SettingsDisplay,
  SettingsTypography,
  SettingsWordPacing,
  SettingsHandedness,
  SettingsBrightness,
  SettingsTheme,
  SettingsPhantomWords,
  SettingsFontSize,
  SettingsLongWords,
  SettingsComplexWords,
  SettingsPunctuation,
  SettingsReset,
  SettingsItemCount,
};

enum TypographyTuningItem : size_t {
  TypographyTuningBack,
  TypographyTuningFontSize,
  TypographyTuningTypeface,
  TypographyTuningPhantomWords,
  TypographyTuningFocusHighlight,
  TypographyTuningTracking,
  TypographyTuningAnchor,
  TypographyTuningGuideWidth,
  TypographyTuningGuideGap,
  TypographyTuningReset,
  TypographyTuningItemCount,
};

enum RestartConfirmItem : size_t {
  RestartConfirmNo,
  RestartConfirmYes,
  RestartConfirmItemCount,
};

enum SdCardRepairConfirmItem : size_t {
  SdCardRepairConfirmNo,
  SdCardRepairConfirmYes,
  SdCardRepairConfirmItemCount,
};

enum UpdateConfirmItem : size_t {
  UpdateConfirmSkip,
  UpdateConfirmUpdate,
  UpdateConfirmItemCount,
};

enum PowerOffConfirmItem : size_t {
  PowerOffConfirmNo,
  PowerOffConfirmYes,
  PowerOffConfirmItemCount,
};

constexpr size_t kRestartConfirmHeaderRows = 1;
constexpr size_t kSdCardRepairConfirmHeaderRows = 1;
constexpr size_t kUpdateConfirmHeaderRows = 2;
constexpr size_t kPowerOffConfirmHeaderRows = 1;
constexpr size_t kSettingsBackIndex = 0;
constexpr size_t kSettingsHomePacingIndex = 1;
constexpr size_t kSettingsHomeDisplayIndex = 2;
constexpr size_t kSettingsHomeTypographyIndex = 3;
constexpr size_t kSettingsHomeSoundIndex = 4;
constexpr size_t kSettingsHomeWifiIndex = 5;
constexpr size_t kSettingsHomeBatteryIndex = 6;
constexpr size_t kSettingsHomeClockIndex = 7;
constexpr size_t kSettingsHomeUpdateIndex = 8;
constexpr size_t kSettingsHomeBootloaderIndex = 9;
constexpr size_t kSettingsHomeFirmwareVersionIndex = 10;
// Settings > Clock page rows.
constexpr size_t kSettingsClockSyncIndex = 1;
constexpr size_t kSettingsClockAutoIndex = 2;
constexpr size_t kSettingsClockTimezoneIndex = 3;
constexpr size_t kSettingsClockYearIndex = 4;
constexpr size_t kSettingsClockMonthIndex = 5;
constexpr size_t kSettingsClockDayIndex = 6;
constexpr size_t kSettingsClockHourIndex = 7;
constexpr size_t kSettingsClockMinuteIndex = 8;
constexpr size_t kSettingsClockStatusIndex = 9;
constexpr size_t kSettingsDisplayThemeIndex = 1;
constexpr size_t kSettingsDisplayBrightnessIndex = 2;
constexpr size_t kSettingsDisplayHandednessIndex = 3;
constexpr size_t kSettingsDisplayChapterLabelIndex = 4;
constexpr size_t kSettingsDisplayFooterIndex = 5;
constexpr size_t kSettingsDisplayBatteryIndex = 6;
constexpr size_t kSettingsDisplayScreensaverIndex = 7;
constexpr size_t kSettingsDisplayReaderBatteryIndex = 8;
constexpr size_t kSettingsDisplayReaderChapterIndex = 9;
constexpr size_t kSettingsDisplayReaderProgressIndex = 10;
constexpr size_t kSettingsDisplayLanguageIndex = 11;
constexpr size_t kSettingsDisplayOrientLockIndex = 12;
constexpr size_t kSettingsPacingReadingModeIndex = 1;
constexpr size_t kSettingsPacingPauseModeIndex = 2;
constexpr size_t kSettingsPacingWpmIndex = 3;
constexpr size_t kSettingsPacingLongWordsIndex = 4;
constexpr size_t kSettingsPacingComplexityIndex = 5;
constexpr size_t kSettingsPacingPunctuationIndex = 6;
constexpr size_t kSettingsPacingResetIndex = 7;
constexpr size_t kSettingsSoundVolumeIndex = 1;
constexpr size_t kSettingsSoundTimerChimeIndex = 2;
constexpr size_t kWifiSettingsNetworkIndex = 1;
constexpr size_t kWifiSettingsChooseIndex = 2;
constexpr size_t kWifiSettingsAutoUpdateIndex = 3;
constexpr size_t kWifiSettingsForgetIndex = 4;

constexpr size_t kSettingsBatteryCpuPlayIndex = 1;
constexpr size_t kSettingsBatteryCpuScrollIndex = 2;
constexpr size_t kSettingsBatteryCpuPausedIndex = 3;
constexpr size_t kSettingsBatteryCpuMenuIndex = 4;
constexpr size_t kSettingsBatteryCpuStandbyIndex = 5;
constexpr size_t kSettingsBatteryAutoDimDelayIndex = 6;
constexpr size_t kSettingsBatteryAutoDimLevelIndex = 7;
constexpr size_t kSettingsBatteryStandbyDelayIndex = 8;  // AMOLED only

constexpr size_t kBookPickerBackIndex = 0;
constexpr size_t kChapterPickerBackIndex = 0;
constexpr size_t kChapterPickerFallbackIndex = 1;
constexpr size_t kWifiNetworksBackIndex = 0;
constexpr size_t kWifiNetworksFirstItemIndex = 1;
constexpr size_t kFocusTimerPresetBackIndex = 0;
constexpr size_t kFocusTimerPresetFirstIndex = 1;
constexpr const char *kPrefsNamespace = "rsvp";
constexpr const char *kPrefBookPath = "book";
constexpr const char *kPrefWpm = "wpm";
constexpr const char *kPrefBrightness = "bright";
constexpr const char *kPrefDarkMode = "dark";
constexpr const char *kPrefNightMode = "night";
constexpr const char *kPrefUiLanguage = "ui_lang";
constexpr const char *kPrefReaderMode = "read_mode";
constexpr const char *kPrefHandedness = "handed";
constexpr const char *kPrefPhantomWords = "phantom_on";
constexpr const char *kPrefFooterMetricMode = "prog_md";
constexpr const char *kPrefBatteryLabelMode = "bat_md";
constexpr const char *kPrefScreensaverMode = "scrn_sv";
constexpr const char *kPrefReaderBatteryVisible = "read_bat";
constexpr const char *kPrefReaderChapterVisible = "read_ch";
constexpr const char *kPrefReaderProgressVisible = "read_pct";
constexpr const char *kPrefChapterLabelEnabled = "ch_lbl_on";   // legacy key (unused)
constexpr const char *kPrefChapterLabelRsvp = "ch_lbl_rsvp";   // default true
constexpr const char *kPrefChapterLabelScroll = "ch_lbl_scroll"; // default false
constexpr const char *kPrefReaderFontSize = "font_size";
constexpr const char *kPrefReaderTypeface = "typeface";
constexpr const char *kPrefTypographyFocusHighlight = "type_hlt";
constexpr const char *kPrefLegacyPacingLong = "pace_len";
constexpr const char *kPrefLegacyPacingComplex = "pace_cpx";
constexpr const char *kPrefLegacyPacingPunctuation = "pace_pnc";
constexpr const char *kPrefPacingLongMs = "pace_lms";
constexpr const char *kPrefPacingComplexMs = "pace_cms";
constexpr const char *kPrefPacingPunctuationMs = "pace_pms";
constexpr const char *kPrefPauseMode = "pause_md";
constexpr const char *kPrefAccurateTime = "time_est_a";
constexpr const char *kPrefTypographyTracking = "type_trk";
constexpr const char *kPrefTypographyAnchor = "type_anc";
constexpr const char *kPrefTypographyGuideWidth = "type_wid";
constexpr const char *kPrefTypographyGuideGap = "type_gap";
constexpr const char *kPrefLifetimeWords = "lt_words";
constexpr const char *kPrefLifetimeMs = "lt_ms";
constexpr const char *kPrefLifetimeBooks = "lt_books";
constexpr const char *kPrefStreakDays = "streak";
constexpr const char *kPrefStreakLastDay = "streak_day";
uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 31;
  }
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    return 29;
  }
  return kDays[month - 1];
}
constexpr const char *kPrefWifiSsid = "wifi_ssid";
constexpr const char *kPrefWifiPass = "wifi_pass";
constexpr const char *kPrefOtaAuto = "ota_auto";
constexpr const char *kPrefCpuPlay = "cpu_play";
constexpr const char *kPrefCpuScroll = "cpu_scroll";
constexpr const char *kPrefCpuPaused = "cpu_paused";
constexpr const char *kPrefCpuMenu = "cpu_menu";
constexpr const char *kPrefCpuStandby = "cpu_stby";
constexpr const char *kPrefAutoDimLevel = "dim_lvl";
constexpr const char *kPrefAutoDimDelay = "dim_dly";
constexpr const char *kPrefDeepStandbyDelay = "ps_dly";
// One packed uint32 per preset: work<<24 | break<<16 | rounds<<8 | longBreak.
constexpr const char *kPrefTimerPresetConfig[FocusTimer::kPresetCount] = {
    "tmr_p0",  // Classic
    "tmr_p1",  // Deep
    "tmr_p2",  // Quick
    "tmr_p3",  // Custom
};
constexpr const char *kPrefTimerChime = "tmr_chime";      // legacy bool
constexpr const char *kPrefTimerChimeStyle = "tmr_chm";   // FocusTimerChime
constexpr const char *kPrefSoundVolume = "snd_vol";       // 0-100 percent
constexpr const char *kPrefOrientLock = "orient_lock";  // bool, default off (auto-rotate on)
constexpr const char *kPrefScrollFontSize = "psc_font";
constexpr const char *kPrefScrollLetterSpacing = "psc_lspc";
constexpr const char *kPrefScrollWordSpacing = "psc_wspc";
constexpr size_t kReaderFontSizeCount = 3;
constexpr size_t kPhantomBeforeCharTargets[] = {64, 96, 144};
constexpr size_t kPhantomAfterCharTargets[] = {96, 144, 208};
constexpr uint16_t kPacingDelayMinMs = 0;
constexpr uint16_t kPacingDelayMaxMs = 600;
constexpr uint16_t kPacingDelayStepMs = 50;
constexpr uint16_t kDefaultPacingDelayMs = 200;
constexpr uint16_t kSettingsWpmMin = 10;
constexpr uint16_t kSettingsWpmLowMax = 100;
constexpr uint16_t kSettingsWpmLowStep = 10;
constexpr uint16_t kSettingsWpmMax = 1000;
constexpr uint16_t kSettingsWpmHighStep = 25;
constexpr int8_t kTypographyTrackingMin = -2;
constexpr int8_t kTypographyTrackingMax = 3;
constexpr uint8_t kTypographyAnchorMin = 30;
constexpr uint8_t kTypographyAnchorMax = 40;
constexpr uint8_t kLeftHandAnchorOffset = 20;
constexpr uint8_t kLeftHandAnchorMin = kTypographyAnchorMin + kLeftHandAnchorOffset;
constexpr uint8_t kLeftHandAnchorMax = kTypographyAnchorMax + kLeftHandAnchorOffset;
constexpr uint8_t kTypographyGuideWidthMin = 12;
constexpr uint8_t kTypographyGuideWidthMax = 30;
constexpr uint8_t kTypographyGuideWidthStep = 2;
constexpr uint8_t kTypographyGuideGapMin = 2;
constexpr uint8_t kTypographyGuideGapMax = 8;
constexpr const char *kTypographyPreviewWords[] = {
    "minimum",
    "encyclopaedia",
    "state-of-the-art",
    "HTTP/2",
    "well-known",
    "rhythms",
    "illumination",
    "WAVEFORM",
    "I",
};
constexpr size_t kTypographyPreviewWordCount =
    sizeof(kTypographyPreviewWords) / sizeof(kTypographyPreviewWords[0]);
constexpr size_t kWifiPasswordMaxLength = 63;

void logApp(const char *message) {
  ESP_LOGI(kAppTag, "%s", message);
  Serial.printf("[app] %s\n", message);
}

String displayNameForPath(const String &path) {
  const int separator = path.lastIndexOf('/');
  String name = separator >= 0 ? path.substring(separator + 1) : path;
  String lowered = name;
  lowered.toLowerCase();
  if (lowered.endsWith(".txt")) {
    name.remove(name.length() - 4);
  }
  if (lowered.endsWith(".rsvp")) {
    name.remove(name.length() - 5);
  }
  return name;
}

int clampIntSetting(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

int nextCyclicSetting(int value, int minValue, int maxValue, int step = 1) {
  step = std::max(1, step);
  const int normalized = clampIntSetting(value, minValue, maxValue);
  int next = normalized + step;
  if (next > maxValue) {
    next = minValue;
  }
  return next;
}

uint16_t nextReaderWpmSetting(uint16_t value) {
  int normalized = clampIntSetting(value, kSettingsWpmMin, kSettingsWpmMax);
  if (normalized < static_cast<int>(kSettingsWpmLowMax)) {
    normalized += kSettingsWpmLowStep;
    if (normalized > static_cast<int>(kSettingsWpmLowMax)) {
      normalized = kSettingsWpmLowMax;
    }
    return static_cast<uint16_t>(normalized);
  }

  int next = normalized + kSettingsWpmHighStep;
  if (next > static_cast<int>(kSettingsWpmMax)) {
    next = kSettingsWpmMin;
  }
  return static_cast<uint16_t>(next);
}

DisplayManager::TypographyConfig defaultTypographyConfig() {
  return DisplayManager::TypographyConfig();
}

bool wifiNetworkRequiresPassword(uint8_t authMode) {
  return static_cast<wifi_auth_mode_t>(authMode) != WIFI_AUTH_OPEN;
}

String wifiSecurityLabel(uint8_t authMode) {
  return wifiNetworkRequiresPassword(authMode) ? "Secure" : "Open";
}

String storedOrFallbackLabel(const String &value, const String &fallback) {
  return value.isEmpty() ? fallback : value;
}

bool sdCardFolderRepairNeeded(const StorageManager::DiagnosticResult &result) {
  return result.mounted &&
         (!result.booksDirectory || !result.bookFilesDirectory ||
          !result.articleFilesDirectory || !result.configDirectory);
}

DisplayManager::ReaderTypeface readerTypefaceFromSetting(uint8_t value) {
  switch (static_cast<DisplayManager::ReaderTypeface>(value)) {
    case DisplayManager::ReaderTypeface::Standard:
    case DisplayManager::ReaderTypeface::OpenDyslexic:
    case DisplayManager::ReaderTypeface::AtkinsonHyperlegible:
      return static_cast<DisplayManager::ReaderTypeface>(value);
  }
  return DisplayManager::ReaderTypeface::Standard;
}

DisplayManager::ReaderTypeface nextReaderTypeface(DisplayManager::ReaderTypeface current) {
  switch (readerTypefaceFromSetting(static_cast<uint8_t>(current))) {
    case DisplayManager::ReaderTypeface::Standard:
      return DisplayManager::ReaderTypeface::AtkinsonHyperlegible;
    case DisplayManager::ReaderTypeface::AtkinsonHyperlegible:
      return DisplayManager::ReaderTypeface::OpenDyslexic;
    case DisplayManager::ReaderTypeface::OpenDyslexic:
    default:
      return DisplayManager::ReaderTypeface::Standard;
  }
}

App::ReaderMode readerModeFromSetting(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(App::ReaderMode::Scroll):
    case 2:  // Migrate the removed word-scroll mode to page scroll.
      return App::ReaderMode::Scroll;
    case static_cast<uint8_t>(App::ReaderMode::Rsvp):
    default:
      return App::ReaderMode::Rsvp;
  }
}

App::HandednessMode handednessModeFromSetting(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(App::HandednessMode::Left):
      return App::HandednessMode::Left;
    case static_cast<uint8_t>(App::HandednessMode::Right):
    default:
      return App::HandednessMode::Right;
  }
}

App::HandednessMode nextHandednessMode(App::HandednessMode current) {
  switch (handednessModeFromSetting(static_cast<uint8_t>(current))) {
    case App::HandednessMode::Left:
      return App::HandednessMode::Right;
    case App::HandednessMode::Right:
    default:
      return App::HandednessMode::Left;
  }
}

App::ReaderMode nextReaderMode(App::ReaderMode current) {
  switch (readerModeFromSetting(static_cast<uint8_t>(current))) {
    case App::ReaderMode::Rsvp:
      return App::ReaderMode::Scroll;
    case App::ReaderMode::Scroll:
    default:
      return App::ReaderMode::Rsvp;
  }
}

uint16_t pacingDelayMsForLegacyLevel(uint8_t levelIndex) {
  constexpr uint16_t kLegacyPacingDelayMs[] = {100, 150, 200, 250, 300};
  constexpr size_t kLegacyPacingLevelCount =
      sizeof(kLegacyPacingDelayMs) / sizeof(kLegacyPacingDelayMs[0]);

  if (levelIndex >= kLegacyPacingLevelCount) {
    levelIndex = 2;
  }
  return kLegacyPacingDelayMs[levelIndex];
}

uint16_t loadPacingDelayMs(Preferences &preferences, const char *key, const char *legacyKey) {
  if (preferences.isKey(key)) {
    return static_cast<uint16_t>(
        clampIntSetting(preferences.getUShort(key, kDefaultPacingDelayMs), kPacingDelayMinMs,
                        kPacingDelayMaxMs));
  }

  if (preferences.isKey(legacyKey)) {
    const uint16_t migratedDelayMs =
        pacingDelayMsForLegacyLevel(preferences.getUChar(legacyKey, 2));
    preferences.putUShort(key, migratedDelayMs);
    return migratedDelayMs;
  }

  return kDefaultPacingDelayMs;
}

}  // namespace

App::App()
    : button_(BoardConfig::PIN_BOOT_BUTTON),
      powerButton_(BoardConfig::PIN_PWR_BUTTON),
      screensaver_(
          display_, [this]() { return static_cast<uint32_t>(reader_.currentIndex()); },
          [this]() { return batteryDisplayedPercent_; }),
      textEntry_(
          display_, [this](uint32_t nowMs) { submitWifiPassword(nowMs); },
          [this]() {
            menuScreen_ = textEntryReturnScreen_;
            textEntry_.close();
            renderMenu();
          }),
      clock_(
          display_, preferences_,
          [this](const char *label, uint32_t nowMs) { return ota_.blockForCheck(label, nowMs); },
          [this](String &ssid, String &password) {
            const OtaUpdater::Config cfg = preferredOtaConfig();
            if (cfg.wifiSsid.isEmpty()) {
              return false;
            }
            ssid = cfg.wifiSsid;
            password = cfg.wifiPassword;
            return true;
          },
          [this]() { updateStreakForToday(); }),
      ota_(
          display_, [this]() { renderMenu(); }, [this]() { saveReadingPosition(true); },
          [this]() { applyStateCpuFrequency(); },
          [this](uint32_t nowMs) {
            if (state_ == AppState::Menu &&
                (menuScreen_ == MenuScreen::SettingsHome ||
                 menuScreen_ == MenuScreen::SettingsDisplay ||
                 menuScreen_ == MenuScreen::SettingsPacing ||
                 menuScreen_ == MenuScreen::SettingsBattery ||
                 menuScreen_ == MenuScreen::SettingsSound ||
                 menuScreen_ == MenuScreen::SettingsClock ||
                 menuScreen_ == MenuScreen::WifiSettings)) {
              rebuildSettingsMenuItems();
              renderSettings();
            } else {
              menuScreen_ = MenuScreen::Main;
              setState(AppState::Paused, nowMs);
            }
          },
          [this](const char *title, const char *line1, const char *line2, int percent) {
            renderStorageStatus(title, line1, line2, percent);
          }),
      timeEstimate_(
          reader_, [this]() { return state_; },
          [this](uint32_t nowMs) { renderActiveReader(nowMs); }, [this]() { renderMenu(); },
          [this](const char *title, const char *line1, const char *line2, int percent) {
            renderStorageStatus(title, line1, line2, percent);
          }),
      library_(preferences_, storage_) {}

void App::setBootReason(int resetReason, int wakeCause) {
  const char *reset = "?";
  switch (resetReason) {
    case ESP_RST_POWERON:  reset = "POWERON"; break;   // fresh power-up (PMU/USB)
    case ESP_RST_SW:       reset = "SW"; break;
    case ESP_RST_PANIC:    reset = "PANIC"; break;
    case ESP_RST_INT_WDT:  reset = "INT_WDT"; break;
    case ESP_RST_TASK_WDT: reset = "TASK_WDT"; break;
    case ESP_RST_WDT:      reset = "WDT"; break;
    case ESP_RST_DEEPSLEEP: reset = "DEEPSLEEP"; break; // returned from deep sleep
    case ESP_RST_BROWNOUT: reset = "BROWNOUT"; break;
    case ESP_RST_EXT:      reset = "EXT"; break;
    default: break;
  }
  const char *wake = "none";
  switch (wakeCause) {
    case ESP_SLEEP_WAKEUP_EXT0:  wake = "EXT0"; break;  // our ext0(GPIO18) fallback
    case ESP_SLEEP_WAKEUP_EXT1:  wake = "EXT1"; break;
    case ESP_SLEEP_WAKEUP_GPIO:  wake = "GPIO"; break;
    case ESP_SLEEP_WAKEUP_TIMER: wake = "TIMER"; break;
    case ESP_SLEEP_WAKEUP_UNDEFINED: wake = "none"; break;
    default: break;
  }
  bootReason_ = String("reset:") + reset + " wake:" + wake;
  Serial.printf("[boot] %s\n", bootReason_.c_str());
}

void App::begin() {
  BoardConfig::begin();
  button_.begin();
  powerButton_.begin();
  bootButtonReleasedSinceBoot_ = !button_.isHeld();
  bootButtonLongPressHandled_ = false;
  powerButtonReleasedSinceBoot_ = !powerButton_.isHeld();
  powerButtonLongPressHandled_ = false;
  storage_.setStatusCallback(&App::handleStorageStatus, this);
  preferences_.begin(kPrefsNamespace, false);
  loadLifetimeStats();
  clock_.loadSettings();
  brightnessLevelIndex_ = preferences_.getUChar(kPrefBrightness, brightnessLevelIndex_);
  if (brightnessLevelIndex_ >= kBrightnessLevelCount) {
    brightnessLevelIndex_ = kBrightnessLevelCount - 1;
  }
  {
    const uint8_t storedChime =
        preferences_.getUChar(kPrefTimerChimeStyle, static_cast<uint8_t>(focusTimerChime_));
    if (storedChime <= static_cast<uint8_t>(FocusTimerChime::Startup)) {
      focusTimerChime_ = static_cast<FocusTimerChime>(storedChime);
    }
    if (!preferences_.isKey(kPrefTimerChimeStyle) && preferences_.isKey(kPrefTimerChime) &&
        !preferences_.getBool(kPrefTimerChime, true)) {
      focusTimerChime_ = FocusTimerChime::Off;
    }
  }
  soundVolumePercent_ =
      static_cast<uint8_t>(clampIntSetting(preferences_.getUChar(kPrefSoundVolume, soundVolumePercent_),
                                           0, 100));
  audio_.setVolumePercent(soundVolumePercent_);
  orientationLockEnabled_ = preferences_.getBool(kPrefOrientLock, orientationLockEnabled_);
  phantomWordsEnabled_ = preferences_.getBool(kPrefPhantomWords, phantomWordsEnabled_);
  readerBatteryVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderBatteryVisible, readerBatteryVisibleWhilePlaying_);
  readerChapterVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderChapterVisible, readerChapterVisibleWhilePlaying_);
  readerProgressVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderProgressVisible, readerProgressVisibleWhilePlaying_);
  uiLanguage_ =
      Localization::sanitizeLanguage(preferences_.getUChar(
          kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_)));
  readerMode_ = readerModeFromSetting(
      preferences_.getUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_)));
  // Load chapter label per reading mode (RSVP defaults ON, Scroll defaults OFF)
  chapterLabelEnabled_ = preferences_.getBool(
      chapterLabelPrefKey(), chapterLabelDefaultForMode(readerMode_));
  handednessMode_ = handednessModeFromSetting(
      preferences_.getUChar(kPrefHandedness, static_cast<uint8_t>(handednessMode_)));
  readerFontSizeIndex_ = preferences_.getUChar(kPrefReaderFontSize, readerFontSizeIndex_);
  if (readerFontSizeIndex_ >= kReaderFontSizeCount) {
    readerFontSizeIndex_ = 0;
  }
  switch (preferences_.getUChar(kPrefFooterMetricMode,
                                static_cast<uint8_t>(footerMetricMode_))) {
    case static_cast<uint8_t>(FooterMetricMode::ChapterTime):
      footerMetricMode_ = FooterMetricMode::ChapterTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::BookTime):
      footerMetricMode_ = FooterMetricMode::BookTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::Percentage):
    default:
      footerMetricMode_ = FooterMetricMode::Percentage;
      break;
  }
  switch (preferences_.getUChar(kPrefBatteryLabelMode,
                                static_cast<uint8_t>(batteryLabelMode_))) {
    case static_cast<uint8_t>(BatteryLabelMode::TimeRemaining):
      batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Voltage):
      batteryLabelMode_ = BatteryLabelMode::Voltage;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Percent):
    default:
      batteryLabelMode_ = BatteryLabelMode::Percent;
      break;
  }
  screensaver_.setMode(StandbyScreensaver::modeFromValue(preferences_.getUChar(
      kPrefScreensaverMode, static_cast<uint8_t>(screensaver_.mode()))));
  switch (preferences_.getUChar(kPrefPauseMode, static_cast<uint8_t>(pauseMode_))) {
    case static_cast<uint8_t>(PauseMode::Instant):
      pauseMode_ = PauseMode::Instant;
      break;
    case static_cast<uint8_t>(PauseMode::SentenceEnd):
    default:
      pauseMode_ = PauseMode::SentenceEnd;
      break;
  }
  {
    constexpr uint32_t kValidMhz[] = {40, 80, 160, 240};
    auto loadCpuMhz = [&](const char *key, uint32_t def) -> uint32_t {
      const uint32_t v = preferences_.getUInt(key, def);
      for (uint32_t m : kValidMhz) {
        if (v == m) return v;
      }
      return def;
    };
    cpuMhzPlay_ = loadCpuMhz(kPrefCpuPlay, cpuMhzPlay_);
    cpuMhzScroll_ = loadCpuMhz(kPrefCpuScroll, cpuMhzScroll_);
    cpuMhzPaused_ = loadCpuMhz(kPrefCpuPaused, cpuMhzPaused_);
    cpuMhzMenu_ = loadCpuMhz(kPrefCpuMenu, cpuMhzMenu_);
    cpuMhzStandby_ = loadCpuMhz(kPrefCpuStandby, cpuMhzStandby_);
  }
  {
    const uint8_t savedDimLevel =
        preferences_.getUChar(kPrefAutoDimLevel, autoDimBrightnessPercent_);
    if (savedDimLevel == 0 || savedDimLevel == 10 || savedDimLevel == 20 || savedDimLevel == 30) {
      autoDimBrightnessPercent_ = savedDimLevel;
    }
  }
  {
    const uint32_t savedDimDelay = preferences_.getUInt(kPrefAutoDimDelay, autoDimDelayMs_);
    if (savedDimDelay == 0 || savedDimDelay == 30000 || savedDimDelay == 60000 ||
        savedDimDelay == 120000) {
      autoDimDelayMs_ = savedDimDelay;
    }
  }
#if defined(BOARD_AMOLED_18)
  {
    const uint32_t savedPs = preferences_.getUInt(kPrefDeepStandbyDelay, deepStandbyDelayMs_);
    if (savedPs == 0 || savedPs == 60000 || savedPs == 180000 || savedPs == 300000 ||
        savedPs == 600000) {
      deepStandbyDelayMs_ = savedPs;
    }
  }
#endif
  pacingLongWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingLongMs, kPrefLegacyPacingLong);
  pacingComplexWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingComplexMs, kPrefLegacyPacingComplex);
  pacingPunctuationDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingPunctuationMs, kPrefLegacyPacingPunctuation);
  timeEstimate_.setAccurateEstimate(true);
  typographyConfig_ = defaultTypographyConfig();
  typographyConfig_.typeface = readerTypefaceFromSetting(
      preferences_.getUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface)));
  typographyConfig_.focusHighlight =
      preferences_.getBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
  typographyConfig_.trackingPx = static_cast<int8_t>(clampIntSetting(
      preferences_.getChar(kPrefTypographyTracking, typographyConfig_.trackingPx),
      kTypographyTrackingMin, kTypographyTrackingMax));
  typographyConfig_.anchorPercent = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent),
      kTypographyAnchorMin, kTypographyAnchorMax));
  typographyConfig_.guideHalfWidth = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth),
      kTypographyGuideWidthMin, kTypographyGuideWidthMax));
  typographyConfig_.guideGap = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap),
      kTypographyGuideGapMin, kTypographyGuideGapMax));
  darkMode_ = preferences_.getBool(kPrefDarkMode, darkMode_);
  nightMode_ = preferences_.getBool(kPrefNightMode, nightMode_);
  cachedOtaAutoCheck_ = otaAutoCheckEnabled();
  applyHandednessSettings(0, false);
  applyDisplayPreferences(0, false);
  applyTypographySettings(0, false);
  applyScrollConfig();
  applyPacingSettings();
  bootStartedMs_ = millis();
  lastStateLogMs_ = bootStartedMs_;
  lastUserActivityMs_ = bootStartedMs_;
  lastScrollAnimationRenderMs_ = 0;
  Serial.printf("[app] version=%s\n", ota_.currentVersion().c_str());

  logApp("Initializing hardware modules");
  const bool displayReady = display_.begin();
  updateBatteryStatus(bootStartedMs_, true);

  if (displayReady) {
    // Boot diagnostics on serial only (reset/wake cause + PMU power state).
    Serial.printf("[boot] %s | %s\n", bootReason_.c_str(),
                  BoardConfig::pmuDebugSummary().c_str());
    BoardConfig::RtcDateTime rtc;
    if (BoardConfig::rtcRead(rtc)) {
      Serial.printf("[boot] RTC present: %04u-%02u-%02u %02u:%02u:%02u valid=%d\n", rtc.year,
                    rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second, rtc.valid ? 1 : 0);
    } else {
      Serial.println("[boot] RTC not responding at 0x51 on touch bus");
    }
    display_.renderCenteredWord("READY");
    logApp("Display init ok");
  } else {
    ESP_LOGE(kAppTag, "Display init failed");
    Serial.println("[app] Display init failed");
  }

  touchInitialized_ = touch_.begin();
  audio_.begin();
  motion_.begin();
  focusTimer_.begin(&motion_);
  loadFocusTimerPreferences();

#if RSVP_USB_TRANSFER_ENABLED && RSVP_USB_TRANSFER_AUTO_START
  state_ = AppState::Booting;
  Serial.println("[app] USB transfer auto-start active");
  enterUsbTransfer(millis());
  return;
#endif

  display_.renderProgress("SD", "Loading books", "Use SD converter for EPUB", 0);
  storageReady_ = storage_.begin();
  const uint16_t savedWpm = preferences_.getUShort(kPrefWpm, reader_.wpm());
  reader_.setWpm(savedWpm);

  pendingBootBookLoad_ = prepareBootBookLoad();
  if (!pendingBootBookLoad_) {
    usingStorageBook_ = false;
    chapterMarkers_.clear();
    paragraphStarts_.clear();
    currentBookPath_ = "";
    currentBookTitle_ = "Demo";
    reader_.begin(bootStartedMs_);
    invalidateContextPreviewWindow();
    timeEstimate_.rebuild(currentBookPath_, currentBookTitle_);
    Serial.println("[app] using built-in demo text");
  } else {
    currentBookTitle_ = storage_.bookDisplayName(pendingBootBookIndex_);
    if (currentBookTitle_.isEmpty()) {
      currentBookTitle_ = "Loading book";
    }
  }

  // Auto clock sync and Auto OTA both need the single Wi-Fi STA, so chain them:
  // if a background clock sync starts now, the OTA check is deferred until it
  // finishes (see the pollSync handling in the main loop).
  maybeAutoSyncClock(bootStartedMs_);
  if (clock_.syncInProgress()) {
    otaCheckDeferredForClock_ = true;
  } else {
    maybeAutoCheckForUpdates(bootStartedMs_);
  }
  Serial.printf("[app] WPM=%u interval=%lu ms\n", reader_.wpm(),
                static_cast<unsigned long>(reader_.wordIntervalMs()));

  state_ = AppState::Booting;
  Serial.println("[app] READY splash active");
}

// ============================================================================
// HARDWARE BUTTON MAP — single source of truth for what each button does.
// All physical-button handling is routed from here; do not poll buttons or add
// button actions elsewhere. Touch gestures live in handleTouch().
//
// AMOLED board (BOARD_AMOLED_18) — 2 buttons:
//   BOOT (GPIO0, handleAmoledButton):
//     tap (release)        -> toggle menu / back one level / resume reading
//     hold >= 900ms        -> cycle brightness
//     tap in USB/Sync page -> exit back to main menu
//   PWR  (AXP2101 PWRKEY, updatePmuPowerKey):
//     tap (short press)    -> ignored (no accidental power-off)
//     hold (long press ~4s)-> power off ("Goodbye" + PMU shutdown)
//     hold while OFF ~2s   -> power on (PMU hardware; BOOT/touch cannot power on)
//
// Bar board (non-AMOLED) — 2 GPIO buttons:
//   BOOT (handleBootButton):  tap -> brightness;  hold >=900ms -> theme
//   PWR  (handlePowerButton): tap -> menu/back/resume;  hold >=1600ms -> power-off
//                             confirm;  hold >=1200ms in focus timer -> cancel
//   BOOT+PWR both held (handleStandbyCombo) -> standby
//
// Standby/Sleeping/Booting/UsbTransfer states gate most actions inside each
// handler; see the per-state guards there.
// ============================================================================
void App::dispatchButtons(uint32_t nowMs) {
  button_.update(nowMs);
  powerButton_.update(nowMs);
#if defined(BOARD_AMOLED_18)
  handleAmoledButton(nowMs);   // BOOT
  updatePmuPowerKey(nowMs);    // PWR (AXP2101 PWRKEY, polled from the PMU)
#else
  if (!handleStandbyCombo(nowMs)) {
    handleBootButton(nowMs);
    handlePowerButton(nowMs);
  }
#endif
}

void App::update(uint32_t nowMs) {
  dispatchButtons(nowMs);
  if (powerOffStarted_) {
    return;
  }

#if defined(BOARD_AMOLED_18)
  if (state_ == AppState::PowerSaving) {
    // Deep standby: display + touch off. dispatchButtons() above already ran
    // updatePmuPowerKey — a PWR tap is the only thing that wakes us. Skip
    // everything else (battery overlay, touch, render) so nothing draws to the
    // sleeping panel until then.
    if (nowMs - lastStateLogMs_ > 1500) {
      lastStateLogMs_ = nowMs;
      ESP_LOGI(kAppTag, "state=%s", stateName(state_));
      Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                    static_cast<unsigned long>(nowMs));
    }
    return;
  }
#endif

  const bool batteryChanged = updateBatteryStatus(nowMs);
  if (powerOffStarted_) {
    return;
  }

  if (batteryWarningOverlayVisible_) {
    updateBatteryWarningOverlay(nowMs);
    if (batteryWarningOverlayVisible_) {
      if (nowMs - lastStateLogMs_ > 1500) {
        lastStateLogMs_ = nowMs;
        ESP_LOGI(kAppTag, "state=%s", stateName(state_));
        Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                      static_cast<unsigned long>(nowMs));
      }
      return;
    }
  }

  if (state_ == AppState::Standby) {
#if defined(BOARD_AMOLED_18)
    handleAmoledStandbyWake(nowMs);
    if (state_ != AppState::Standby) {
      return;  // Woke up this frame; resume normal handling next tick.
    }
    updateDeepStandbyIdle(nowMs);  // Screensaver standby can fall through to deep standby.
    if (state_ != AppState::Standby) {
      return;
    }
#endif
    handleTouch(nowMs);
    screensaver_.update(nowMs);
    if (nowMs - lastStateLogMs_ > 1500) {
      lastStateLogMs_ = nowMs;
      ESP_LOGI(kAppTag, "state=%s", stateName(state_));
      Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                    static_cast<unsigned long>(nowMs));
    }
    return;
  }

  ota_.pollResult(nowMs);
  if (clock_.pollSync(nowMs) && otaCheckDeferredForClock_) {
    otaCheckDeferredForClock_ = false;
    maybeAutoCheckForUpdates(nowMs);  // Wi-Fi is free again now the clock sync is done
  }
  updateState(nowMs);
  loadPendingBootBook(nowMs);
  maybeOpenUpdateConfirm(nowMs);
  updateAutoOrientation(nowMs);
  updateFocusTimer(nowMs);
  updateReader(nowMs);
  handleTouch(nowMs);
  updateWpmFeedback(nowMs);
  updateBrightnessToast(nowMs);
  updateAutoDim(nowMs);
  updateBatteryRuntimeLabel(nowMs);
  maybeSaveReadingPosition(nowMs);
  timeEstimate_.update(nowMs, currentBookPath_);
#if defined(BOARD_AMOLED_18)
  updateDeepStandbyIdle(nowMs);  // deep standby auto-enter (own configurable delay)
  if (state_ == AppState::PowerSaving) {
    return;
  }
  updateIdleStandby(nowMs);
#endif

  if (batteryChanged && (state_ == AppState::Paused || state_ == AppState::Playing)) {
    renderActiveReader(nowMs);
  } else if (batteryChanged && state_ == AppState::Menu) {
    renderMenu();
  }

  if (nowMs - lastStateLogMs_ > 1500) {
    lastStateLogMs_ = nowMs;
    ESP_LOGI(kAppTag, "state=%s", stateName(state_));
    Serial.printf("[app] state=%s ms=%lu\n", stateName(state_),
                  static_cast<unsigned long>(nowMs));
  }
}

const char *App::stateName(AppState state) const {
  switch (state) {
    case AppState::Booting:
      return "Booting";
    case AppState::Paused:
      return "Paused";
    case AppState::Playing:
      return "Playing";
    case AppState::Finished:
      return "Finished";
    case AppState::Menu:
      return "Menu";
    case AppState::CompanionSync:
      return "CompanionSync";
    case AppState::UsbTransfer:
      return "UsbTransfer";
    case AppState::Standby:
      return "Standby";
    case AppState::PowerSaving:
      return "PowerSaving";
    case AppState::Sleeping:
      return "Sleeping";
  }
  return "Unknown";
}

const char *App::touchPhaseName(TouchPhase phase) const {
  switch (phase) {
    case TouchPhase::Start:
      return "Start";
    case TouchPhase::Move:
      return "Move";
    case TouchPhase::End:
      return "End";
  }
  return "Unknown";
}

void App::setState(AppState nextState, uint32_t nowMs) {
  if (nextState == state_) {
    return;
  }

  const AppState previousState = state_;
  if (previousState == AppState::Menu && nextState != AppState::Menu) {
    timeEstimate_.flushPendingRebuild(currentBookPath_, currentBookTitle_);
  }

  // Accumulate active reading time for the completion-screen + lifetime stats.
  if (previousState == AppState::Playing && nowMs >= playingStartedMs_) {
    const uint32_t segment = nowMs - playingStartedMs_;
    readingSessionMs_ += segment;
    lifetimeReadMs_ += segment;
    lifetimeStatsDirty_ = true;
  }
  if (nextState == AppState::Playing) {
    playingStartedMs_ = nowMs;
  }

  if (nextState != AppState::Paused) {
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    contextViewVisible_ = false;
    invalidateContextPreviewWindow();
    wpmFeedbackVisible_ = false;
  }
  if (nextState != AppState::Playing) {
    touchPlayHeld_ = false;
    playLocked_ = false;
    pauseAtSentenceEndRequested_ = false;
    chapterTransitionVisible_ = false;
  }
  if (nextState != AppState::Paused && nextState != AppState::Playing) {
    resetReaderTapTracking();
  }

  state_ = nextState;

  // Any state change counts as user activity and clears auto-dim.
  lastUserActivityMs_ = nowMs;
  if (autoDimActive_) {
    autoDimActive_ = false;
    display_.setBrightnessPercent(currentBrightnessPercent());
  }

  switch (state_) {
    case AppState::Paused:
      renderActiveReader(nowMs);
      break;
    case AppState::Playing:
      reader_.start(nowMs);
      updateStreakForToday();  // reading today counts toward the streak
      renderActiveReader(nowMs);
      break;
    case AppState::Finished:
      renderBookFinished();
      break;
    case AppState::Menu:
      renderMenu();
      break;
    case AppState::CompanionSync:
      display_.renderStatus("Sync", companionSync_.statusLine1(), companionSync_.statusLine2());
      break;
    case AppState::UsbTransfer:
      display_.renderStatus("USB", "Preparing SD", "Eject when done");
      break;
    case AppState::Standby:
      screensaver_.seed(nowMs);
      screensaver_.update(nowMs, true);
      break;
    case AppState::PowerSaving:
      // Screen + touch are already off; nothing to draw.
      break;
    case AppState::Sleeping:
      display_.renderCenteredWord("SLEEP");
      break;
    case AppState::Booting:
      display_.renderCenteredWord("READY");
      break;
  }

  if (state_ == AppState::Paused && previousState == AppState::Playing) {
    saveReadingPosition(true);
  }

  applyStateCpuFrequency();

  ESP_LOGI(kAppTag, "state -> %s", stateName(state_));
  Serial.printf("[app] state -> %s at %lu ms\n", stateName(state_),
                static_cast<unsigned long>(nowMs));
}

void App::applyStateCpuFrequency() {
  // While a background OTA check is running we need the full clock for Wi-Fi/TLS.
  if (ota_.checkInProgress()) {
    if (getCpuFrequencyMhz() != 240) {
      setCpuFrequencyMhz(240);
      Serial.println("[power] CPU -> 240 MHz (OTA active)");
    }
    return;
  }

  uint32_t mhz;
  switch (state_) {
    case AppState::Playing:
      mhz = scrollModeEnabled() ? cpuMhzScroll_ : cpuMhzPlay_;
      break;
    case AppState::Paused:
      // In scroll mode, Paused is the active reading state (manual swipe scrolling).
      mhz = scrollModeEnabled() ? cpuMhzScroll_ : cpuMhzPaused_;
      break;
    case AppState::Finished:
    case AppState::Menu:
      mhz = cpuMhzMenu_;
      break;
    case AppState::Standby:
    case AppState::PowerSaving:
      mhz = cpuMhzStandby_;
      break;
    default:
      // Booting, CompanionSync, UsbTransfer, Sleeping: keep full speed.
      mhz = 240;
      break;
  }

  if (getCpuFrequencyMhz() != mhz) {
    setCpuFrequencyMhz(mhz);
    Serial.printf("[power] CPU -> %u MHz (state=%s)\n", mhz, stateName(state_));
  }
}

void App::updateState(uint32_t nowMs) {
  if (state_ == AppState::Booting) {
    if (nowMs - bootStartedMs_ < kBootSplashMs) {
      return;
    }

    setState((touchPlayHeld_ || playLocked_ || pauseAtSentenceEndRequested_) ? AppState::Playing
                                                                              : AppState::Paused,
             nowMs);
    return;
  }

  if (state_ == AppState::UsbTransfer) {
    updateUsbTransfer(nowMs);
    return;
  }

  if (state_ == AppState::CompanionSync) {
    updateCompanionSync(nowMs);
    return;
  }

  if (state_ == AppState::Finished || state_ == AppState::Menu ||
      state_ == AppState::Standby || state_ == AppState::Sleeping) {
    // Finished, menu, standby, and sleeping state changes are driven by direct
    // input and power events, not the play/pause auto-transition below.
    return;
  }

  if (touchPlayHeld_ || playLocked_ || pauseAtSentenceEndRequested_) {
    setState(AppState::Playing, nowMs);
    return;
  }

  setState(AppState::Paused, nowMs);
}

void App::updateReader(uint32_t nowMs) {
  if (state_ != AppState::Playing) {
    return;
  }

  if (updateChapterTransition(nowMs)) {
    return;
  }

  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }

  if (shouldFinalizeReaderPause(nowMs)) {
    finalizeReaderPause(nowMs);
    return;
  }

  const size_t previousIndex = reader_.currentIndex();
  const bool changed = reader_.update(nowMs, !pauseAtSentenceEndRequested_);
  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }
  if (changed && reader_.currentIndex() > previousIndex) {
    const size_t delta = reader_.currentIndex() - previousIndex;
    wordsReadThisSession_ += delta;
    lifetimeWordsRead_ += static_cast<uint32_t>(delta);
    lifetimeStatsDirty_ = true;
  }
  if (changed && maybeStartChapterTransition(previousIndex, reader_.currentIndex(), nowMs)) {
    return;
  }

  // Real (storage) books stop at the last word; the demo loops by design.
  // Once the final word has been shown for its full duration, finish the book.
  if (usingStorageBook_ && reader_.atEnd()) {
    const uint32_t durationMs = reader_.currentWordDurationMs();
    if (durationMs == 0 || reader_.elapsedInCurrentWordMs(nowMs) >= durationMs) {
      enterBookFinished(nowMs);
      return;
    }
  }

  if (scrollModeEnabled()) {
    if (changed || nowMs - lastScrollAnimationRenderMs_ >= kScrollAnimationFrameMs) {
      renderScrollReader(nowMs);
      lastScrollAnimationRenderMs_ = nowMs;
    }
    return;
  }

  if (changed) {
    renderReaderWord();
  }
}

void App::maybeSaveReadingPosition(uint32_t nowMs) {
  if (!usingStorageBook_ || currentBookPath_.isEmpty() || state_ != AppState::Playing) {
    return;
  }

  if (nowMs - lastProgressSaveMs_ < kProgressSaveIntervalMs) {
    return;
  }

  lastProgressSaveMs_ = nowMs;
  saveReadingPosition(false);
  saveLifetimeStats();  // flush lifetime totals on the same throttle
}

bool App::handleStandbyCombo(uint32_t nowMs) {
  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_ || !bootButtonReleasedSinceBoot_ ||
      !powerButtonReleasedSinceBoot_) {
    return false;
  }

  const bool bothHeld = button_.isHeld() && powerButton_.isHeld();
  if (state_ == AppState::Standby) {
    const bool pastGrace = nowMs - standbyEnteredMs_ >= kStandbyWakeGraceMs;
    if (!bothHeld && !button_.isHeld() && !powerButton_.isHeld() && pastGrace) {
      standbyButtonsReleased_ = true;
    }

    if (bothHeld) {
      if (standbyButtonsReleased_) {
        bootButtonLongPressHandled_ = true;
        powerButtonLongPressHandled_ = true;
        exitStandby(nowMs);
      }
      return true;
    }

    if (standbyComboActive_) {
      standbyComboActive_ = false;
      standbyComboHandled_ = false;
      bootButtonLongPressHandled_ = false;
      powerButtonLongPressHandled_ = false;
      return true;
    }

    return false;
  }

  if (bothHeld) {
    if (!standbyComboActive_) {
      standbyComboActive_ = true;
      standbyComboHandled_ = true;
      standbyComboStartedMs_ = nowMs;
      bootButtonLongPressHandled_ = true;
      powerButtonLongPressHandled_ = true;
      enterStandby(nowMs);
    }
    return true;
  }

  if (standbyComboActive_) {
    standbyComboActive_ = false;
    standbyComboHandled_ = false;
    bootButtonLongPressHandled_ = false;
    powerButtonLongPressHandled_ = false;
    return true;
  }

  return false;
}

#if defined(BOARD_AMOLED_18)
void App::handleAmoledButton(uint32_t nowMs) {
  // Single hardware button (BOOT): short press toggles the menu / backs out of a
  // submenu; long press cycles brightness.
  if (state_ == AppState::Booting || state_ == AppState::Sleeping || powerOffStarted_ ||
      state_ == AppState::Standby || state_ == AppState::PowerSaving) {
    // In deep standby BOOT does nothing — only a PWR tap wakes. Acting here would
    // flip state without waking the panel (black screen + live background touches).
    return;
  }

  if (!bootButtonReleasedSinceBoot_) {
    if (!button_.isHeld()) {
      bootButtonReleasedSinceBoot_ = true;
    }
    return;
  }

  if (button_.isHeld() || button_.wasPressedEvent() || button_.wasReleasedEvent()) {
    noteActivity(nowMs);  // BOOT interaction resets the idle-standby timer.
  }

  // Full-screen utility pages: a BOOT press exits straight back to the main menu.
  if (state_ == AppState::UsbTransfer) {
    if (button_.wasReleasedEvent()) {
      exitUsbTransfer(nowMs);
      openMainMenu(nowMs);
    }
    return;
  }
  if (state_ == AppState::CompanionSync) {
    if (button_.wasReleasedEvent()) {
      exitCompanionSync(nowMs);
      openMainMenu(nowMs);
    }
    return;
  }

  if (button_.isHeld() && !bootButtonLongPressHandled_ &&
      button_.heldDurationMs(nowMs) >= kThemeToggleHoldMs) {
    bootButtonLongPressHandled_ = true;
    cycleBrightness(nowMs);
    return;
  }

  if (!button_.wasReleasedEvent()) {
    return;
  }

  if (bootButtonLongPressHandled_) {
    bootButtonLongPressHandled_ = false;
    return;
  }

  menuBackOneLevel(nowMs);
}
#endif

void App::handleBootButton(uint32_t nowMs) {
  if (state_ == AppState::Standby) {
    if (!standbyButtonsReleased_ && !button_.isHeld() && !powerButton_.isHeld() &&
        nowMs - standbyEnteredMs_ >= kStandbyWakeGraceMs) {
      standbyButtonsReleased_ = true;
    }
    if (standbyButtonsReleased_ && button_.wasPressedEvent()) {
      bootButtonLongPressHandled_ = true;
      exitStandby(nowMs);
    }
    return;
  }

  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_) {
    return;
  }

  if (!bootButtonReleasedSinceBoot_) {
    if (!button_.isHeld()) {
      bootButtonReleasedSinceBoot_ = true;
    }
    return;
  }

  if (button_.isHeld() && !bootButtonLongPressHandled_ &&
      button_.heldDurationMs(nowMs) >= kThemeToggleHoldMs) {
    bootButtonLongPressHandled_ = true;
    lastUserActivityMs_ = nowMs;
    cycleThemeMode(nowMs);
    return;
  }

  if (!button_.wasReleasedEvent()) {
    return;
  }

  if (bootButtonLongPressHandled_) {
    bootButtonLongPressHandled_ = false;
    return;
  }

  if (button_.lastHoldDurationMs() < kThemeToggleHoldMs) {
    lastUserActivityMs_ = nowMs;
    cycleBrightness(nowMs);
  }
}

#if defined(BOARD_AMOLED_18)
void App::updatePmuPowerKey(uint32_t nowMs) {
  // AXP2101 PWR button: a short tap opens the "are you sure" confirm; a long
  // press commits straight to power off. (Power-on is handled by the PMU in
  // hardware: hold PWR while the device is off.)
  if (state_ == AppState::Booting || state_ == AppState::Sleeping || powerOffStarted_) {
    return;
  }

  const BoardConfig::PowerKeyEvent event = BoardConfig::pmuPollPowerKey();
  if (event != BoardConfig::PowerKeyEvent::None && lastPowerButtonActionMs_ != 0 &&
      nowMs - lastPowerButtonActionMs_ < kPowerButtonActionCooldownMs) {
    Serial.println("[power] PWRKEY event ignored during debounce window");
    return;
  }
  switch (event) {
    case BoardConfig::PowerKeyEvent::LongPress:
      lastPowerButtonActionMs_ = nowMs;
      if (state_ == AppState::PowerSaving) {
        if (nowMs - powerSaveEnteredMs_ >= kStandbyWakeGraceMs) {
          exitPowerSaving(nowMs);
        }
      } else {
        // Hold = power off. The hold itself is the confirmation, so no prompt.
        Serial.println("[power] PWRKEY long press -> power off");
        enterPowerOff(nowMs);
      }
      break;
    case BoardConfig::PowerKeyEvent::ShortPress:
      // Tap toggles deep standby (screen + touch off). It is the only gesture for
      // this mode: BOOT tap/hold stay menu/brightness, PWR hold stays power-off.
      lastPowerButtonActionMs_ = nowMs;
      if (state_ == AppState::PowerSaving) {
        if (nowMs - powerSaveEnteredMs_ >= kStandbyWakeGraceMs) {
          exitPowerSaving(nowMs);
        }
      } else {
        enterPowerSaving(nowMs);
      }
      break;
    case BoardConfig::PowerKeyEvent::PressDown:
    case BoardConfig::PowerKeyEvent::None:
      break;
  }
}
#endif  // BOARD_AMOLED_18

void App::handlePowerButton(uint32_t nowMs) {
  if (!powerButtonReleasedSinceBoot_) {
    if (!powerButton_.isHeld()) {
      powerButtonReleasedSinceBoot_ = true;
    }
    return;
  }

  if (state_ == AppState::Standby) {
    if (!standbyButtonsReleased_ && !button_.isHeld() && !powerButton_.isHeld() &&
        nowMs - standbyEnteredMs_ >= kStandbyWakeGraceMs) {
      standbyButtonsReleased_ = true;
    }
    if (standbyButtonsReleased_ && powerButton_.wasPressedEvent()) {
      powerButtonLongPressHandled_ = true;
      exitStandby(nowMs);
    }
    return;
  }

  if (state_ == AppState::UsbTransfer || state_ == AppState::CompanionSync || powerOffStarted_) {
    return;
  }

  if (powerButtonLongPressHandled_ && powerButton_.isHeld()) {
    return;
  }

  if (state_ == AppState::Menu && isFocusTimerMenuScreen(menuScreen_) &&
      powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kUsbTransferExitHoldMs) {
    powerButtonLongPressHandled_ = true;
    resetFocusTimer();
    menuScreen_ = MenuScreen::Main;
    renderMainMenu();
    return;
  }

  if (powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kPowerOffHoldMs) {
    if (lastPowerButtonActionMs_ != 0 &&
        nowMs - lastPowerButtonActionMs_ < kPowerButtonActionCooldownMs) {
      return;
    }
    lastPowerButtonActionMs_ = nowMs;
    powerButtonLongPressHandled_ = true;
    openPowerOffConfirm(nowMs);
    return;
  }

  if (!powerButton_.wasReleasedEvent()) {
    return;
  }

  if (powerButtonLongPressHandled_) {
    powerButtonLongPressHandled_ = false;
    return;
  }

  if (lastPowerButtonActionMs_ != 0 &&
      nowMs - lastPowerButtonActionMs_ < kPowerButtonActionCooldownMs) {
    Serial.println("[power] PWR release ignored during debounce window");
    return;
  }
  lastPowerButtonActionMs_ = nowMs;
  menuBackOneLevel(nowMs);
}

void App::toggleMenuFromPowerButton(uint32_t nowMs) {
  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync || state_ == AppState::Standby ||
      state_ == AppState::Sleeping) {
    return;
  }

  if (state_ == AppState::Menu) {
    if (menuScreen_ == MenuScreen::Main) {
      setState(AppState::Paused, nowMs);
    } else {
      if (isFocusTimerMenuScreen(menuScreen_)) {
        resetFocusTimer();
      }
      menuScreen_ = MenuScreen::Main;
      renderMainMenu();
    }
    return;
  }

  openMainMenu(nowMs);
}

void App::menuBackOneLevel(uint32_t nowMs) {
  // Swipe-right / hardware back should pop exactly ONE level, mirroring the
  // per-screen "Back" row, instead of collapsing straight to Main. Only the
  // two-levels-deep screens need special handling; everything else (one level
  // below Main, or not in the menu at all) falls through to the toggle, which
  // already does Main->resume / one-level->Main / closed->open.
  if (state_ != AppState::Menu) {
    toggleMenuFromPowerButton(nowMs);
    return;
  }

  switch (menuScreen_) {
    case MenuScreen::SettingsDisplay:
    case MenuScreen::SettingsPacing:
    case MenuScreen::SettingsBattery:
    case MenuScreen::SettingsSound:
    case MenuScreen::SettingsClock:
    case MenuScreen::WifiSettings:
      settingsSelectedIndex_ = kSettingsBackIndex;
      selectSettingsItem(nowMs);  // routes to the screen's Back handler -> SettingsHome
      return;
    case MenuScreen::WifiNetworks:
      wifiNetworkSelectedIndex_ = kWifiNetworksBackIndex;
      selectWifiNetworkItem(nowMs);  // -> WifiSettings
      return;
    case MenuScreen::TypographyTuning:
      typographyTuningSelectedIndex_ = TypographyTuningBack;
      selectTypographyTuningItem(nowMs);  // -> SettingsHome
      return;
    default:
      toggleMenuFromPowerButton(nowMs);
      return;
  }
}

void App::openMainMenu(uint32_t nowMs) {
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  menuScreen_ = MenuScreen::Main;
  menuSelectedIndex_ = MenuResume;
  wpmFeedbackVisible_ = false;
  contextViewVisible_ = false;
  if (state_ == AppState::Playing) {
    saveReadingPosition(true);
  }
  setState(AppState::Menu, nowMs);
}

uint8_t App::currentBrightnessPercent() const {
  return nightMode_ ? kNightBrightnessLevels[brightnessLevelIndex_]
                    : kBrightnessLevels[brightnessLevelIndex_];
}

void App::applyDisplayPreferences(uint32_t nowMs, bool rerender) {
  display_.setDarkMode(darkMode_);
  display_.setNightMode(nightMode_);
  display_.setBrightnessPercent(currentBrightnessPercent());

  if (!rerender) {
    return;
  }

  if (state_ == AppState::Menu) {
    if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
        menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
        menuScreen_ == MenuScreen::SettingsSound ||
        menuScreen_ == MenuScreen::SettingsClock ||
        menuScreen_ == MenuScreen::WifiSettings) {
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    }
    renderMenu();
    return;
  }

  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
    return;
  }

  if (state_ == AppState::Booting) {
    display_.renderCenteredWord("READY");
  }
}

void App::applyHandednessSettings(uint32_t nowMs, bool rerender) {
  (void)nowMs;
  applyReaderUiOrientation();

  if (!rerender) {
    return;
  }

  if (state_ == AppState::Menu &&
      (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
       menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
       menuScreen_ == MenuScreen::SettingsSound ||
       menuScreen_ == MenuScreen::SettingsClock ||
       menuScreen_ == MenuScreen::WifiSettings)) {
    rebuildSettingsMenuItems();
  }

  applyTypographySettings(nowMs);
}

void App::reloadRuntimePreferences(uint32_t nowMs, bool rerender) {
  brightnessLevelIndex_ = preferences_.getUChar(kPrefBrightness, brightnessLevelIndex_);
  if (brightnessLevelIndex_ >= kBrightnessLevelCount) {
    brightnessLevelIndex_ = kBrightnessLevelCount - 1;
  }
  phantomWordsEnabled_ = preferences_.getBool(kPrefPhantomWords, phantomWordsEnabled_);
  readerBatteryVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderBatteryVisible, readerBatteryVisibleWhilePlaying_);
  readerChapterVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderChapterVisible, readerChapterVisibleWhilePlaying_);
  readerProgressVisibleWhilePlaying_ =
      preferences_.getBool(kPrefReaderProgressVisible, readerProgressVisibleWhilePlaying_);
  uiLanguage_ =
      Localization::sanitizeLanguage(preferences_.getUChar(
          kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_)));
  readerMode_ = readerModeFromSetting(
      preferences_.getUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_)));
  handednessMode_ = handednessModeFromSetting(
      preferences_.getUChar(kPrefHandedness, static_cast<uint8_t>(handednessMode_)));
  readerFontSizeIndex_ = preferences_.getUChar(kPrefReaderFontSize, readerFontSizeIndex_);
  if (readerFontSizeIndex_ >= kReaderFontSizeCount) {
    readerFontSizeIndex_ = 0;
  }

  switch (preferences_.getUChar(kPrefFooterMetricMode,
                                static_cast<uint8_t>(footerMetricMode_))) {
    case static_cast<uint8_t>(FooterMetricMode::ChapterTime):
      footerMetricMode_ = FooterMetricMode::ChapterTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::BookTime):
      footerMetricMode_ = FooterMetricMode::BookTime;
      break;
    case static_cast<uint8_t>(FooterMetricMode::Percentage):
    default:
      footerMetricMode_ = FooterMetricMode::Percentage;
      break;
  }

  switch (preferences_.getUChar(kPrefBatteryLabelMode,
                                static_cast<uint8_t>(batteryLabelMode_))) {
    case static_cast<uint8_t>(BatteryLabelMode::TimeRemaining):
      batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Voltage):
      batteryLabelMode_ = BatteryLabelMode::Voltage;
      break;
    case static_cast<uint8_t>(BatteryLabelMode::Percent):
    default:
      batteryLabelMode_ = BatteryLabelMode::Percent;
      break;
  }

  screensaver_.setMode(StandbyScreensaver::modeFromValue(preferences_.getUChar(
      kPrefScreensaverMode, static_cast<uint8_t>(screensaver_.mode()))));

  switch (preferences_.getUChar(kPrefPauseMode, static_cast<uint8_t>(pauseMode_))) {
    case static_cast<uint8_t>(PauseMode::Instant):
      pauseMode_ = PauseMode::Instant;
      break;
    case static_cast<uint8_t>(PauseMode::SentenceEnd):
    default:
      pauseMode_ = PauseMode::SentenceEnd;
      break;
  }

  pacingLongWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingLongMs, kPrefLegacyPacingLong);
  pacingComplexWordDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingComplexMs, kPrefLegacyPacingComplex);
  pacingPunctuationDelayMs_ =
      loadPacingDelayMs(preferences_, kPrefPacingPunctuationMs, kPrefLegacyPacingPunctuation);
  timeEstimate_.setAccurateEstimate(true);

  typographyConfig_ = defaultTypographyConfig();
  typographyConfig_.typeface = readerTypefaceFromSetting(
      preferences_.getUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface)));
  typographyConfig_.focusHighlight =
      preferences_.getBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
  typographyConfig_.trackingPx = static_cast<int8_t>(clampIntSetting(
      preferences_.getChar(kPrefTypographyTracking, typographyConfig_.trackingPx),
      kTypographyTrackingMin, kTypographyTrackingMax));
  typographyConfig_.anchorPercent = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent),
      kTypographyAnchorMin, kTypographyAnchorMax));
  typographyConfig_.guideHalfWidth = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth),
      kTypographyGuideWidthMin, kTypographyGuideWidthMax));
  typographyConfig_.guideGap = static_cast<uint8_t>(clampIntSetting(
      preferences_.getUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap),
      kTypographyGuideGapMin, kTypographyGuideGapMax));
  darkMode_ = preferences_.getBool(kPrefDarkMode, darkMode_);
  nightMode_ = preferences_.getBool(kPrefNightMode, nightMode_);

  reader_.setWpm(preferences_.getUShort(kPrefWpm, reader_.wpm()));
  applyReaderUiOrientation();
  applyDisplayPreferences(nowMs, false);
  applyTypographySettings(nowMs, false);
  applyPacingSettings();
  if (rerender) {
    renderActiveReader(nowMs);
  }
}

void App::applyTypographySettings(uint32_t nowMs, bool rerender) {
  display_.setTypographyConfig(effectiveTypographyConfig());

  Serial.printf("[typography] face=%s highlight=%s track=%d anchor=%u guideWidth=%u guideGap=%u\n",
                readerTypefaceLabel().c_str(),
                focusHighlightLabel().c_str(),
                static_cast<int>(typographyConfig_.trackingPx),
                static_cast<unsigned int>(effectiveAnchorPercent()),
                static_cast<unsigned int>(typographyConfig_.guideHalfWidth),
                static_cast<unsigned int>(typographyConfig_.guideGap));

  if (!rerender) {
    return;
  }

  if (state_ == AppState::Menu) {
    renderMenu();
    return;
  }

  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  }
}

void App::cycleBrightness(uint32_t nowMs) {
  brightnessLevelIndex_ = static_cast<uint8_t>((brightnessLevelIndex_ + 1) % kBrightnessLevelCount);
  preferences_.putUChar(kPrefBrightness, brightnessLevelIndex_);
  const uint8_t percent = currentBrightnessPercent();
  Serial.printf("[display] brightness level %u/%u (%u%%)\n",
                static_cast<unsigned int>(brightnessLevelIndex_ + 1),
                static_cast<unsigned int>(kBrightnessLevelCount),
                static_cast<unsigned int>(percent));
  brightnessToastVisible_ = true;
  brightnessToastUntilMs_ = nowMs + kBrightnessToastMs;
  display_.setBrightnessOverlay(String(percent) + "%");
  applyDisplayPreferences(nowMs);
}

void App::cycleThemeMode(uint32_t nowMs) {
  if (nightMode_) {
    nightMode_ = false;
    darkMode_ = true;
  } else if (darkMode_) {
    darkMode_ = false;
  } else {
    darkMode_ = true;
    nightMode_ = true;
  }

  preferences_.putBool(kPrefDarkMode, darkMode_);
  preferences_.putBool(kPrefNightMode, nightMode_);
  Serial.printf("[display] theme=%s\n", themeModeLabel().c_str());
  applyDisplayPreferences(nowMs);
}

void App::cycleUiLanguage(uint32_t nowMs) {
  uiLanguage_ = Localization::nextLanguage(uiLanguage_);
  preferences_.putUChar(kPrefUiLanguage, static_cast<uint8_t>(uiLanguage_));
  Serial.printf("[display] language=%s\n", uiLanguageLabel().c_str());

  if (state_ == AppState::Menu) {
    if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
        menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
        menuScreen_ == MenuScreen::SettingsSound ||
        menuScreen_ == MenuScreen::SettingsClock ||
        menuScreen_ == MenuScreen::WifiSettings) {
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    }
    renderMenu();
    return;
  }

  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  }
}

void App::cycleReaderMode(uint32_t nowMs) {
  readerMode_ = nextReaderMode(readerMode_);
  preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(readerMode_));
  // Reload chapter label default for the new mode
  chapterLabelEnabled_ = preferences_.getBool(
      chapterLabelPrefKey(), chapterLabelDefaultForMode(readerMode_));
  Serial.printf("[display] reader mode=%s\n", readerModeLabel().c_str());
  invalidateContextPreviewWindow();

  // CPU frequency and battery estimate both depend on reader mode — refresh both immediately.
  applyStateCpuFrequency();
  batteryLabel_ = currentBatteryLabel();
  display_.setBatteryLabel(batteryLabel_);

  if (state_ == AppState::Menu) {
    rebuildSettingsMenuItems();
    renderSettings();
    return;
  }

  renderActiveReader(nowMs);
}

void App::cycleHandednessMode(uint32_t nowMs) {
  handednessMode_ = nextHandednessMode(handednessMode_);
  preferences_.putUChar(kPrefHandedness, static_cast<uint8_t>(handednessMode_));
  Serial.printf("[display] handedness=%s rotation180=%u\n", handednessLabel().c_str(),
                uiRotated180() ? 1U : 0U);
  applyHandednessSettings(nowMs);
}

void App::togglePhantomWords(uint32_t nowMs) {
  phantomWordsEnabled_ = !phantomWordsEnabled_;
  preferences_.putBool(kPrefPhantomWords, phantomWordsEnabled_);
  Serial.printf("[display] phantom words=%s\n", phantomWordsLabel().c_str());
  applyDisplayPreferences(nowMs);
}

void App::cycleReaderFontSize(uint32_t nowMs) {
  readerFontSizeIndex_ = static_cast<uint8_t>((readerFontSizeIndex_ + 1) % kReaderFontSizeCount);
  preferences_.putUChar(kPrefReaderFontSize, readerFontSizeIndex_);
  Serial.printf("[display] font size=%s\n", readerFontSizeLabel().c_str());
  applyDisplayPreferences(nowMs);
}

bool App::updateBatteryStatus(uint32_t nowMs, bool force) {
  bool powerSourceChanged = false;
  if (force || nowMs - lastBatteryPowerSourcePollMs_ >= kBatteryPowerSourcePollIntervalMs) {
    lastBatteryPowerSourcePollMs_ = nowMs;
    const bool vbusPresent = BoardConfig::pmuVbusPresent();
    if (!batteryVbusSampleInitialized_) {
      batteryVbusPresent_ = vbusPresent;
      batteryVbusSampleInitialized_ = true;
    } else if (batteryVbusPresent_ != vbusPresent) {
      batteryVbusPresent_ = vbusPresent;
      powerSourceChanged = true;
    }
  }

  if (!force && !powerSourceChanged) {
    const bool lowBatteryKnown =
        batteryPresent_ && batterySampleInitialized_ &&
        (batteryFilteredVoltage_ <= kBatteryLowWarningVoltage ||
         batteryDisplayedPercent_ <= kBatteryLowWarningPercent);
    uint32_t sampleIntervalMs =
        lowBatteryKnown ? kBatteryLowSampleIntervalMs : kBatterySampleIntervalMs;
    if (state_ == AppState::Playing && !lowBatteryKnown) {
      sampleIntervalMs = kBatteryPlayingSampleIntervalMs;
    }
    if (nowMs - lastBatterySampleMs_ < sampleIntervalMs) {
      return false;
    }
  }

  lastBatterySampleMs_ = nowMs;

  BoardConfig::BatteryStatus status;
  const bool wasCharging = batteryCharging_;
  if (BoardConfig::readBatteryStatus(status)) {
    batteryPresent_ = true;
    batteryCharging_ = status.charging && batteryVbusPresent_;
    if (!batterySampleInitialized_) {
      batteryFilteredVoltage_ = status.voltage;
      batteryFilteredPercent_ = status.percent;
      batteryDisplayedPercent_ = status.percent;
      batteryRuntimeAnchorPercent_ = status.percent;
      batteryRuntimeAnchorMs_ = nowMs;
      batterySampleInitialized_ = true;
    } else {
      batteryFilteredVoltage_ = (batteryFilteredVoltage_ * 0.72f) + (status.voltage * 0.28f);
      batteryFilteredPercent_ = (batteryFilteredPercent_ * 0.72f) + (status.percent * 0.28f);

      const int filteredPercent =
          std::max(0, std::min(100, static_cast<int>(batteryFilteredPercent_ + 0.5f)));
      const int delta = filteredPercent - static_cast<int>(batteryDisplayedPercent_);
      if (force || abs(delta) >= kBatteryDisplayHysteresisPercent ||
          filteredPercent <= 10 || filteredPercent >= 99) {
        batteryDisplayedPercent_ = static_cast<uint8_t>(filteredPercent);
      }

      if (batteryDisplayedPercent_ > batteryRuntimeAnchorPercent_) {
        batteryRuntimeAnchorPercent_ = batteryDisplayedPercent_;
        batteryRuntimeAnchorMs_ = nowMs;
        batteryRuntimeEstimateReady_ = false;
      } else {
        const uint8_t percentDrop = batteryRuntimeAnchorPercent_ - batteryDisplayedPercent_;
        const uint32_t elapsedMs = nowMs - batteryRuntimeAnchorMs_;
        if (percentDrop >= kBatteryRuntimeMinDropPercent &&
            elapsedMs >= kBatteryRuntimeMinElapsedMs) {
          const float minutesPerPercent =
              (static_cast<float>(elapsedMs) / 60000.0f) / static_cast<float>(percentDrop);
          batteryRuntimeMinutesRemaining_ =
              static_cast<uint32_t>(batteryDisplayedPercent_ * minutesPerPercent + 0.5f);
          batteryRuntimeEstimateReady_ = true;
        }
      }
    }
  } else {
    batteryPresent_ = false;
    batteryCharging_ = false;
    batteryCriticalSampleCount_ = 0;
  }
  const bool chargingChanged = batteryCharging_ != wasCharging;
  display_.setBatteryCharging(batteryCharging_);

  handleBatteryProtection(nowMs);
  if (powerOffStarted_) {
    return false;
  }

  const String nextLabel = currentBatteryLabel();
  if (nextLabel == batteryLabel_ && !chargingChanged) {
    return false;
  }

  batteryLabel_ = nextLabel;
  display_.setBatteryLabel(batteryLabel_);
  if (!batteryLabel_.isEmpty()) {
    Serial.printf("[power] battery %.2f V raw=%u%% shown=%u%% label=%s\n", status.voltage,
                  static_cast<unsigned int>(status.percent),
                  static_cast<unsigned int>(batteryDisplayedPercent_), batteryLabel_.c_str());
  } else {
    Serial.println("[power] battery not detected");
  }
  return true;
}

void App::handleBatteryProtection(uint32_t nowMs) {
  if (!batteryPresent_ || !batterySampleInitialized_) {
    batteryCriticalSampleCount_ = 0;
    return;
  }

  const bool critical = batteryFilteredVoltage_ <= kBatteryCriticalVoltage ||
                        batteryDisplayedPercent_ <= kBatteryCriticalPercent;
  if (critical) {
    if (batteryCriticalSampleCount_ < 255) {
      ++batteryCriticalSampleCount_;
    }
  } else {
    batteryCriticalSampleCount_ = 0;
  }

  if (batteryCriticalSampleCount_ >= kBatteryCriticalConsecutiveSamples) {
    const String line2 =
        batteryVoltageLabel() + " " + String(static_cast<unsigned int>(batteryDisplayedPercent_)) +
        "%";
    Serial.printf("[power] critical battery %.2f V %u%%; powering off\n",
                  static_cast<double>(batteryFilteredVoltage_),
                  static_cast<unsigned int>(batteryDisplayedPercent_));
    display_.renderStatus("LOW BATTERY", "Powering off", line2);
    delay(kBatteryShutdownNoticeMs);
    enterPowerOff(millis());
    return;
  }

  const bool low = batteryFilteredVoltage_ <= kBatteryLowWarningVoltage ||
                   batteryDisplayedPercent_ <= kBatteryLowWarningPercent;
  if (!low) {
    return;
  }

  if (lastLowBatteryWarningMs_ == 0 ||
      nowMs - lastLowBatteryWarningMs_ >= kBatteryLowWarningRepeatMs) {
    showLowBatteryWarning(nowMs);
  }
}

void App::showLowBatteryWarning(uint32_t nowMs) {
  lastLowBatteryWarningMs_ = nowMs;
  batteryWarningOverlayVisible_ = true;
  batteryWarningRestoreAtMs_ = nowMs + kBatteryWarningVisibleMs;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  wpmFeedbackVisible_ = false;
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;

  if (state_ == AppState::Playing) {
    setState(AppState::Paused, nowMs);
  }

  const String line1 =
      String(static_cast<unsigned int>(batteryDisplayedPercent_)) + "% remaining";
  display_.renderStatus("LOW BATTERY", line1, batteryVoltageLabel() + " charge soon");
  Serial.printf("[power] low battery warning %.2f V %u%%\n",
                static_cast<double>(batteryFilteredVoltage_),
                static_cast<unsigned int>(batteryDisplayedPercent_));
}

void App::updateBatteryWarningOverlay(uint32_t nowMs) {
  if (!batteryWarningOverlayVisible_ || nowMs < batteryWarningRestoreAtMs_) {
    return;
  }

  batteryWarningOverlayVisible_ = false;
  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  } else if (state_ == AppState::Menu) {
    renderMenu();
  } else if (state_ == AppState::Standby) {
    screensaver_.update(nowMs, true);
  }
}

void App::updateBrightnessToast(uint32_t nowMs) {
  if (!brightnessToastVisible_) {
    return;
  }

  if (nowMs < brightnessToastUntilMs_) {
    return;
  }

  brightnessToastVisible_ = false;
  display_.setBrightnessOverlay("");
  applyDisplayPreferences(nowMs);
}

void App::updateAutoDim(uint32_t nowMs) {
  // Only dim when the user is idle in Paused or Menu — never during active
  // reading or while a focus timer is running.
  const bool dimEligible =
      !shouldStayAwake() && (state_ == AppState::Paused || state_ == AppState::Menu);
  if (!dimEligible) {
    if (autoDimActive_) {
      restoreFromAutoDim(nowMs);
    }
    return;
  }

  // autoDimDelayMs_ == 0 means auto-dim is disabled.
  if (autoDimDelayMs_ == 0) {
    return;
  }

  if (!autoDimActive_ && lastUserActivityMs_ > 0 &&
      nowMs - lastUserActivityMs_ >= autoDimDelayMs_) {
    autoDimActive_ = true;
    display_.setBrightnessPercent(autoDimBrightnessPercent_);
    Serial.println("[power] auto-dim active");
  }
}

void App::restoreFromAutoDim(uint32_t nowMs) {
  if (!autoDimActive_) {
    return;
  }
  autoDimActive_ = false;
  lastUserActivityMs_ = nowMs;
  display_.setBrightnessPercent(currentBrightnessPercent());
  Serial.println("[power] auto-dim restored");
}

void App::updateBatteryRuntimeLabel(uint32_t nowMs) {
  if (!batteryPresent_ || !batterySampleInitialized_) {
    return;
  }
  if (batteryLabelMode_ != BatteryLabelMode::TimeRemaining) {
    return;
  }
  if (!batteryRuntimeEstimateReady_) {
    return;
  }
  if (nowMs - lastBatteryLabelRefreshMs_ < kBatteryLabelRefreshIntervalMs) {
    return;
  }
  lastBatteryLabelRefreshMs_ = nowMs;

  // Project remaining time forward from the last ADC sample.
  const uint32_t elapsedSinceSampleMinutes = (nowMs - lastBatterySampleMs_) / 60000UL;
  const uint32_t projected = batteryRuntimeMinutesRemaining_ > elapsedSinceSampleMinutes
                                 ? batteryRuntimeMinutesRemaining_ - elapsedSinceSampleMinutes
                                 : 0;
  const String nextLabel = formatBatteryTimeRemaining(projected);
  if (nextLabel == batteryLabel_) {
    return;
  }
  batteryLabel_ = nextLabel;
  display_.setBatteryLabel(batteryLabel_);
  if (state_ == AppState::Paused || state_ == AppState::Playing) {
    renderActiveReader(nowMs);
  } else if (state_ == AppState::Menu) {
    renderMenu();
  }
}

void App::updateWpmFeedback(uint32_t nowMs) {
  if (!wpmFeedbackVisible_ || state_ != AppState::Paused) {
    return;
  }

  if (nowMs < wpmFeedbackUntilMs_) {
    return;
  }

  wpmFeedbackVisible_ = false;
  renderActiveReader(nowMs);
}

void App::resetReaderTapTracking() { lastReaderTapValid_ = false; }

bool App::isFooterMetricTap(uint16_t x, uint16_t y) const {
  return x >= BoardConfig::DISPLAY_WIDTH - kFooterMetricTapWidthPx &&
         y >= BoardConfig::DISPLAY_HEIGHT - kFooterMetricTapHeightPx;
}

bool App::isBatteryBadgeTap(uint16_t x, uint16_t y) const {
  return x >= BoardConfig::DISPLAY_WIDTH - kBatteryBadgeTapWidthPx &&
         y <= kBatteryBadgeTapHeightPx;
}

bool App::isPreviousSentenceTap(uint16_t x, uint16_t y) const {
  return x < kPreviousSentenceTapWidthPx && y < kPreviousSentenceTapHeightPx;
}

bool App::isActivelyReading() const { return state_ == AppState::Playing; }

// True while the device must stay fully awake: an active reading session or a
// running focus timer. Blocks every idle path — screensaver standby, deep
// power-save, and auto-dim — because the timer counts down on its own screen
// with no touch to refresh activity, and reading should never blank.
bool App::shouldStayAwake() const {
  return isActivelyReading() || focusTimer_.isRunning();
}

DisplayManager::ReaderChrome App::readerChrome() const {
  DisplayManager::ReaderChrome chrome;
  const bool reading = isActivelyReading();
  chrome.showBattery = !reading || readerBatteryVisibleWhilePlaying_;
  const bool chapterAllowed = chapterLabelEnabled_ && !scrollModeEnabled();
  chrome.showChapter = chapterAllowed && (!reading || readerChapterVisibleWhilePlaying_);
  chrome.showProgress = !reading || readerProgressVisibleWhilePlaying_;
  chrome.showPreviousSentenceHint = !contextViewVisible_ || scrollModeEnabled();
  return chrome;
}

bool App::readerFooterVisible() const {
  const DisplayManager::ReaderChrome chrome = readerChrome();
  return chrome.showChapter || chrome.showProgress;
}

String App::readerFooterStatusLabel() const {
  if (isActivelyReading()) {
    return String(readingProgressPercent()) + "%";
  }

  return currentFooterMetricLabel();
}

String App::onOffLabel(bool enabled) const { return enabled ? uiText(UiText::On) : uiText(UiText::Off); }

bool App::handlePreviousSentenceTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  const bool previewBrowseMode = contextViewVisible_ && !scrollModeEnabled();
  if (previewBrowseMode || !isPreviousSentenceTap(x, y)) {
    return false;
  }

  resetReaderTapTracking();
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  reader_.rewindSentence();

  if (state_ == AppState::Playing) {
    setState(AppState::Paused, nowMs);
  } else {
    renderActiveReader(nowMs);
    saveReadingPosition(true);
  }

  Serial.printf("[app] sentence rewind index=%u word=%s\n",
                static_cast<unsigned int>(reader_.currentIndex()), reader_.currentWord().c_str());
  return true;
}

bool App::handleFooterMetricTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (isActivelyReading() || !readerFooterVisible() || !isFooterMetricTap(x, y)) {
    return false;
  }

  switch (footerMetricMode_) {
    case FooterMetricMode::Percentage:
      footerMetricMode_ = FooterMetricMode::ChapterTime;
      break;
    case FooterMetricMode::ChapterTime:
      footerMetricMode_ = FooterMetricMode::BookTime;
      break;
    case FooterMetricMode::BookTime:
    default:
      footerMetricMode_ = FooterMetricMode::Percentage;
      break;
  }

  preferences_.putUChar(kPrefFooterMetricMode, static_cast<uint8_t>(footerMetricMode_));
  resetReaderTapTracking();
  renderActiveReader(nowMs);
  const char *modeName = "percent";
  switch (footerMetricMode_) {
    case FooterMetricMode::ChapterTime:
      modeName = "chapter";
      break;
    case FooterMetricMode::BookTime:
      modeName = "book";
      break;
    case FooterMetricMode::Percentage:
    default:
      modeName = "percent";
      break;
  }
  Serial.printf("[reader] footer metric=%s\n", modeName);
  return true;
}

bool App::handleBatteryBadgeTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (batteryLabel_.isEmpty() || !readerChrome().showBattery || !isBatteryBadgeTap(x, y)) {
    return false;
  }

  switch (batteryLabelMode_) {
    case BatteryLabelMode::Percent:
      batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
      break;
    case BatteryLabelMode::TimeRemaining:
      batteryLabelMode_ = BatteryLabelMode::Voltage;
      break;
    case BatteryLabelMode::Voltage:
    default:
      batteryLabelMode_ = BatteryLabelMode::Percent;
      break;
  }
  preferences_.putUChar(kPrefBatteryLabelMode, static_cast<uint8_t>(batteryLabelMode_));
  batteryLabel_ = currentBatteryLabel();
  display_.setBatteryLabel(batteryLabel_);
  resetReaderTapTracking();
  renderActiveReader(nowMs);
  const char *modeName = "percent";
  if (batteryLabelMode_ == BatteryLabelMode::TimeRemaining) {
    modeName = "time";
  } else if (batteryLabelMode_ == BatteryLabelMode::Voltage) {
    modeName = "voltage";
  }
  Serial.printf("[power] battery label mode=%s label=%s\n", modeName, batteryLabel_.c_str());
  return true;
}

void App::handleReaderTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  const bool recentTap =
      lastReaderTapValid_ && nowMs - lastReaderTapMs_ <= kReaderDoubleTapWindowMs;
  const bool sameRegion =
      recentTap &&
      abs(static_cast<int>(x) - static_cast<int>(lastReaderTapX_)) <=
          static_cast<int>(kReaderDoubleTapSlopPx) &&
      abs(static_cast<int>(y) - static_cast<int>(lastReaderTapY_)) <=
          static_cast<int>(kReaderDoubleTapSlopPx);

  if (sameRegion) {
    resetReaderTapTracking();

    if (state_ == AppState::Playing) {
      requestReaderPauseAtSentenceEnd(nowMs);
    } else if (state_ == AppState::Paused) {
      playLocked_ = true;
      pauseAtSentenceEndRequested_ = false;
      wpmFeedbackVisible_ = false;
      setState(AppState::Playing, nowMs);
    }
    Serial.printf("[touch] reader double tap state=%s\n", stateName(state_));
    return;
  }

  if (recentTap) {
    Serial.printf("[touch] double tap miss dx=%d dy=%d dt=%lu\n",
                  static_cast<int>(x) - static_cast<int>(lastReaderTapX_),
                  static_cast<int>(y) - static_cast<int>(lastReaderTapY_),
                  static_cast<unsigned long>(nowMs - lastReaderTapMs_));
  }

  lastReaderTapValid_ = true;
  lastReaderTapMs_ = nowMs;
  lastReaderTapX_ = x;
  lastReaderTapY_ = y;
}

void App::requestReaderPauseAtSentenceEnd(uint32_t nowMs) {
  if (state_ != AppState::Playing) {
    return;
  }

  playLocked_ = false;
  touchPlayHeld_ = false;
  if (pauseMode_ == PauseMode::Instant) {
    pauseAtSentenceEndRequested_ = false;
    setState(AppState::Paused, nowMs);
    return;
  }

  if (!pauseAtSentenceEndRequested_) {
    pauseAtSentenceEndRequested_ = true;
    Serial.println("[app] pause requested at sentence end");
  }

  if (shouldFinalizeReaderPause(nowMs)) {
    finalizeReaderPause(nowMs);
  }
}

bool App::shouldFinalizeReaderPause(uint32_t nowMs) const {
  if (state_ != AppState::Playing || !pauseAtSentenceEndRequested_) {
    return false;
  }

  const uint32_t durationMs = reader_.currentWordDurationMs();
  if (durationMs == 0 || reader_.elapsedInCurrentWordMs(nowMs) < durationMs) {
    return false;
  }

  return reader_.currentWordEndsSentence() || reader_.atEnd();
}

void App::finalizeReaderPause(uint32_t nowMs) {
  pauseAtSentenceEndRequested_ = false;
  playLocked_ = false;
  touchPlayHeld_ = false;
  setState(AppState::Paused, nowMs);
}

void App::handleTouch(uint32_t nowMs) {
  if (!touchInitialized_) {
    return;
  }

  if (state_ == AppState::Booting || state_ == AppState::UsbTransfer ||
      state_ == AppState::CompanionSync || state_ == AppState::Standby ||
      state_ == AppState::Sleeping) {
    touch_.cancel();
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    touchPlayHeld_ = false;
    resetReaderTapTracking();
    return;
  }

  TouchEvent ev;
  if (!touch_.poll(ev)) {
    return;
  }

  // Any touch counts as user activity: update idle/auto-dim timers and restore dim if needed.
  noteActivity(nowMs);
  if (autoDimActive_) {
    restoreFromAutoDim(nowMs);
    // Consume this touch as a wake-up event; cancel outstanding state and skip handling.
    touch_.cancel();
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    return;
  }

  Serial.printf("[touch] phase=%s touched=%u x=%u y=%u gesture=%u state=%s\n",
                touchPhaseName(ev.phase), ev.touched ? 1 : 0, ev.x, ev.y, ev.gesture,
                stateName(state_));
  if (state_ == AppState::Finished) {
    // Completion screen: any tap dismisses to the library/book picker.
    if (ev.phase == TouchPhase::End) {
      setState(AppState::Menu, nowMs);
      openBookPicker(false);
    }
    return;
  }

  if (state_ == AppState::Menu) {
    if (menuScreen_ == MenuScreen::FocusTimerSession) {
      applyFocusTimerTouch(ev, nowMs);
    } else {
      applyMenuTouchGesture(ev, nowMs);
    }
  } else {
    applyPausedTouchGesture(ev, nowMs);
  }
}

void App::applyPausedTouchGesture(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::End && touchPlayHeld_) {
    resetReaderTapTracking();
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    touchPlayHeld_ = false;
    requestReaderPauseAtSentenceEnd(nowMs);
    return;
  }

  if (event.phase == TouchPhase::Start) {
    pausedTouch_.active = true;
    pausedTouchIntent_ = TouchIntent::None;
    if (state_ != AppState::Playing) {
      invalidateContextPreviewWindow();
    }
    pausedTouch_.startX = event.x;
    pausedTouch_.startY = event.y;
    pausedTouch_.lastX = event.x;
    pausedTouch_.lastY = event.y;
    pausedTouch_.startMs = nowMs;
    pausedTouch_.lastMs = nowMs;
    pausedTouch_.startWordIndex = reader_.currentIndex();
    pausedTouch_.gestureStepsApplied = 0;
    pausedTouch_.browseOffsetPermille = 0;
    return;
  }

  if (!pausedTouch_.active) {
    return;
  }

  const uint32_t elapsedSinceLastEventMs = nowMs - pausedTouch_.lastMs;
  pausedTouch_.lastX = event.x;
  pausedTouch_.lastY = event.y;
  pausedTouch_.lastMs = nowMs;

  const int deltaX = static_cast<int>(pausedTouch_.lastX) - static_cast<int>(pausedTouch_.startX);
  const int deltaY = static_cast<int>(pausedTouch_.lastY) - static_cast<int>(pausedTouch_.startY);
  const int absDeltaX = abs(deltaX);
  const int absDeltaY = abs(deltaY);
  const uint32_t pressDurationMs = nowMs - pausedTouch_.startMs;
  const bool ended = event.phase == TouchPhase::End;
  const bool tapLike = absDeltaX <= static_cast<int>(kTapSlopPx) &&
                       absDeltaY <= static_cast<int>(kTapSlopPx);
  const bool previewBrowseMode = contextViewVisible_ && !scrollModeEnabled();

  if (state_ == AppState::Playing) {
    if (ended) {
      pausedTouch_.active = false;
      pausedTouchIntent_ = TouchIntent::None;
      if (tapLike) {
        if (handleBatteryBadgeTap(event.x, event.y, nowMs)) {
          return;
        }
        if (handleFooterMetricTap(event.x, event.y, nowMs)) {
          return;
        }
        if (handlePreviousSentenceTap(event.x, event.y, nowMs)) {
          return;
        }
        if (playLocked_ || pauseAtSentenceEndRequested_) {
          resetReaderTapTracking();
          requestReaderPauseAtSentenceEnd(nowMs);
        } else {
          handleReaderTap(event.x, event.y, nowMs);
        }
      } else {
        resetReaderTapTracking();
      }
    }
    return;
  }

  if (!previewBrowseMode && !ended && pausedTouchIntent_ == TouchIntent::None &&
      pressDurationMs >= kTouchPlayHoldMs && tapLike) {
    resetReaderTapTracking();
    touchPlayHeld_ = true;
    pausedTouchIntent_ = TouchIntent::PlayHold;
    wpmFeedbackVisible_ = false;
    setState(AppState::Playing, nowMs);
    return;
  }

  if (pausedTouchIntent_ == TouchIntent::None) {
    if (absDeltaX >= static_cast<int>(kSwipeThresholdPx) &&
        absDeltaX > absDeltaY + static_cast<int>(kAxisBiasPx)) {
      resetReaderTapTracking();
      pausedTouchIntent_ = TouchIntent::Scrub;
    } else if (previewBrowseMode && !ended && pressDurationMs >= kPreviewBrowseHoldMs &&
               absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
      resetReaderTapTracking();
      pausedTouchIntent_ = TouchIntent::BrowseScroll;
    } else if (!previewBrowseMode && absDeltaY >= static_cast<int>(kSwipeThresholdPx) &&
               absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
      resetReaderTapTracking();
      pausedTouchIntent_ = TouchIntent::Wpm;
    }
  }

  if (pausedTouchIntent_ == TouchIntent::Scrub) {
    const int hMult = 1;
    applyScrubTarget(scrubStepsForDrag(deltaX) * hMult, nowMs);
    if (ended) {
      pausedTouch_.active = false;
      pausedTouchIntent_ = TouchIntent::None;
      saveReadingPosition(true);
    }
    return;
  }

  if (pausedTouchIntent_ == TouchIntent::BrowseScroll) {
    applyBrowseHoldScroll(event.y, elapsedSinceLastEventMs, nowMs);
    if (ended) {
      pausedTouch_.active = false;
      pausedTouchIntent_ = TouchIntent::None;
      saveReadingPosition(true);
    }
    return;
  }

  if (pausedTouchIntent_ == TouchIntent::Wpm) {
    if (!ended) {
      return;
    }

    const int vMult = 1;
    const int wpmDelta = (deltaY < 0) ? vMult : -vMult;
    reader_.adjustWpm(wpmDelta);
    preferences_.putUShort(kPrefWpm, reader_.wpm());
    renderWpmFeedback(nowMs);
    Serial.printf("[app] WPM=%u interval=%lu ms\n", reader_.wpm(),
                  static_cast<unsigned long>(reader_.wordIntervalMs()));
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    return;
  }

  if (ended) {
    pausedTouch_.active = false;
    pausedTouchIntent_ = TouchIntent::None;
    if (tapLike && handleBatteryBadgeTap(event.x, event.y, nowMs)) {
      return;
    }
    if (tapLike && handleFooterMetricTap(event.x, event.y, nowMs)) {
      return;
    }
    if (tapLike && handlePreviousSentenceTap(event.x, event.y, nowMs)) {
      return;
    }
    if (tapLike && previewBrowseMode) {
      resetReaderTapTracking();
      contextViewVisible_ = false;
      renderActiveReader(nowMs);
    } else if (tapLike) {
      handleReaderTap(event.x, event.y, nowMs);
    } else {
      resetReaderTapTracking();
    }
  }
}

int App::scrubStepsForDrag(int deltaX) const {
  const int absDeltaX = abs(deltaX);
  if (absDeltaX < static_cast<int>(kSwipeThresholdPx)) {
    return 0;
  }

  int steps = 1 + ((absDeltaX - static_cast<int>(kSwipeThresholdPx)) /
                   static_cast<int>(kScrubStepPx));
  steps = std::min(steps, kMaxScrubStepsPerGesture);

  return (deltaX > 0) ? steps : -steps;
}

void App::applyScrubTarget(int targetSteps, uint32_t nowMs) {
  if (targetSteps == pausedTouch_.gestureStepsApplied) {
    return;
  }

  reader_.seekRelative(pausedTouch_.startWordIndex, targetSteps);
  pausedTouch_.gestureStepsApplied = targetSteps;
  if (!scrollModeEnabled()) {
    contextViewVisible_ = true;
  }
  wpmFeedbackVisible_ = false;
  renderActiveReader(nowMs);
  Serial.printf("[app] scrub target=%d word=%s\n", targetSteps, reader_.currentWord().c_str());
}

int App::browseScrollRatePermille(uint16_t y) const {
  const int centerY = BoardConfig::DISPLAY_HEIGHT / 2;
  const int signedDistance = static_cast<int>(y) - centerY;
  const int absDistance = abs(signedDistance);
  if (absDistance <= static_cast<int>(kBrowseNeutralZonePx)) {
    return 0;
  }

  const int activeRange = std::max(1, centerY - static_cast<int>(kBrowseNeutralZonePx));
  const int activeDistance =
      std::min(activeRange, absDistance - static_cast<int>(kBrowseNeutralZonePx));
  const uint32_t speedPermille =
      kBrowseMinWordsPerSecondPermille +
      ((kBrowseMaxWordsPerSecondPermille - kBrowseMinWordsPerSecondPermille) *
       static_cast<uint32_t>(activeDistance)) /
          static_cast<uint32_t>(activeRange);

  return signedDistance < 0 ? -static_cast<int>(speedPermille) : static_cast<int>(speedPermille);
}

void App::renderContextBrowsePreview(size_t currentIndex, uint16_t scrollProgressPermille) {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    renderReaderWord();
    return;
  }

  if (currentIndex >= wordCount) {
    currentIndex = wordCount - 1;
    scrollProgressPermille = 0;
  }

  updateContextPreviewWindow(currentIndex);
  contextViewVisible_ = true;
  const DisplayManager::ReaderChrome chrome = readerChrome();
  display_.renderScrollView(contextPreviewWords_, currentReaderContentToken(),
                            contextPreviewStartIndex_, currentIndex, scrollProgressPermille,
                            currentChapterLabel(), readingProgressPercent(), "",
                            readerFooterStatusLabel(), chrome);
}

void App::applyBrowseHoldScroll(uint16_t y, uint32_t elapsedMs, uint32_t nowMs) {
  if (elapsedMs == 0) {
    return;
  }

  const int ratePermille = browseScrollRatePermille(y);
  pausedTouch_.browseOffsetPermille +=
      (static_cast<int32_t>(ratePermille) * static_cast<int32_t>(elapsedMs)) / 1000;

  int targetWords = pausedTouch_.browseOffsetPermille / 1000;
  int32_t remainderPermille = pausedTouch_.browseOffsetPermille % 1000;
  if (remainderPermille < 0) {
    remainderPermille += 1000;
    --targetWords;
  }

  reader_.seekRelative(pausedTouch_.startWordIndex, targetWords);
  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }
  pausedTouch_.gestureStepsApplied = targetWords;
  contextViewVisible_ = true;
  wpmFeedbackVisible_ = false;
  renderContextBrowsePreview(reader_.currentIndex(),
                             static_cast<uint16_t>(remainderPermille));
  Serial.printf("[app] browse hold target=%d progress=%ld word=%s\n", targetWords,
                static_cast<long>(remainderPermille), reader_.currentWord().c_str());
}

void App::applyMenuTouchGesture(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::Start) {
    pausedTouch_.active = true;
    pausedTouchIntent_ = TouchIntent::None;
    pausedTouch_.startX = event.x;
    pausedTouch_.startY = event.y;
    pausedTouch_.lastX = event.x;
    pausedTouch_.lastY = event.y;
    pausedTouch_.startMs = nowMs;
    pausedTouch_.lastMs = nowMs;
    return;
  }

  if (!pausedTouch_.active) {
    return;
  }

  pausedTouch_.lastX = event.x;
  pausedTouch_.lastY = event.y;
  pausedTouch_.lastMs = nowMs;

  if (event.phase != TouchPhase::End) {
    return;
  }

  pausedTouch_.active = false;

  const int deltaX = static_cast<int>(pausedTouch_.lastX) - static_cast<int>(pausedTouch_.startX);
  const int deltaY = static_cast<int>(pausedTouch_.lastY) - static_cast<int>(pausedTouch_.startY);
  const int absDeltaX = abs(deltaX);
  const int absDeltaY = abs(deltaY);

  if (menuScreen_ == MenuScreen::TextEntry) {
    if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
      textEntry_.handleTap(event.x, event.y, nowMs);
    }
    return;
  }

  if (menuScreen_ == MenuScreen::TypographyTuning &&
      absDeltaX >= static_cast<int>(kSwipeThresholdPx) &&
      absDeltaX > absDeltaY + static_cast<int>(kAxisBiasPx)) {
    cycleTypographyPreviewSample(deltaX < 0 ? 1 : -1);
    return;
  }

  // Swipe right = go back one level, the same action as the hardware back
  // button: pop one sub-menu page (mirrors the "Back" row), and only resume
  // reading from Main. Saves scrolling up to "Back".
  if (deltaX > 0 && absDeltaX >= static_cast<int>(kSwipeThresholdPx) &&
      absDeltaX > absDeltaY + static_cast<int>(kAxisBiasPx)) {
    lastMenuTapValid_ = false;
    menuBackOneLevel(nowMs);
    return;
  }

  if (absDeltaY >= static_cast<int>(kSwipeThresholdPx) &&
      absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx)) {
    moveMenuSelection(deltaY < 0 ? -1 : 1);
    lastMenuTapValid_ = false;  // A scroll cancels any pending double-tap.
    return;
  }

  if (absDeltaX <= static_cast<int>(kTapSlopPx) && absDeltaY <= static_cast<int>(kTapSlopPx)) {
    // Selection requires a DOUBLE tap so a short swipe that lands like a tap
    // does not accidentally open a menu item. A single tap only arms the
    // window; the second tap nearby and in time confirms the selection.
    const bool recentTap =
        lastMenuTapValid_ && nowMs - lastMenuTapMs_ <= kReaderDoubleTapWindowMs &&
        abs(static_cast<int>(event.x) - static_cast<int>(lastMenuTapX_)) <=
            static_cast<int>(kReaderDoubleTapSlopPx) &&
        abs(static_cast<int>(event.y) - static_cast<int>(lastMenuTapY_)) <=
            static_cast<int>(kReaderDoubleTapSlopPx);
    if (recentTap) {
      lastMenuTapValid_ = false;
      selectMenuItem(nowMs);
    } else {
      lastMenuTapValid_ = true;
      lastMenuTapMs_ = nowMs;
      lastMenuTapX_ = event.x;
      lastMenuTapY_ = event.y;
    }
  }
}

void App::applyFocusTimerTouch(const TouchEvent &event, uint32_t nowMs) {
  if (event.phase == TouchPhase::Start) {
    pausedTouch_.active = true;
    pausedTouch_.startX = event.x;
    pausedTouch_.startY = event.y;
    pausedTouch_.lastX = event.x;
    pausedTouch_.lastY = event.y;
    pausedTouch_.startMs = nowMs;
    pausedTouch_.lastMs = nowMs;
    focusTimerCancelHoldTriggered_ = false;
    return;
  }

  if (!pausedTouch_.active) {
    return;
  }

  pausedTouch_.lastX = event.x;
  pausedTouch_.lastY = event.y;
  pausedTouch_.lastMs = nowMs;

  const int deltaX = static_cast<int>(pausedTouch_.lastX) - static_cast<int>(pausedTouch_.startX);
  const int deltaY = static_cast<int>(pausedTouch_.lastY) - static_cast<int>(pausedTouch_.startY);
  const int absDeltaX = abs(deltaX);
  const int absDeltaY = abs(deltaY);

  // Touch-and-hold cancels the session (or backs out of Setup) -> preset picker.
  if (!focusTimerCancelHoldTriggered_ && event.phase != TouchPhase::End &&
      absDeltaX <= static_cast<int>(kFocusTimerCancelHoldMaxDriftPx) &&
      absDeltaY <= static_cast<int>(kFocusTimerCancelHoldMaxDriftPx) &&
      nowMs - pausedTouch_.startMs >= kFocusTimerCancelHoldMs) {
    if (nowMs - lastFocusTimerActionMs_ < kFocusTimerActionCooldownMs) {
      return;
    }
    lastFocusTimerActionMs_ = nowMs;
    pausedTouch_.active = false;
    focusTimerCancelHoldTriggered_ = true;
    focusTimer_.hold(nowMs);
    playFocusTimerCue(focusTimer_.consumeCue());
    openFocusTimerPresetPicker();
    return;
  }

  if (event.phase != TouchPhase::End) {
    return;
  }

  pausedTouch_.active = false;

  if (focusTimerCancelHoldTriggered_) {
    focusTimerCancelHoldTriggered_ = false;
    return;
  }

  const bool tapLike = absDeltaX <= static_cast<int>(kTapSlopPx) &&
                       absDeltaY <= static_cast<int>(kTapSlopPx);
  const bool horizontalSwipe = absDeltaX >= static_cast<int>(kSwipeThresholdPx) &&
                               absDeltaX > absDeltaY + static_cast<int>(kAxisBiasPx);
  const bool verticalSwipe = absDeltaY >= static_cast<int>(kSwipeThresholdPx) &&
                             absDeltaY > absDeltaX + static_cast<int>(kAxisBiasPx);

  const FocusTimer::State before = focusTimer_.state();

  if (before == FocusTimer::State::Setup) {
    if (verticalSwipe) {
      focusTimer_.stepField(deltaY > 0 ? 1 : -1);
    } else if (horizontalSwipe) {
      focusTimer_.stepFieldValue(deltaX > 0 ? 1 : -1);
      saveFocusTimerPreset(focusTimer_.preset());
    } else if (tapLike) {
      focusTimer_.tap(nowMs);
      playFocusTimerCue(focusTimer_.consumeCue());
    } else {
      return;
    }
    renderFocusTimerSession();
    return;
  }

  if (horizontalSwipe) {
    if (nowMs - lastFocusTimerActionMs_ < kFocusTimerActionCooldownMs) {
      renderFocusTimerSession();
      return;
    }
    lastFocusTimerActionMs_ = nowMs;
    focusTimer_.swipe(deltaX > 0 ? 1 : -1, nowMs);
  } else if (tapLike) {
    const bool debouncedTapAction =
        before == FocusTimer::State::WorkRunning || before == FocusTimer::State::BreakRunning ||
        before == FocusTimer::State::WorkPaused || before == FocusTimer::State::BreakPaused ||
        before == FocusTimer::State::WaitWorkStart || before == FocusTimer::State::Complete ||
        before == FocusTimer::State::Cancelled;
    if (debouncedTapAction) {
      if (nowMs - lastFocusTimerActionMs_ < kFocusTimerActionCooldownMs) {
        renderFocusTimerSession();
        return;
      }
      lastFocusTimerActionMs_ = nowMs;
    }
    focusTimer_.tap(nowMs);
  } else {
    return;
  }

  playFocusTimerCue(focusTimer_.consumeCue());

  // A tap on the completion screen returns to the preset picker.
  if (focusTimer_.state() == FocusTimer::State::Complete && before == FocusTimer::State::Complete) {
    openFocusTimerPresetPicker();
    return;
  }

  renderFocusTimerSession();
}

void App::openFocusTimer() {
  focusTimer_.open(millis());
  openFocusTimerPresetPicker();
}

void App::openFocusTimerPresetPicker() {
  focusTimer_.abandon(millis());
  rebuildFocusTimerPresetMenuItems();
  focusTimerPresetSelectedIndex_ = focusTimerPresetMenuItems_.size() > 1
                                       ? kFocusTimerPresetFirstIndex
                                       : kFocusTimerPresetBackIndex;
  focusTimerCancelHoldTriggered_ = false;
  lastFocusTimerActionMs_ = 0;
  pausedTouch_.active = false;
  menuScreen_ = MenuScreen::FocusTimerPresets;
  renderFocusTimerPresets();
}

void App::updateFocusTimer(uint32_t nowMs) {
  if (state_ != AppState::Menu || menuScreen_ != MenuScreen::FocusTimerSession) {
    return;
  }

  focusTimer_.update(nowMs);
  playFocusTimerCue(focusTimer_.consumeCue());

  if (focusTimer_.state() == FocusTimer::State::Cancelled) {
    openFocusTimerPresetPicker();
    return;
  }

  renderFocusTimerSession();
}

void App::resetFocusTimer() {
  focusTimer_.abandon(millis());
  focusTimerCancelHoldTriggered_ = false;
  pausedTouch_.active = false;
  focusTimerPresetSelectedIndex_ = kFocusTimerPresetBackIndex;
  applyReaderUiOrientation();
}

void App::rebuildFocusTimerPresetMenuItems() {
  focusTimerPresetMenuItems_.clear();
  focusTimerPresetMenuItems_.push_back(uiText(UiText::Back));
  for (uint8_t i = 0; i < FocusTimer::kPresetCount; ++i) {
    const FocusTimer::Preset preset = static_cast<FocusTimer::Preset>(i);
    const FocusTimer::Config c = focusTimer_.presetConfig(preset);
    focusTimerPresetMenuItems_.push_back(String(FocusTimer::presetLabel(preset)) + "  " +
                                         String(c.workMin) + "/" + String(c.breakMin) + "  x" +
                                         String(c.rounds));
  }
  if (focusTimerPresetSelectedIndex_ >= focusTimerPresetMenuItems_.size()) {
    focusTimerPresetSelectedIndex_ = focusTimerPresetMenuItems_.size() > 1
                                         ? kFocusTimerPresetFirstIndex
                                         : kFocusTimerPresetBackIndex;
  }
}

void App::selectFocusTimerPreset(uint32_t nowMs) {
  if (focusTimerPresetMenuItems_.empty()) {
    rebuildFocusTimerPresetMenuItems();
  }

  if (focusTimerPresetSelectedIndex_ == kFocusTimerPresetBackIndex) {
    resetFocusTimer();
    menuScreen_ = MenuScreen::Main;
    renderMainMenu();
    return;
  }

  const uint8_t presetIdx =
      static_cast<uint8_t>(focusTimerPresetSelectedIndex_ - kFocusTimerPresetFirstIndex);
  if (presetIdx >= FocusTimer::kPresetCount) {
    return;
  }

  focusTimer_.selectPreset(static_cast<FocusTimer::Preset>(presetIdx), nowMs);
  focusTimerCancelHoldTriggered_ = false;
  lastFocusTimerActionMs_ = 0;
  pausedTouch_.active = false;
  menuScreen_ = MenuScreen::FocusTimerSession;
  renderFocusTimerSession();
}

void App::loadFocusTimerPreferences() {
  for (uint8_t i = 0; i < FocusTimer::kPresetCount; ++i) {
    const uint32_t packed = preferences_.getUInt(kPrefTimerPresetConfig[i], 0);
    if (packed == 0) {
      continue;  // no stored override -> keep the built-in default
    }
    FocusTimer::Config c;
    c.workMin = static_cast<uint16_t>((packed >> 24) & 0xFF);
    c.breakMin = static_cast<uint16_t>((packed >> 16) & 0xFF);
    c.rounds = static_cast<uint8_t>((packed >> 8) & 0xFF);
    c.longBreakMin = static_cast<uint16_t>(packed & 0xFF);
    focusTimer_.setPresetConfig(static_cast<FocusTimer::Preset>(i), c);
  }
}

void App::saveFocusTimerPreset(FocusTimer::Preset preset) {
  const uint8_t i = static_cast<uint8_t>(preset);
  if (i >= FocusTimer::kPresetCount) {
    return;
  }
  const FocusTimer::Config c = focusTimer_.presetConfig(preset);
  const uint32_t packed = (static_cast<uint32_t>(c.workMin & 0xFF) << 24) |
                          (static_cast<uint32_t>(c.breakMin & 0xFF) << 16) |
                          (static_cast<uint32_t>(c.rounds & 0xFF) << 8) |
                          static_cast<uint32_t>(c.longBreakMin & 0xFF);
  preferences_.putUInt(kPrefTimerPresetConfig[i], packed);
}

void App::moveMenuSelection(int direction) {
  if (direction == 0 || menuScreen_ == MenuScreen::TextEntry) {
    return;
  }

  size_t *selectedIndex = &menuSelectedIndex_;
  size_t itemCount = MenuItemCount;
  if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
      menuScreen_ == MenuScreen::SettingsSound ||
      menuScreen_ == MenuScreen::SettingsClock ||
      menuScreen_ == MenuScreen::WifiSettings) {
    selectedIndex = &settingsSelectedIndex_;
    itemCount = settingsMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::WifiNetworks) {
    selectedIndex = &wifiNetworkSelectedIndex_;
    itemCount = wifiNetworkMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::TypographyTuning) {
    selectedIndex = &typographyTuningSelectedIndex_;
    itemCount = TypographyTuningItemCount;
  } else if (menuScreen_ == MenuScreen::BookPicker) {
    selectedIndex = &bookPickerSelectedIndex_;
    itemCount = bookMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::ChapterPicker) {
    selectedIndex = &chapterPickerSelectedIndex_;
    itemCount = chapterMenuItems_.size();
  } else if (menuScreen_ == MenuScreen::ReadingStats) {
    selectedIndex = &readingStatsSelectedIndex_;
    itemCount = readingStatsItems_.size();
  } else if (menuScreen_ == MenuScreen::RestartConfirm) {
    selectedIndex = &restartConfirmSelectedIndex_;
    itemCount = RestartConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    selectedIndex = &sdCardRepairConfirmSelectedIndex_;
    itemCount = SdCardRepairConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::UpdateConfirm) {
    selectedIndex = &updateConfirmSelectedIndex_;
    itemCount = UpdateConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::PowerOffConfirm) {
    selectedIndex = &powerOffConfirmSelectedIndex_;
    itemCount = PowerOffConfirmItemCount;
  } else if (menuScreen_ == MenuScreen::FocusTimerPresets) {
    selectedIndex = &focusTimerPresetSelectedIndex_;
    itemCount = focusTimerPresetMenuItems_.size();
  }

  if (itemCount == 0) {
    return;
  }

  const int next = static_cast<int>(*selectedIndex) + direction;
  if (next < 0) {
    *selectedIndex = itemCount - 1;
  } else if (next >= static_cast<int>(itemCount)) {
    *selectedIndex = 0;
  } else {
    *selectedIndex = static_cast<size_t>(next);
  }

  renderMenu();
  if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
      menuScreen_ == MenuScreen::SettingsSound ||
      menuScreen_ == MenuScreen::SettingsClock ||
      menuScreen_ == MenuScreen::WifiSettings) {
    Serial.printf("[settings] selected=%s\n", settingsMenuItems_[settingsSelectedIndex_].c_str());
  } else if (menuScreen_ == MenuScreen::WifiNetworks) {
    Serial.printf("[wifi] selected=%s\n", wifiNetworkMenuItems_[wifiNetworkSelectedIndex_].title.c_str());
  } else if (menuScreen_ == MenuScreen::TypographyTuning) {
    Serial.printf("[typography] selected=%s\n", typographyTuningLabel().c_str());
  } else if (menuScreen_ == MenuScreen::BookPicker) {
    Serial.printf("[book-picker] selected=%s\n",
                  bookMenuItems_[bookPickerSelectedIndex_].title.c_str());
  } else if (menuScreen_ == MenuScreen::ChapterPicker) {
    Serial.printf("[chapter-picker] selected=%s\n",
                  chapterMenuItems_[chapterPickerSelectedIndex_].c_str());
  } else if (menuScreen_ == MenuScreen::RestartConfirm) {
    String selectedLabel = uiText(UiText::AreYouSure);
    switch (restartConfirmSelectedIndex_) {
      case RestartConfirmNo:
        selectedLabel = uiText(UiText::NoKeepPlace);
        break;
      case RestartConfirmYes:
        selectedLabel = uiText(UiText::YesRestart);
        break;
      default:
        break;
    }
    Serial.printf("[restart] selected=%s\n", selectedLabel.c_str());
  } else if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    const String selectedLabel =
        sdCardRepairConfirmSelectedIndex_ == SdCardRepairConfirmYes ? "Create folders" : "Not now";
    Serial.printf("[sd-check] selected=%s\n", selectedLabel.c_str());
  } else if (menuScreen_ == MenuScreen::UpdateConfirm) {
    const String selectedLabel =
        updateConfirmSelectedIndex_ == UpdateConfirmUpdate ? "Update" : "Skip for now";
    Serial.printf("[ota] selected=%s\n", selectedLabel.c_str());
  } else if (menuScreen_ == MenuScreen::PowerOffConfirm) {
    const String selectedLabel =
        powerOffConfirmSelectedIndex_ == PowerOffConfirmYes ? "Yes" : "Cancel";
    Serial.printf("[power-off] selected=%s\n", selectedLabel.c_str());
  } else if (menuScreen_ == MenuScreen::FocusTimerPresets) {
    Serial.printf("[timer] selected preset=%s\n",
                  focusTimerPresetMenuItems_[focusTimerPresetSelectedIndex_].c_str());
  } else {
    String selectedLabel = uiText(UiText::Resume);
    switch (menuSelectedIndex_) {
      case MenuResume:
        selectedLabel = uiText(UiText::Resume);
        break;
      case MenuChapters:
        selectedLabel = uiText(UiText::Chapters);
        break;
      case MenuBooks:
        selectedLabel = "Books";
        break;
      case MenuArticles:
        selectedLabel = "Articles";
        break;
      case MenuFocusTimer:
        selectedLabel = "Focus Timer";
        break;
      case MenuSettings:
        selectedLabel = uiText(UiText::Settings);
        break;
      case MenuSdCardCheck:
        selectedLabel = "SD card check";
        break;
      case MenuRssFeeds:
        selectedLabel = "RSS feeds";
        break;
      case MenuCompanionSync:
        selectedLabel = "Companion sync";
        break;
#if RSVP_USB_TRANSFER_ENABLED
      case MenuUsbTransfer:
        selectedLabel = uiText(UiText::UsbTransfer);
        break;
#endif
      case MenuPowerOff:
        selectedLabel = uiText(UiText::PowerOff);
        break;
      default:
        break;
    }
    Serial.printf("[menu] selected=%s\n", selectedLabel.c_str());
  }
}

void App::selectMenuItem(uint32_t nowMs) {
  if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
      menuScreen_ == MenuScreen::SettingsSound ||
      menuScreen_ == MenuScreen::SettingsClock ||
      menuScreen_ == MenuScreen::WifiSettings) {
    selectSettingsItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::WifiNetworks) {
    selectWifiNetworkItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::TypographyTuning) {
    selectTypographyTuningItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::BookPicker) {
    selectBookPickerItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::ChapterPicker) {
    selectChapterPickerItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::ReadingStats) {
    selectReadingStatsItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::RestartConfirm) {
    selectRestartConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    selectSdCardRepairConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::UpdateConfirm) {
    selectUpdateConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::PowerOffConfirm) {
    selectPowerOffConfirmItem(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::FocusTimerPresets) {
    selectFocusTimerPreset(nowMs);
    return;
  }
  if (menuScreen_ == MenuScreen::FocusTimerSession) {
    return;
  }

  switch (menuSelectedIndex_) {
    case MenuResume:
      setState(AppState::Paused, nowMs);
      return;
    case MenuPowerOff:
      // Use the same confirm + sequence as the hardware power-off path so the
      // menu item is not an instant, jarring black-out.
      openPowerOffConfirm(nowMs);
      return;
    case MenuCompanionSync:
      enterCompanionSync(nowMs);
      return;
    case MenuSdCardCheck:
      runSdCardCheck(nowMs);
      return;
    case MenuRssFeeds:
      runRssFeedCheck(nowMs);
      return;
#if RSVP_USB_TRANSFER_ENABLED
    case MenuUsbTransfer:
      enterUsbTransfer(nowMs);
      return;
#endif
    case MenuChapters:
      openChapterPicker();
      return;
    case MenuReadingStats:
      openReadingStats();
      return;
    case MenuBooks:
      openBookPicker(false);
      return;
    case MenuArticles:
      openBookPicker(true);
      return;
    case MenuFocusTimer:
      openFocusTimer();
      return;
    case MenuSettings:
      openSettings();
      return;
    default:
      return;
  }
}

void App::openSettings() {
  settingsSelectedIndex_ = kSettingsHomeDisplayIndex;
  menuScreen_ = MenuScreen::SettingsHome;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectSettingsItem(uint32_t nowMs) {
  if (settingsMenuItems_.empty()) {
    if (menuScreen_ == MenuScreen::WifiSettings) {
      openWifiSettings();
    } else {
      openSettings();
    }
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsHome) {
    switch (settingsSelectedIndex_) {
      case kSettingsBackIndex:
        menuScreen_ = MenuScreen::Main;
        renderMainMenu();
        return;
      case kSettingsHomeDisplayIndex:
        settingsSelectedIndex_ = kSettingsDisplayThemeIndex;
        menuScreen_ = MenuScreen::SettingsDisplay;
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsHomeTypographyIndex:
        openTypographyTuning();
        return;
      case kSettingsHomeSoundIndex:
        openSoundSettings();
        return;
      case kSettingsHomePacingIndex:
        settingsSelectedIndex_ = kSettingsPacingReadingModeIndex;
        menuScreen_ = MenuScreen::SettingsPacing;
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsHomeWifiIndex:
        openWifiSettings();
        return;
      case kSettingsHomeUpdateIndex: {
        ota_.runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
        return;
      }
      case kSettingsHomeBootloaderIndex:
        restartToBootloader(nowMs);
        return;
      case kSettingsHomeBatteryIndex:
        openBatterySettings();
        return;
      case kSettingsHomeClockIndex:
        openClockSettings();
        return;
      default:
        return;
    }
  }

  if (menuScreen_ == MenuScreen::WifiSettings) {
    selectWifiSettingsItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsClock) {
    selectClockSettingsItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsBattery) {
    selectBatterySettingsItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsSound) {
    selectSoundSettingsItem(nowMs);
    return;
  }

  if (menuScreen_ == MenuScreen::SettingsDisplay) {
    switch (settingsSelectedIndex_) {
      case kSettingsBackIndex:
        settingsSelectedIndex_ = kSettingsHomeDisplayIndex;
        menuScreen_ = MenuScreen::SettingsHome;
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayThemeIndex:
        cycleThemeMode(nowMs);
        return;
      case kSettingsDisplayBrightnessIndex:
        cycleBrightness(nowMs);
        return;
      case kSettingsDisplayHandednessIndex:
        cycleHandednessMode(nowMs);
        return;
      case kSettingsDisplayFooterIndex:
        switch (footerMetricMode_) {
          case FooterMetricMode::Percentage:
            footerMetricMode_ = FooterMetricMode::ChapterTime;
            break;
          case FooterMetricMode::ChapterTime:
            footerMetricMode_ = FooterMetricMode::BookTime;
            break;
          case FooterMetricMode::BookTime:
            footerMetricMode_ = FooterMetricMode::Percentage;
            break;
        }
        preferences_.putUChar(kPrefFooterMetricMode, static_cast<uint8_t>(footerMetricMode_));
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayBatteryIndex:
        switch (batteryLabelMode_) {
          case BatteryLabelMode::Percent:
            batteryLabelMode_ = BatteryLabelMode::TimeRemaining;
            break;
          case BatteryLabelMode::TimeRemaining:
            batteryLabelMode_ = BatteryLabelMode::Voltage;
            break;
          case BatteryLabelMode::Voltage:
          default:
            batteryLabelMode_ = BatteryLabelMode::Percent;
            break;
        }
        preferences_.putUChar(kPrefBatteryLabelMode, static_cast<uint8_t>(batteryLabelMode_));
        batteryLabel_ = currentBatteryLabel();
        display_.setBatteryLabel(batteryLabel_);
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayScreensaverIndex:
        preferences_.putUChar(kPrefScreensaverMode,
                              static_cast<uint8_t>(screensaver_.cycleMode()));
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayReaderBatteryIndex:
        readerBatteryVisibleWhilePlaying_ = !readerBatteryVisibleWhilePlaying_;
        preferences_.putBool(kPrefReaderBatteryVisible, readerBatteryVisibleWhilePlaying_);
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayReaderChapterIndex:
        readerChapterVisibleWhilePlaying_ = !readerChapterVisibleWhilePlaying_;
        preferences_.putBool(kPrefReaderChapterVisible, readerChapterVisibleWhilePlaying_);
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayReaderProgressIndex:
        readerProgressVisibleWhilePlaying_ = !readerProgressVisibleWhilePlaying_;
        preferences_.putBool(kPrefReaderProgressVisible, readerProgressVisibleWhilePlaying_);
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayLanguageIndex:
        cycleUiLanguage(nowMs);
        return;
      case kSettingsDisplayOrientLockIndex:
        cycleOrientationLock(nowMs);
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      case kSettingsDisplayChapterLabelIndex:
        chapterLabelEnabled_ = !chapterLabelEnabled_;
        preferences_.putBool(chapterLabelPrefKey(), chapterLabelEnabled_);
        rebuildSettingsMenuItems();
        renderSettings();
        return;
      default:
        return;
    }
  }

  if (menuScreen_ != MenuScreen::SettingsPacing) {
    return;
  }

  bool pacingConfigChanged = false;
  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      timeEstimate_.flushPendingRebuild(currentBookPath_, currentBookTitle_);
      settingsSelectedIndex_ = kSettingsHomePacingIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsPacingReadingModeIndex:
      cycleReaderMode(nowMs);
      return;
    case kSettingsPacingPauseModeIndex:
      pauseMode_ =
          pauseMode_ == PauseMode::SentenceEnd ? PauseMode::Instant : PauseMode::SentenceEnd;
      preferences_.putUChar(kPrefPauseMode, static_cast<uint8_t>(pauseMode_));
      Serial.printf("[settings] pause mode=%s\n", pauseModeLabel().c_str());
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsPacingWpmIndex:
      reader_.setWpm(nextReaderWpmSetting(reader_.wpm()));
      preferences_.putUShort(kPrefWpm, reader_.wpm());
      Serial.printf("[settings] WPM=%u interval=%lu ms\n", reader_.wpm(),
                    static_cast<unsigned long>(reader_.wordIntervalMs()));
      break;
    case kSettingsPacingLongWordsIndex:
      pacingLongWordDelayMs_ = static_cast<uint16_t>(nextCyclicSetting(
          pacingLongWordDelayMs_, kPacingDelayMinMs, kPacingDelayMaxMs, kPacingDelayStepMs));
      preferences_.putUShort(kPrefPacingLongMs, pacingLongWordDelayMs_);
      pacingConfigChanged = true;
      break;
    case kSettingsPacingComplexityIndex:
      pacingComplexWordDelayMs_ = static_cast<uint16_t>(nextCyclicSetting(
          pacingComplexWordDelayMs_, kPacingDelayMinMs, kPacingDelayMaxMs, kPacingDelayStepMs));
      preferences_.putUShort(kPrefPacingComplexMs, pacingComplexWordDelayMs_);
      pacingConfigChanged = true;
      break;
    case kSettingsPacingPunctuationIndex:
      pacingPunctuationDelayMs_ = static_cast<uint16_t>(nextCyclicSetting(
          pacingPunctuationDelayMs_, kPacingDelayMinMs, kPacingDelayMaxMs, kPacingDelayStepMs));
      preferences_.putUShort(kPrefPacingPunctuationMs, pacingPunctuationDelayMs_);
      pacingConfigChanged = true;
      break;
    case kSettingsPacingResetIndex:
      pacingLongWordDelayMs_ = kDefaultPacingDelayMs;
      pacingComplexWordDelayMs_ = kDefaultPacingDelayMs;
      pacingPunctuationDelayMs_ = kDefaultPacingDelayMs;
      preferences_.putUShort(kPrefPacingLongMs, pacingLongWordDelayMs_);
      preferences_.putUShort(kPrefPacingComplexMs, pacingComplexWordDelayMs_);
      preferences_.putUShort(kPrefPacingPunctuationMs, pacingPunctuationDelayMs_);
      pacingConfigChanged = true;
      break;
    default:
      return;
  }

  if (pacingConfigChanged) {
    applyPacingSettings();
  }
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::openBatterySettings() {
  settingsSelectedIndex_ = kSettingsBatteryCpuPlayIndex;
  menuScreen_ = MenuScreen::SettingsBattery;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectBatterySettingsItem(uint32_t nowMs) {
  (void)nowMs;

  auto cycleCpuMhz = [](uint32_t current) -> uint32_t {
    if (current <= 80) return 160;
    if (current <= 160) return 240;
    return 80;
  };
  auto cycleCpuMhzStandby = [](uint32_t current) -> uint32_t {
    if (current <= 40) return 80;
    if (current <= 80) return 160;
    if (current <= 160) return 240;
    return 40;
  };

  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsHomeBatteryIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsBatteryCpuPlayIndex:
      cpuMhzPlay_ = cycleCpuMhz(cpuMhzPlay_);
      preferences_.putUInt(kPrefCpuPlay, cpuMhzPlay_);
      applyStateCpuFrequency();
      Serial.printf("[battery] CPU play -> %u MHz\n", cpuMhzPlay_);
      break;
    case kSettingsBatteryCpuScrollIndex:
      cpuMhzScroll_ = cycleCpuMhz(cpuMhzScroll_);
      preferences_.putUInt(kPrefCpuScroll, cpuMhzScroll_);
      applyStateCpuFrequency();
      Serial.printf("[battery] CPU scroll -> %u MHz\n", cpuMhzScroll_);
      break;
    case kSettingsBatteryCpuPausedIndex:
      cpuMhzPaused_ = cycleCpuMhz(cpuMhzPaused_);
      preferences_.putUInt(kPrefCpuPaused, cpuMhzPaused_);
      applyStateCpuFrequency();
      Serial.printf("[battery] CPU paused -> %u MHz\n", cpuMhzPaused_);
      break;
    case kSettingsBatteryCpuMenuIndex:
      cpuMhzMenu_ = cycleCpuMhz(cpuMhzMenu_);
      preferences_.putUInt(kPrefCpuMenu, cpuMhzMenu_);
      applyStateCpuFrequency();
      Serial.printf("[battery] CPU menu -> %u MHz\n", cpuMhzMenu_);
      break;
    case kSettingsBatteryCpuStandbyIndex:
      cpuMhzStandby_ = cycleCpuMhzStandby(cpuMhzStandby_);
      preferences_.putUInt(kPrefCpuStandby, cpuMhzStandby_);
      applyStateCpuFrequency();
      Serial.printf("[battery] CPU standby -> %u MHz\n", cpuMhzStandby_);
      break;
    case kSettingsBatteryAutoDimDelayIndex: {
      if (autoDimDelayMs_ == 0) {
        autoDimDelayMs_ = 30000;
      } else if (autoDimDelayMs_ <= 30000) {
        autoDimDelayMs_ = 60000;
      } else if (autoDimDelayMs_ <= 60000) {
        autoDimDelayMs_ = 120000;
      } else {
        autoDimDelayMs_ = 0;
      }
      preferences_.putUInt(kPrefAutoDimDelay, autoDimDelayMs_);
      if (autoDimDelayMs_ == 0 && autoDimActive_) {
        restoreFromAutoDim(nowMs);
      }
      Serial.printf("[battery] auto-dim delay -> %s\n", autoDimDelayLabel().c_str());
      break;
    }
    case kSettingsBatteryAutoDimLevelIndex: {
      if (autoDimBrightnessPercent_ >= 30) {
        autoDimBrightnessPercent_ = 0;
      } else if (autoDimBrightnessPercent_ == 0) {
        autoDimBrightnessPercent_ = 10;
      } else {
        autoDimBrightnessPercent_ = static_cast<uint8_t>(autoDimBrightnessPercent_ + 10);
      }
      preferences_.putUChar(kPrefAutoDimLevel, autoDimBrightnessPercent_);
      if (autoDimActive_) {
        display_.setBrightnessPercent(autoDimBrightnessPercent_);
      }
      Serial.printf("[battery] auto-dim level -> %u%%\n",
                    static_cast<unsigned int>(autoDimBrightnessPercent_));
      break;
    }
#if defined(BOARD_AMOLED_18)
    case kSettingsBatteryStandbyDelayIndex: {
      if (deepStandbyDelayMs_ == 0) {
        deepStandbyDelayMs_ = 60000;
      } else if (deepStandbyDelayMs_ <= 60000) {
        deepStandbyDelayMs_ = 180000;
      } else if (deepStandbyDelayMs_ <= 180000) {
        deepStandbyDelayMs_ = 300000;
      } else if (deepStandbyDelayMs_ <= 300000) {
        deepStandbyDelayMs_ = 600000;
      } else {
        deepStandbyDelayMs_ = 0;
      }
      preferences_.putUInt(kPrefDeepStandbyDelay, deepStandbyDelayMs_);
      Serial.printf("[power] deep standby delay -> %s\n", deepStandbyDelayLabel().c_str());
      break;
    }
#endif
    default:
      return;
  }

  // Refresh battery bootstrap label to reflect new nominal runtime.
  if (!batteryRuntimeEstimateReady_) {
    batteryLabel_ = currentBatteryLabel();
    display_.setBatteryLabel(batteryLabel_);
    lastBatteryLabelRefreshMs_ = nowMs;
  }
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::openSoundSettings() {
  settingsSelectedIndex_ = kSettingsSoundVolumeIndex;
  menuScreen_ = MenuScreen::SettingsSound;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectSoundSettingsItem(uint32_t nowMs) {
  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsHomeSoundIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsSoundVolumeIndex:
      cycleSoundVolume(nowMs);
      return;
    case kSettingsSoundTimerChimeIndex:
      cycleFocusTimerChime(nowMs);
      return;
    default:
      return;
  }
}

String App::soundVolumeLabel() const {
  if (soundVolumePercent_ == 0) {
    return "Muted";
  }
  return String(soundVolumePercent_) + "%";
}

void App::cycleSoundVolume(uint32_t nowMs) {
  soundVolumePercent_ = static_cast<uint8_t>(
      nextCyclicSetting(soundVolumePercent_, 0, 100, 10));
  preferences_.putUChar(kPrefSoundVolume, soundVolumePercent_);
  audio_.setVolumePercent(soundVolumePercent_);
  Serial.printf("[sound] volume=%u%%\n", static_cast<unsigned int>(soundVolumePercent_));
  rebuildSettingsMenuItems();
  renderSettings();
  if (soundVolumePercent_ > 0 && focusTimerChime_ != FocusTimerChime::Off) {
    playFocusTimerCue(FocusTimer::Cue::WorkComplete);
  }
  lastFocusTimerActionMs_ = nowMs;
}

void App::openWifiSettings() {
  settingsSelectedIndex_ = configuredWifiSsid().isEmpty() ? kWifiSettingsChooseIndex
                                                          : kWifiSettingsAutoUpdateIndex;
  menuScreen_ = MenuScreen::WifiSettings;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectWifiSettingsItem(uint32_t nowMs) {
  (void)nowMs;

  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsHomeWifiIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kWifiSettingsNetworkIndex:
    case kWifiSettingsChooseIndex:
      scanWifiNetworks();
      return;
    case kWifiSettingsAutoUpdateIndex:
      preferences_.putBool(kPrefOtaAuto, !otaAutoCheckEnabled());
      cachedOtaAutoCheck_ = otaAutoCheckEnabled();
      // Auto OTA toggle affects nominal battery estimate — refresh label if in bootstrap mode.
      if (!batteryRuntimeEstimateReady_) {
        batteryLabel_ = currentBatteryLabel();
        display_.setBatteryLabel(batteryLabel_);
      }
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kWifiSettingsForgetIndex:
      preferences_.remove(kPrefWifiSsid);
      preferences_.remove(kPrefWifiPass);
      display_.renderStatus("Wi-Fi", "Credentials cleared", "");
      delay(900);
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    default:
      return;
  }
}

void App::scanWifiNetworks() {
  if (ota_.blockForCheck("Wi-Fi", millis())) {
    return;
  }

  display_.renderProgress("Wi-Fi", "Scanning networks", "", 5);

  WiFi.persistent(false);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();

  const int networkCount = WiFi.scanNetworks(false, true);
  wifiNetworks_.clear();
  wifiNetworkMenuItems_.clear();
  wifiNetworkMenuItems_.push_back({uiText(UiText::Back), ""});

  if (networkCount > 0) {
    for (int i = 0; i < networkCount; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) {
        continue;
      }

      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = WiFi.RSSI(i);
      network.authMode = static_cast<uint8_t>(WiFi.encryptionType(i));
      wifiNetworks_.push_back(network);
    }
  }

  WiFi.scanDelete();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  if (wifiNetworks_.empty()) {
    display_.renderStatus("Wi-Fi", "No networks found", "");
    delay(1200);
    openWifiSettings();
    return;
  }

  const String savedSsid = configuredWifiSsid();
  std::stable_sort(wifiNetworks_.begin(), wifiNetworks_.end(),
                   [&savedSsid](const WifiNetworkInfo &left, const WifiNetworkInfo &right) {
                     const bool leftSaved = !savedSsid.isEmpty() && left.ssid == savedSsid;
                     const bool rightSaved = !savedSsid.isEmpty() && right.ssid == savedSsid;
                     if (leftSaved != rightSaved) {
                       return leftSaved;
                     }
                     if (left.rssi != right.rssi) {
                       return left.rssi > right.rssi;
                     }
                     return left.ssid < right.ssid;
                   });

  wifiNetworkMenuItems_.reserve(wifiNetworks_.size() + 1);
  for (const WifiNetworkInfo &network : wifiNetworks_) {
    wifiNetworkMenuItems_.push_back(
        {network.ssid, wifiSecurityLabel(network.authMode) + "  " + String(network.rssi) + " dBm"});
  }

  wifiNetworkSelectedIndex_ =
      wifiNetworkMenuItems_.size() > 1 ? kWifiNetworksFirstItemIndex : kWifiNetworksBackIndex;
  menuScreen_ = MenuScreen::WifiNetworks;
  renderWifiNetworks();
}

void App::renderWifiNetworks() {
  if (wifiNetworkMenuItems_.empty()) {
    display_.renderStatus("Wi-Fi", "No networks found", "");
    return;
  }

  display_.renderLibrary(wifiNetworkMenuItems_, wifiNetworkSelectedIndex_);
}

void App::selectWifiNetworkItem(uint32_t nowMs) {
  (void)nowMs;

  if (wifiNetworkSelectedIndex_ == kWifiNetworksBackIndex || wifiNetworkMenuItems_.size() <= 1) {
    openWifiSettings();
    return;
  }

  const size_t networkIndex = wifiNetworkSelectedIndex_ - kWifiNetworksFirstItemIndex;
  if (networkIndex >= wifiNetworks_.size()) {
    openWifiSettings();
    return;
  }

  const WifiNetworkInfo &network = wifiNetworks_[networkIndex];
  if (wifiNetworkRequiresPassword(network.authMode)) {
    String initialValue;
    if (configuredWifiSsid() == network.ssid) {
      initialValue = preferredOtaConfig().wifiPassword;
    }
    openWifiPasswordEntry(network.ssid, initialValue);
    return;
  }

  preferences_.putString(kPrefWifiSsid, network.ssid);
  preferences_.putString(kPrefWifiPass, "");
  display_.renderStatus("Wi-Fi", "Network saved", network.ssid);
  delay(900);
  openWifiSettings();
}

void App::openWifiPasswordEntry(const String &ssid, const String &initialValue) {
  textEntryReturnScreen_ = MenuScreen::WifiNetworks;
  menuScreen_ = MenuScreen::TextEntry;
  textEntry_.open(ssid, "Password", "", initialValue, ssid, true, kWifiPasswordMaxLength);
}

void App::submitWifiPassword(uint32_t nowMs) {
  (void)nowMs;
  if (textEntry_.value().isEmpty()) {
    display_.renderStatus("Wi-Fi", "Password required", textEntry_.contextValue());
    delay(1000);
    textEntry_.render();
    return;
  }

  const String ssid = textEntry_.contextValue();
  preferences_.putString(kPrefWifiSsid, ssid);
  preferences_.putString(kPrefWifiPass, textEntry_.value());
  textEntry_.close();
  display_.renderStatus("Wi-Fi", "Network saved", ssid);
  delay(900);
  openWifiSettings();
}

void App::openTypographyTuning() {
  if (typographyTuningSelectedIndex_ >= TypographyTuningItemCount) {
    typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  }
  if (typographyTuningSelectedIndex_ == TypographyTuningBack) {
    typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  }
  menuScreen_ = MenuScreen::TypographyTuning;
  renderTypographyTuning();
}

void App::selectTypographyTuningItem(uint32_t nowMs) {
  switch (typographyTuningSelectedIndex_) {
    case TypographyTuningBack:
      settingsSelectedIndex_ = kSettingsHomeTypographyIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case TypographyTuningFontSize:
      cycleReaderFontSize(nowMs);
      return;
    case TypographyTuningTypeface:
      typographyConfig_.typeface = nextReaderTypeface(typographyConfig_.typeface);
      preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface));
      break;
    case TypographyTuningPhantomWords:
      togglePhantomWords(nowMs);
      return;
    case TypographyTuningFocusHighlight:
      typographyConfig_.focusHighlight = !typographyConfig_.focusHighlight;
      preferences_.putBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
      break;
    case TypographyTuningTracking:
      typographyConfig_.trackingPx = static_cast<int8_t>(
          nextCyclicSetting(typographyConfig_.trackingPx, kTypographyTrackingMin,
                            kTypographyTrackingMax));
      preferences_.putChar(kPrefTypographyTracking, typographyConfig_.trackingPx);
      break;
    case TypographyTuningAnchor: {
      const uint8_t anchorMin =
          (handednessMode_ == HandednessMode::Left) ? kLeftHandAnchorMin : kTypographyAnchorMin;
      const uint8_t anchorMax =
          (handednessMode_ == HandednessMode::Left) ? kLeftHandAnchorMax : kTypographyAnchorMax;
      const uint8_t nextAnchorPercent = static_cast<uint8_t>(
          nextCyclicSetting(effectiveAnchorPercent(), anchorMin, anchorMax));
      typographyConfig_.anchorPercent = (handednessMode_ == HandednessMode::Left)
                                            ? static_cast<uint8_t>(nextAnchorPercent -
                                                                   kLeftHandAnchorOffset)
                                            : nextAnchorPercent;
      preferences_.putUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent);
      break;
    }
    case TypographyTuningGuideWidth:
      typographyConfig_.guideHalfWidth = static_cast<uint8_t>(nextCyclicSetting(
          typographyConfig_.guideHalfWidth, kTypographyGuideWidthMin,
          kTypographyGuideWidthMax, kTypographyGuideWidthStep));
      preferences_.putUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth);
      break;
    case TypographyTuningGuideGap:
      typographyConfig_.guideGap = static_cast<uint8_t>(nextCyclicSetting(
          typographyConfig_.guideGap, kTypographyGuideGapMin, kTypographyGuideGapMax));
      preferences_.putUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap);
      break;
    case TypographyTuningReset:
      typographyConfig_ = defaultTypographyConfig();
      preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(typographyConfig_.typeface));
      preferences_.putBool(kPrefTypographyFocusHighlight, typographyConfig_.focusHighlight);
      preferences_.putChar(kPrefTypographyTracking, typographyConfig_.trackingPx);
      preferences_.putUChar(kPrefTypographyAnchor, typographyConfig_.anchorPercent);
      preferences_.putUChar(kPrefTypographyGuideWidth, typographyConfig_.guideHalfWidth);
      preferences_.putUChar(kPrefTypographyGuideGap, typographyConfig_.guideGap);
      break;
    default:
      return;
  }

  applyTypographySettings(nowMs);
}

void App::cycleTypographyPreviewSample(int direction) {
  if (kTypographyPreviewWordCount == 0 || direction == 0) {
    return;
  }

  const int current = static_cast<int>(typographyPreviewSampleIndex_);
  int next = current + direction;
  if (next < 0) {
    next = static_cast<int>(kTypographyPreviewWordCount) - 1;
  } else if (next >= static_cast<int>(kTypographyPreviewWordCount)) {
    next = 0;
  }
  typographyPreviewSampleIndex_ = static_cast<size_t>(next);
  renderTypographyTuning();
}

void App::rebuildSettingsMenuItems() {
  settingsMenuItems_.clear();
  settingsMenuItems_.reserve(SettingsItemCount);
  if (menuScreen_ == MenuScreen::SettingsHome) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back(uiText(UiText::WordPacing));
    settingsMenuItems_.push_back(uiText(UiText::Display));
    settingsMenuItems_.push_back(uiText(UiText::TypographyTune));
    settingsMenuItems_.push_back("Sound");
    settingsMenuItems_.push_back("Wi-Fi");
    settingsMenuItems_.push_back("Battery");
    settingsMenuItems_.push_back("Clock");
    settingsMenuItems_.push_back(firmwareUpdateMenuLabel());
    settingsMenuItems_.push_back("Restart to bootloader");
    settingsMenuItems_.push_back("Installed: " + ota_.firmwareVersionLabel());
  } else if (menuScreen_ == MenuScreen::SettingsDisplay) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back("Display mode: " + themeModeLabel());
    settingsMenuItems_.push_back(uiText(UiText::Brightness) + ": " +
                                 String(currentBrightnessPercent()) + "%");
    settingsMenuItems_.push_back("Reader hand: " + handednessLabel());
    settingsMenuItems_.push_back("Chapter label: " + onOffLabel(chapterLabelEnabled_));
    settingsMenuItems_.push_back("Footer label: " + footerMetricModeLabel());
    settingsMenuItems_.push_back("Battery label: " + batteryLabelModeLabel());
    settingsMenuItems_.push_back("Screensaver: " + screensaver_.modeLabel());
    settingsMenuItems_.push_back("Reading battery: " +
                                 onOffLabel(readerBatteryVisibleWhilePlaying_));
    settingsMenuItems_.push_back("Reading chapter: " +
                                 onOffLabel(readerChapterVisibleWhilePlaying_));
    settingsMenuItems_.push_back("Reading percent: " +
                                 onOffLabel(readerProgressVisibleWhilePlaying_));
    settingsMenuItems_.push_back(uiText(UiText::Language) + ": " + uiLanguageLabel());
    settingsMenuItems_.push_back("Orientation lock: " + orientationLockLabel());
  } else if (menuScreen_ == MenuScreen::SettingsPacing) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back("Reading mode: " + readerModeLabel());
    settingsMenuItems_.push_back("Pause behaviour: " + pauseModeLabel());
    settingsMenuItems_.push_back("Base speed: " + String(reader_.wpm()) + " WPM");
    settingsMenuItems_.push_back(uiText(UiText::LongWords) + ": " +
                                 pacingDelayLabel(pacingLongWordDelayMs_));
    settingsMenuItems_.push_back(uiText(UiText::Complexity) + ": " +
                                 pacingDelayLabel(pacingComplexWordDelayMs_));
    settingsMenuItems_.push_back(uiText(UiText::Punctuation) + ": " +
                                 pacingDelayLabel(pacingPunctuationDelayMs_));
    settingsMenuItems_.push_back(uiText(UiText::ResetPacing));
  } else if (menuScreen_ == MenuScreen::SettingsSound) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back("Volume: " + soundVolumeLabel());
    settingsMenuItems_.push_back("Timer chime: " + focusTimerChimeLabel());
  } else if (menuScreen_ == MenuScreen::WifiSettings) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back("Network: " + storedOrFallbackLabel(configuredWifiSsid(), "Not set"));
    settingsMenuItems_.push_back("Choose network");
    settingsMenuItems_.push_back("Auto OTA: " + String(otaAutoCheckEnabled() ? "On" : "Off"));
    settingsMenuItems_.push_back("Forget network");
  } else if (menuScreen_ == MenuScreen::SettingsBattery) {
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back("CPU RSVP mode: " + cpuMhzLabel(cpuMhzPlay_));
    settingsMenuItems_.push_back("CPU scroll mode: " + cpuMhzLabel(cpuMhzScroll_));
    settingsMenuItems_.push_back("CPU paused (affects scroll in RSVP): " + cpuMhzLabel(cpuMhzPaused_));
    settingsMenuItems_.push_back("CPU menu: " + cpuMhzLabel(cpuMhzMenu_));
    settingsMenuItems_.push_back(
        "CPU standby: " + cpuMhzLabel(cpuMhzStandby_) +
        (cpuMhzStandby_ <= 40 ? " (Might affect animations)" : ""));
    settingsMenuItems_.push_back("Auto-dim delay: " + autoDimDelayLabel());
    settingsMenuItems_.push_back("Auto-dim brightness level: " + autoDimBrightnessLabel());
#if defined(BOARD_AMOLED_18)
    settingsMenuItems_.push_back("Standby after: " + deepStandbyDelayLabel());
#endif
  } else if (menuScreen_ == MenuScreen::SettingsClock) {
    char buf[8];
    settingsMenuItems_.push_back(uiText(UiText::Back));
    settingsMenuItems_.push_back("Sync via Wi-Fi");
    settingsMenuItems_.push_back(String("Auto clock: ") +
                                 (clock_.autoSyncEnabled() ? "On" : "Off"));
    settingsMenuItems_.push_back("Timezone: " + clock_.timezoneLabel());
    const BoardConfig::RtcDateTime &clockEdit = clock_.clockEdit();
    std::snprintf(buf, sizeof(buf), "%04u", clockEdit.year);
    settingsMenuItems_.push_back(String("Year: ") + buf);
    std::snprintf(buf, sizeof(buf), "%02u", clockEdit.month);
    settingsMenuItems_.push_back(String("Month: ") + buf);
    std::snprintf(buf, sizeof(buf), "%02u", clockEdit.day);
    settingsMenuItems_.push_back(String("Day: ") + buf);
    std::snprintf(buf, sizeof(buf), "%02u", clockEdit.hour);
    settingsMenuItems_.push_back(String("Hour: ") + buf);
    std::snprintf(buf, sizeof(buf), "%02u", clockEdit.minute);
    settingsMenuItems_.push_back(String("Minute: ") + buf);
    BoardConfig::RtcDateTime now;
    int32_t day = 0;
    if (clock_.localNow(now, day)) {
      std::snprintf(buf, sizeof(buf), "%02u:%02u", now.hour, now.minute);
      settingsMenuItems_.push_back(String("Now: ") + buf + " " + clock_.timezoneLabel());
    } else {
      settingsMenuItems_.push_back("Now: not set");
    }
  }

  if (settingsSelectedIndex_ >= settingsMenuItems_.size()) {
    settingsSelectedIndex_ = kSettingsBackIndex;
  }
}

void App::applyScrollConfig() {
  DisplayManager::ScrollConfig cfg;
  cfg.fontSizeDivisor = 2;
  cfg.letterSpacingPx = 0;
  cfg.wordSpacingPx = 10;
  cfg.showSearchIcon = false;
  display_.setScrollConfig(cfg);
}


void App::applyPacingSettings() {
  ReadingLoop::PacingConfig pacingConfig;
  pacingConfig.longWordDelayMs = pacingLongWordDelayMs_;
  pacingConfig.complexWordDelayMs = pacingComplexWordDelayMs_;
  pacingConfig.punctuationDelayMs = pacingPunctuationDelayMs_;
  reader_.setPacingConfig(pacingConfig);

  Serial.printf("[settings] pacing long=%u ms complexity=%u ms punctuation=%u ms\n",
                static_cast<unsigned int>(pacingLongWordDelayMs_),
                static_cast<unsigned int>(pacingComplexWordDelayMs_),
                static_cast<unsigned int>(pacingPunctuationDelayMs_));
  if (state_ == AppState::Menu && menuScreen_ == MenuScreen::SettingsPacing) {
    timeEstimate_.markPendingRebuild();
  } else {
    timeEstimate_.rebuild(currentBookPath_, currentBookTitle_);
  }
}

OtaUpdater::Config App::preferredOtaConfig() {
  OtaUpdater::Config otaConfig;
  ota_.loadConfig(otaConfig);

  if (preferences_.isKey(kPrefWifiSsid)) {
    otaConfig.wifiSsid = preferences_.getString(kPrefWifiSsid, "");
  }
  if (preferences_.isKey(kPrefWifiPass)) {
    otaConfig.wifiPassword = preferences_.getString(kPrefWifiPass, "");
  }
  if (preferences_.isKey(kPrefOtaAuto)) {
    otaConfig.autoCheck = preferences_.getBool(kPrefOtaAuto, otaConfig.autoCheck);
  }

  return otaConfig;
}

String App::configuredWifiSsid() {
  String ssid = preferences_.getString(kPrefWifiSsid, "");
  if (ssid.isEmpty()) {
    OtaUpdater::Config otaConfig;
    ota_.loadConfig(otaConfig);
    ssid = otaConfig.wifiSsid;
  }
  ssid.trim();
  return ssid;
}

bool App::otaAutoCheckEnabled() {
  if (preferences_.isKey(kPrefOtaAuto)) {
    return preferences_.getBool(kPrefOtaAuto, false);
  }

  OtaUpdater::Config otaConfig;
  ota_.loadConfig(otaConfig);
  return otaConfig.autoCheck;
}

void App::maybeAutoCheckForUpdates(uint32_t nowMs) {
  (void)nowMs;
  OtaUpdater::Config otaConfig = preferredOtaConfig();
  if (!otaConfig.autoCheck || !ota_.isConfigured(otaConfig)) {
    return;
  }

  Serial.println("[ota] auto-check enabled");
  ota_.startBackgroundCheck(otaConfig);
}

void App::maybeAutoSyncClock(uint32_t nowMs) {
  if (!clock_.shouldAutoSync()) {
    return;
  }
  if (clock_.startBackgroundSync(nowMs)) {
    Serial.println("[clock] auto-sync enabled");
  }
}

bool App::updateConfirmCanOpen() const {
  return ota_.updatePromptPending() && !pendingBootBookLoad_ && state_ == AppState::Paused;
}

void App::maybeOpenUpdateConfirm(uint32_t nowMs) {
  if (!updateConfirmCanOpen()) {
    return;
  }

  ota_.clearUpdatePrompt();
  setState(AppState::Menu, nowMs);
  openUpdateConfirm();
}

void App::runRssFeedCheck(uint32_t nowMs) {
  (void)nowMs;
  if (ota_.blockForCheck("RSS", nowMs)) {
    return;
  }

  saveReadingPosition(true);

  display_.renderStatus("RSS", "Checking feeds", "Please wait");
  const RssFeedManager::Result result =
      rssFeedManager_.checkFeeds(preferredOtaConfig(), preferences_, &App::handleStorageStatus, this);

  Serial.printf("[rss] feeds=%u saved=%u skipped=%u summary=%s detail=%s\n",
                static_cast<unsigned int>(result.feedsChecked),
                static_cast<unsigned int>(result.articlesSaved),
                static_cast<unsigned int>(result.articlesSkipped), result.summary.c_str(),
                result.detail.c_str());

  storage_.refreshBooks(false);
  display_.renderStatus("RSS", result.summary, result.detail);
  delay(1800);
  renderMainMenu();
}

String App::pacingDelayLabel(uint16_t delayMs) const { return String(delayMs) + " ms"; }

String App::firmwareUpdateMenuLabel() const { return "Firmware update"; }

String App::uiText(UiText key) const { return Localization::text(uiLanguage_, key); }

String App::themeModeLabel() const {
  if (nightMode_) {
    return uiText(UiText::Night);
  }
  return darkMode_ ? uiText(UiText::Dark) : uiText(UiText::Light);
}

String App::phantomWordsLabel() const {
  return phantomWordsEnabled_ ? uiText(UiText::On) : uiText(UiText::Off);
}

String App::focusHighlightLabel() const {
  return typographyConfig_.focusHighlight ? uiText(UiText::On) : uiText(UiText::Off);
}

String App::uiLanguageLabel() const { return Localization::languageName(uiLanguage_); }

String App::readerModeLabel() const {
  switch (readerMode_) {
    case ReaderMode::Scroll:
      return uiText(UiText::ScrollMode);
    case ReaderMode::Rsvp:
    default:
      return uiText(UiText::RsvpMode);
  }
}

String App::pauseModeLabel() const {
  return pauseMode_ == PauseMode::Instant ? "Instant" : "Sentence";
}

String App::handednessLabel() const {
  return handednessMode_ == HandednessMode::Left ? "Left" : "Right";
}

String App::readerFontSizeLabel() const {
  uint8_t levelIndex = readerFontSizeIndex_;
  if (levelIndex >= kReaderFontSizeCount) {
    levelIndex = 0;
  }

  switch (levelIndex) {
    case 0:
      return uiText(UiText::Large);
    case 1:
      return uiText(UiText::Medium);
    case 2:
    default:
      return uiText(UiText::Small);
  }
}

String App::readerTypefaceLabel() const {
  switch (typographyConfig_.typeface) {
    case DisplayManager::ReaderTypeface::AtkinsonHyperlegible:
      return "Atkinson";
    case DisplayManager::ReaderTypeface::OpenDyslexic:
      return "OpenDyslexic";
    case DisplayManager::ReaderTypeface::Standard:
    default:
      return uiText(UiText::Standard);
  }
}

String App::typographyTuningLabel() const {
  switch (typographyTuningSelectedIndex_) {
    case TypographyTuningBack:
      return uiText(UiText::Back);
    case TypographyTuningFontSize:
      return uiText(UiText::FontSize);
    case TypographyTuningTypeface:
      return uiText(UiText::Typeface);
    case TypographyTuningPhantomWords:
      return uiText(UiText::PhantomWords);
    case TypographyTuningFocusHighlight:
      return uiText(UiText::RedHighlight);
    case TypographyTuningTracking:
      return uiText(UiText::Tracking);
    case TypographyTuningAnchor:
      return uiText(UiText::Anchor);
    case TypographyTuningGuideWidth:
      return uiText(UiText::GuideWidth);
    case TypographyTuningGuideGap:
      return uiText(UiText::GuideGap);
    case TypographyTuningReset:
      return uiText(UiText::Reset);
    default:
      return uiText(UiText::Typography);
  }
}

String App::typographyTuningValueLabel() const {
  switch (typographyTuningSelectedIndex_) {
    case TypographyTuningBack:
      return uiText(UiText::TapToExit);
    case TypographyTuningFontSize:
      return readerFontSizeLabel();
    case TypographyTuningTypeface:
      return readerTypefaceLabel();
    case TypographyTuningPhantomWords:
      return phantomWordsLabel();
    case TypographyTuningFocusHighlight:
      return focusHighlightLabel();
    case TypographyTuningTracking:
      return String(typographyConfig_.trackingPx >= 0 ? "+" : "") +
             String(static_cast<int>(typographyConfig_.trackingPx)) + " px";
    case TypographyTuningAnchor:
      return String(static_cast<unsigned int>(effectiveAnchorPercent())) + "%";
    case TypographyTuningGuideWidth:
      return String(static_cast<unsigned int>(typographyConfig_.guideHalfWidth)) + " px";
    case TypographyTuningGuideGap:
      return String(static_cast<unsigned int>(typographyConfig_.guideGap)) + " px";
    case TypographyTuningReset:
      return uiText(UiText::TapToReset);
    default:
      return "";
  }
}

void App::openBookPicker(bool articlesOnly) {
  storage_.refreshBooks();
  bookMenuItems_.clear();
  bookPickerBookIndices_.clear();
  bookMenuItems_.push_back({uiText(UiText::Back), ""});

  const size_t count = storage_.bookCount();
  std::vector<size_t> sortedBookIndices;
  sortedBookIndices.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (storage_.bookIsArticle(i) != articlesOnly) {
      continue;
    }
    sortedBookIndices.push_back(i);
  }

  std::stable_sort(sortedBookIndices.begin(), sortedBookIndices.end(),
                   [this](size_t leftIndex, size_t rightIndex) {
                     const bool leftCurrent =
                         usingStorageBook_ && leftIndex == currentBookIndex_;
                     const bool rightCurrent =
                         usingStorageBook_ && rightIndex == currentBookIndex_;
                     if (leftCurrent != rightCurrent) {
                       return leftCurrent;
                     }

                     const uint32_t leftRecent =
                         library_.recentSequence(storage_.bookPath(leftIndex));
                     const uint32_t rightRecent =
                         library_.recentSequence(storage_.bookPath(rightIndex));
                     const bool leftHasRecent = leftRecent > 0;
                     const bool rightHasRecent = rightRecent > 0;
                     if (leftHasRecent != rightHasRecent) {
                       return leftHasRecent;
                     }
                     if (leftRecent != rightRecent) {
                       return leftRecent > rightRecent;
                     }

                     return false;
                   });

  for (size_t bookIndex : sortedBookIndices) {
    bookPickerBookIndices_.push_back(bookIndex);
    bookMenuItems_.push_back(libraryItemForBook(bookIndex));
  }

  if (sortedBookIndices.empty()) {
    Serial.printf("[book-picker] No SD %s available\n", articlesOnly ? "articles" : "books");
  }

  menuScreen_ = MenuScreen::BookPicker;
  bookPickerSelectedIndex_ = kBookPickerBackIndex;
  if (usingStorageBook_) {
    for (size_t row = 0; row < bookPickerBookIndices_.size(); ++row) {
      if (bookPickerBookIndices_[row] == currentBookIndex_) {
        bookPickerSelectedIndex_ = row + 1;
        break;
      }
    }
  }
  renderBookPicker();
}

void App::selectBookPickerItem(uint32_t nowMs) {
  if (bookPickerSelectedIndex_ == kBookPickerBackIndex || bookMenuItems_.size() <= 1) {
    menuScreen_ = MenuScreen::Main;
    renderMainMenu();
    return;
  }

  const size_t rowIndex = bookPickerSelectedIndex_ - 1;
  if (rowIndex >= bookPickerBookIndices_.size()) {
    renderBookPicker();
    return;
  }

  const size_t bookIndex = bookPickerBookIndices_[rowIndex];
  saveReadingPosition(true);
  if (!loadBookAtIndex(bookIndex, nowMs)) {
    Serial.println("[book-picker] Failed to load selected book");
    display_.renderStatus("Book open failed", storage_.bookDisplayName(bookIndex),
                          "Check serial log");
    delay(1800);
    renderBookPicker();
    return;
  }

  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
  saveReadingPosition(true);
}

void App::openChapterPicker() {
  chapterMenuItems_.clear();
  chapterMenuItems_.push_back(uiText(UiText::Back));

  if (chapterMarkers_.empty()) {
    chapterMenuItems_.push_back(uiText(UiText::StartOfBook));
    chapterPickerSelectedIndex_ = kChapterPickerFallbackIndex;
    Serial.println("[chapter-picker] No chapter markers found; showing start fallback");
  } else {
    for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
      chapterMenuItems_.push_back(chapterMenuLabel(i));
    }

    size_t selectedChapter = 0;
    const size_t currentWordIndex = reader_.currentIndex();
    for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
      if (chapterMarkers_[i].wordIndex <= currentWordIndex) {
        selectedChapter = i;
      }
    }
    chapterPickerSelectedIndex_ = selectedChapter + 1;
  }

  chapterMenuItems_.push_back(uiText(UiText::RestartBook));

  menuScreen_ = MenuScreen::ChapterPicker;
  renderChapterPicker();
}

void App::selectChapterPickerItem(uint32_t nowMs) {
  if (chapterPickerSelectedIndex_ == kChapterPickerBackIndex || chapterMenuItems_.size() <= 1) {
    menuScreen_ = MenuScreen::Main;
    renderMainMenu();
    return;
  }

  const size_t restartIndex = chapterMenuItems_.size() - 1;
  if (chapterPickerSelectedIndex_ == restartIndex) {
    openRestartConfirm();
    return;
  }

  if (chapterMarkers_.empty()) {
    reader_.seekTo(0);
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    saveReadingPosition(true);
    Serial.println("[chapter-picker] jumped to start of book");
    return;
  }

  const size_t chapterIndex = chapterPickerSelectedIndex_ - 1;
  if (chapterIndex >= chapterMarkers_.size()) {
    renderChapterPicker();
    return;
  }

  reader_.seekTo(chapterMarkers_[chapterIndex].wordIndex);
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
  saveReadingPosition(true);
  Serial.printf("[chapter-picker] jumped to %s at word %u\n",
                chapterMarkers_[chapterIndex].title.c_str(),
                static_cast<unsigned int>(chapterMarkers_[chapterIndex].wordIndex));
}

void App::openRestartConfirm() {
  restartConfirmReturnScreen_ = menuScreen_;
  restartConfirmSelectedIndex_ = RestartConfirmNo;
  menuScreen_ = MenuScreen::RestartConfirm;
  renderRestartConfirm();
}

void App::selectRestartConfirmItem(uint32_t nowMs) {
  if (restartConfirmSelectedIndex_ != RestartConfirmYes) {
    menuScreen_ = restartConfirmReturnScreen_;
    renderMenu();
    return;
  }

  reader_.begin(nowMs);
  restartConfirmReturnScreen_ = MenuScreen::Main;
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
  saveReadingPosition(true);
  Serial.println("[restart] book restarted from beginning");
}

void App::openSdCardRepairConfirm() {
  sdCardRepairConfirmSelectedIndex_ = SdCardRepairConfirmNo;
  menuScreen_ = MenuScreen::SdCardRepairConfirm;
  renderSdCardRepairConfirm();
}

void App::selectSdCardRepairConfirmItem(uint32_t nowMs) {
  if (sdCardRepairConfirmSelectedIndex_ != SdCardRepairConfirmYes) {
    Serial.println("[sd-check] folder repair declined");
    menuScreen_ = MenuScreen::Main;
    renderMenu();
    return;
  }

  runSdCardRepair(nowMs);
}

void App::openUpdateConfirm() {
  updateConfirmSelectedIndex_ = UpdateConfirmSkip;
  menuScreen_ = MenuScreen::UpdateConfirm;
  renderUpdateConfirm();
}

void App::selectUpdateConfirmItem(uint32_t nowMs) {
  if (updateConfirmSelectedIndex_ != UpdateConfirmUpdate) {
    Serial.println("[ota] update skipped by user");
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Paused, nowMs);
    return;
  }

  Serial.println("[ota] update confirmed by user");
  ota_.runFirmwareUpdate(preferredOtaConfig(), false, nowMs);
}

void App::openPowerOffConfirm(uint32_t nowMs) {
  powerOffConfirmReturnState_ = state_;
  powerOffConfirmReturnScreen_ = (state_ == AppState::Menu) ? menuScreen_ : MenuScreen::Main;
  powerOffConfirmSelectedIndex_ = PowerOffConfirmNo;
  if (state_ != AppState::Menu) {
    saveReadingPosition(true);
  }
  setState(AppState::Menu, nowMs);
  menuScreen_ = MenuScreen::PowerOffConfirm;
  renderPowerOffConfirm();
}

void App::selectPowerOffConfirmItem(uint32_t nowMs) {
  if (powerOffConfirmSelectedIndex_ != PowerOffConfirmYes) {
    Serial.println("[power-off] cancelled by user");
    const AppState returnState = powerOffConfirmReturnState_;
    menuScreen_ = powerOffConfirmReturnScreen_;
    if (returnState == AppState::Menu) {
      renderMenu();
    } else {
      setState(returnState == AppState::Playing ? AppState::Paused : returnState, nowMs);
    }
    return;
  }

  Serial.println("[power-off] confirmed by user");
  enterPowerOff(nowMs);
}

void App::renderPowerOffConfirm() {
  std::vector<String> items;
  items.reserve(PowerOffConfirmItemCount + kPowerOffConfirmHeaderRows);
  items.push_back("Power off?");
  items.push_back("Cancel");
  items.push_back("Yes");

  display_.renderMenu(items, powerOffConfirmSelectedIndex_ + kPowerOffConfirmHeaderRows);
}

void App::enterCompanionSync(uint32_t nowMs) {
  if (ota_.blockForCheck("Sync", nowMs)) {
    return;
  }

  Serial.println("[app] entering companion sync mode");
  saveReadingPosition(true);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  display_.renderStatus("Sync", "Starting Wi-Fi", "");

  OtaUpdater::Config wifiConfig = preferredOtaConfig();
  CompanionSyncManager::Config syncConfig;
  syncConfig.wifiSsid = wifiConfig.wifiSsid;
  syncConfig.wifiPassword = wifiConfig.wifiPassword;

  if (!companionSync_.begin(syncConfig)) {
    Serial.println("[app] companion sync failed");
    display_.renderStatus("Sync", "Could not start", "Returning");
    delay(1400);
    menuScreen_ = MenuScreen::Main;
    setState(AppState::Menu, nowMs);
    return;
  }

  lastCompanionSyncRenderMs_ = 0;
  setState(AppState::CompanionSync, nowMs);
}

void App::updateCompanionSync(uint32_t nowMs) {
  companionSync_.update();

  if (powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kUsbTransferExitHoldMs) {
    powerButtonLongPressHandled_ = true;
    exitCompanionSync(nowMs);
    return;
  }

  if (nowMs - lastCompanionSyncRenderMs_ >= 1000) {
    lastCompanionSyncRenderMs_ = nowMs;
    display_.renderStatus("Sync", companionSync_.statusLine1(), companionSync_.statusLine2());
  }
}

void App::exitCompanionSync(uint32_t nowMs) {
  Serial.println("[app] leaving companion sync mode");
  display_.renderStatus("Sync", "Stopping", "");
  companionSync_.end();
  preferences_.end();
  preferences_.begin(kPrefsNamespace, false);
  reloadRuntimePreferences(nowMs, false);
  storage_.refreshBooks();
  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
}

void App::runSdCardCheck(uint32_t nowMs) {
  (void)nowMs;
  Serial.println("[app] running SD card check");
  display_.renderStatus("SD check", "Starting", "");
  const StorageManager::DiagnosticResult result = storage_.diagnoseSdCard();

  if (sdCardFolderRepairNeeded(result)) {
    display_.renderStatus("SD check", "Folders missing", "Confirm repair");
    delay(900);
    openSdCardRepairConfirm();
    return;
  }

  String detail = result.detail;
  if (detail.isEmpty() && result.mounted) {
    detail = String(static_cast<unsigned int>(result.sizeMb)) + " MB";
  }
  display_.renderStatus("SD check", result.summary, detail);
  delay(2600);

  menuScreen_ = MenuScreen::Main;
  renderMenu();
}

void App::runSdCardRepair(uint32_t nowMs) {
  (void)nowMs;
  Serial.println("[app] repairing SD card folder layout");
  display_.renderStatus("SD check", "Repairing folders", "Please wait");
  const bool repaired = storage_.repairSdCardFolders();
  if (!repaired) {
    display_.renderStatus("SD check", "Folder repair failed", "Format FAT32 MBR");
    delay(2600);
    menuScreen_ = MenuScreen::Main;
    renderMenu();
    return;
  }

  display_.renderStatus("SD check", "Folders repaired", "Checking card");
  delay(900);

  const StorageManager::DiagnosticResult result = storage_.diagnoseSdCard();
  String detail = result.detail;
  if (detail.isEmpty() && result.mounted) {
    detail = String(static_cast<unsigned int>(result.sizeMb)) + " MB";
  }
  display_.renderStatus("SD check", result.summary, detail);
  delay(2600);

  menuScreen_ = MenuScreen::Main;
  renderMenu();
}

void App::enterUsbTransfer(uint32_t nowMs) {
  Serial.println("[app] entering USB transfer mode");
  saveReadingPosition(true);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  const size_t resumeIndex = reader_.currentIndex();
  setState(AppState::UsbTransfer, nowMs);

  activeBookStore_.close();
  storage_.end();
  if (!usbTransfer_.begin(true)) {
    Serial.printf("[app] USB transfer failed: %s\n", usbTransfer_.statusMessage());
    display_.renderStatus("USB", "SD not ready", "Returning");
    storageReady_ = storage_.begin();
    if (storageReady_ && usingStorageBook_ && !currentBookPath_.isEmpty()) {
      const int refreshedBookIndex = library_.findIndexByPath(currentBookPath_);
      if (refreshedBookIndex >= 0 &&
          loadBookAtIndex(static_cast<size_t>(refreshedBookIndex), nowMs, false, false, false,
                          false)) {
        reader_.seekTo(resumeIndex);
      }
    }
    setState(AppState::Paused, nowMs);
    return;
  }

  const uint64_t sizeMb = usbTransfer_.cardSizeBytes() / (1024ULL * 1024ULL);
  Serial.printf("[app] USB transfer active (%llu MB). Eject from computer when finished.\n",
                sizeMb);
  display_.renderStatus("USB", "Copy books now", "Eject then hold PWR");
}

void App::updateUsbTransfer(uint32_t nowMs) {
  if (!usbTransfer_.active()) {
    return;
  }

  const bool powerExitRequested =
      powerButton_.isHeld() && nowMs - powerButton_.lastEdgeMs() >= kUsbTransferExitHoldMs;
  if (!usbTransfer_.ejected() && !powerExitRequested) {
    return;
  }

  if (powerExitRequested && !usbTransfer_.ejected()) {
    Serial.println("[app] leaving USB transfer by PWR hold; make sure host was ejected first");
  }

  if (powerExitRequested) {
    powerButtonLongPressHandled_ = true;
  }

  exitUsbTransfer(nowMs);
}

void App::exitUsbTransfer(uint32_t nowMs) {
  Serial.println("[app] USB transfer ejected; remounting SD");
  display_.renderStatus("USB", "Remounting SD", "");
  usbTransfer_.end();

  storageReady_ = storage_.begin();
  if (storageReady_) {
    const int refreshedBookIndex = library_.findIndexByPath(currentBookPath_);
    if (refreshedBookIndex >= 0) {
      const size_t resumeIndex = reader_.currentIndex();
      if (loadBookAtIndex(static_cast<size_t>(refreshedBookIndex), nowMs, false, false, false,
                          false)) {
        reader_.seekTo(resumeIndex);
      } else {
        Serial.println("[app] current indexed book unavailable after USB transfer");
        usingStorageBook_ = false;
        currentBookPath_ = "";
        currentBookTitle_ = "Demo";
        reader_.clearLoadedBook(nowMs);
        reader_.begin(nowMs);
      }
    } else if (storage_.bookCount() > 0) {
      loadBookAtIndex(0, nowMs);
    }
  } else {
    Serial.println("[app] SD remount failed after USB transfer");
  }

  menuScreen_ = MenuScreen::Main;
  setState(AppState::Paused, nowMs);
}

void App::enterStandby(uint32_t nowMs) {
  if (state_ == AppState::UsbTransfer || state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_) {
    return;
  }

  standbyReturnState_ = state_ == AppState::Playing ? AppState::Paused : state_;
  if (standbyReturnState_ == AppState::Booting || standbyReturnState_ == AppState::Standby) {
    standbyReturnState_ = AppState::Paused;
  }

  if (state_ == AppState::Playing) {
    saveReadingPosition(true);
  }

  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  batteryWarningOverlayVisible_ = false;
  standbyEnteredMs_ = nowMs;
  standbyButtonsReleased_ = false;
  screensaver_.resetFrameTimer();
  setState(AppState::Standby, nowMs);
  Serial.println("[app] standby screensaver started");
}

void App::exitStandby(uint32_t nowMs) {
  if (state_ != AppState::Standby) {
    return;
  }

  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  batteryWarningOverlayVisible_ = false;
  standbyButtonsReleased_ = false;

  AppState nextState = standbyReturnState_;
  if (nextState == AppState::Booting || nextState == AppState::Playing ||
      nextState == AppState::CompanionSync || nextState == AppState::UsbTransfer ||
      nextState == AppState::Standby || nextState == AppState::Sleeping) {
    nextState = AppState::Paused;
  }

  Serial.println("[app] leaving standby");
  screensaver_.wakeIfScreenOff();
  setState(nextState, nowMs);
}

void App::noteActivity(uint32_t nowMs) {
  lastActivityMs_ = nowMs;
  lastUserActivityMs_ = nowMs;
}

#if defined(BOARD_AMOLED_18)
void App::updateIdleStandby(uint32_t nowMs) {
  // Never blank while reading or a focus timer is running (the timer screen
  // counts down with no touch to refresh activity).
  if (shouldStayAwake()) {
    lastActivityMs_ = nowMs;
    return;
  }
  // Only the resting states accrue idle time. Reading (Playing) holds activity
  // alive via continuous touch; utility/sync/boot screens manage their own life.
  if (state_ != AppState::Paused && state_ != AppState::Menu &&
      state_ != AppState::Finished) {
    lastActivityMs_ = nowMs;
    return;
  }
  if (lastActivityMs_ == 0) {
    lastActivityMs_ = nowMs;
    return;
  }
  if (nowMs - lastActivityMs_ >= kIdleStandbyTimeoutMs) {
    enterStandby(nowMs);
  }
}

void App::handleAmoledStandbyWake(uint32_t nowMs) {
  if (state_ != AppState::Standby) {
    return;
  }
  // Ignore input briefly so the gesture/idle that led here cannot bounce back.
  if (nowMs - standbyEnteredMs_ < kStandbyWakeGraceMs) {
    if (touchInitialized_) {
      TouchEvent drop;
      touch_.poll(drop);  // Drain so a held touch is not queued as a wake.
    }
    return;
  }

  // Wake on a BOOT press...
  if (button_.wasReleasedEvent() || button_.wasPressedEvent()) {
    bootButtonLongPressHandled_ = true;
    exitStandby(nowMs);
    noteActivity(nowMs);
    return;
  }

  // ...or on any screen touch.
  if (touchInitialized_) {
    TouchEvent ev;
    if (touch_.poll(ev) && ev.touched) {
      touch_.cancel();
      exitStandby(nowMs);
      noteActivity(nowMs);
    }
  }
}

void App::enterPowerSaving(uint32_t nowMs) {
  if (state_ == AppState::PowerSaving || state_ == AppState::Booting ||
      state_ == AppState::UsbTransfer || state_ == AppState::CompanionSync ||
      state_ == AppState::Sleeping || powerOffStarted_) {
    return;
  }

  // Remember a sensible place to come back to; reading is saved and resumes paused.
  AppState ret = state_;
  if (ret == AppState::Playing) {
    saveReadingPosition(true);
    ret = AppState::Paused;
  }
  powerSaveReturnState_ = (ret == AppState::Menu) ? AppState::Menu : AppState::Paused;

  // Sleep the panel. NOTE: do NOT touch_.end() here — TouchHandler::end() calls
  // Wire.end(), and the AXP2101 PMU shares that same I2C bus (pins 15/14). Killing
  // the bus would make the PWR key unreadable, so we could never wake. Instead we
  // simply stop polling touch (the PowerSaving update branch skips handleTouch),
  // which disables touch input while leaving the bus alive for the PMU.
  if (touchInitialized_) {
    touch_.cancel();
  }
  display_.prepareForSleep();
  screensaver_.clearScreenOff();  // clear any leftover screensaver-standby flag

  powerSaveEnteredMs_ = nowMs;
  setState(AppState::PowerSaving, nowMs);
  Serial.println("[power] deep standby on (screen + touch off; tap PWR to wake)");
}

void App::exitPowerSaving(uint32_t nowMs) {
  if (state_ != AppState::PowerSaving) {
    return;
  }

  display_.wakeFromSleep();
  if (touchInitialized_) {
    touch_.cancel();  // discard any stale touch state accrued while parked
  }

  AppState next = powerSaveReturnState_;
  if (next != AppState::Paused && next != AppState::Menu) {
    next = AppState::Paused;
  }
  setState(next, nowMs);
  noteActivity(nowMs);
  Serial.println("[power] deep standby off (woke on PWR tap)");
}

void App::updateDeepStandbyIdle(uint32_t nowMs) {
  if (deepStandbyDelayMs_ == 0 || state_ == AppState::PowerSaving) {
    return;  // disabled, or already there.
  }
  if (shouldStayAwake()) {  // reading / running timer -> stay fully on
    lastActivityMs_ = nowMs;
    return;
  }
  // Eligible from the resting states and from the screensaver standby. Other
  // states (reading, sync, USB, boot) keep their own activity alive elsewhere.
  if (state_ != AppState::Paused && state_ != AppState::Menu &&
      state_ != AppState::Finished && state_ != AppState::Standby) {
    return;
  }
  if (lastActivityMs_ == 0) {
    lastActivityMs_ = nowMs;
    return;
  }
  if (nowMs - lastActivityMs_ >= deepStandbyDelayMs_) {
    enterPowerSaving(nowMs);
  }
}

String App::deepStandbyDelayLabel() const {
  if (deepStandbyDelayMs_ == 0) {
    return "Off";
  }
  if (deepStandbyDelayMs_ <= 60000) {
    return "1min";
  }
  if (deepStandbyDelayMs_ <= 180000) {
    return "3min";
  }
  if (deepStandbyDelayMs_ <= 300000) {
    return "5min";
  }
  return "10min";
}
#endif  // BOARD_AMOLED_18

void App::restartToBootloader(uint32_t nowMs) {
  Serial.println("[app] restarting into USB bootloader");
  if (state_ == AppState::Playing && nowMs >= playingStartedMs_) {
    lifetimeReadMs_ += nowMs - playingStartedMs_;
    lifetimeStatsDirty_ = true;
  }
  saveReadingPosition(true);
  saveLifetimeStats();

  display_.renderStatus("Bootloader", "Restarting", "Use USB flasher");
  delay(700);

  activeBookStore_.close();
  storage_.end();
  Serial.flush();

#if CONFIG_TINYUSB_ENABLED
  usb_persist_restart(RESTART_BOOTLOADER);
#else
  ESP.restart();
#endif
}

void App::enterPowerOff(uint32_t nowMs) {
  if (powerOffStarted_) {
    return;
  }

#if defined(BOARD_AMOLED_18)
  Serial.println("[app] soft power off; PWR wakes from PMU-polled standby");
  // Count the final reading segment and persist lifetime totals before sleeping
  // (this sets state directly, bypassing setState's normal accrual).
  if (state_ == AppState::Playing && nowMs >= playingStartedMs_) {
    lifetimeReadMs_ += nowMs - playingStartedMs_;
    lifetimeStatsDirty_ = true;
  }
  saveReadingPosition(true);
  saveLifetimeStats();
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  playLocked_ = false;
  pauseAtSentenceEndRequested_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  batteryWarningOverlayVisible_ = false;
  menuScreen_ = MenuScreen::Main;

  display_.renderStatus("Goodbye", "", "");
  delay(1200);  // let the user see the farewell before the panel blanks
  if (touchInitialized_) {
    touch_.cancel();
  }
  display_.prepareForSleep();
  screensaver_.clearScreenOff();

  powerSaveReturnState_ = AppState::Paused;
  powerSaveEnteredMs_ = millis();
  setState(AppState::PowerSaving, powerSaveEnteredMs_);
  return;
#endif

  powerOffStarted_ = true;
  Serial.println("[app] powering off; hold PWR to start again");
  // Count the final reading segment and persist lifetime totals before sleeping
  // (this sets state directly, bypassing setState's normal accrual).
  if (state_ == AppState::Playing && nowMs >= playingStartedMs_) {
    lifetimeReadMs_ += nowMs - playingStartedMs_;
    lifetimeStatsDirty_ = true;
  }
  saveReadingPosition(true);
  saveLifetimeStats();
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  touchPlayHeld_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  menuScreen_ = MenuScreen::Main;
  state_ = AppState::Sleeping;

  display_.renderStatus("Goodbye", "", "");
  delay(1200);  // let the user see the farewell before the panel blanks
  display_.prepareForSleep();

  activeBookStore_.close();
  storage_.end();
  touch_.end();
  touchInitialized_ = false;
  Serial.flush();

  BoardConfig::holdBacklightOffForDeepSleep();

#if defined(BOARD_AMOLED_18)
  // The PWR button is the AXP2101 PWRKEY, not a GPIO, so there is no valid ext0
  // wake pin (arming the unused GPIO18 woke instantly -> self-reopen).
  if (BoardConfig::pmuVbusPresent()) {
    // On USB: cutting the PMU rail just makes VBUS repower it (POWERON reboot),
    // so don't shut down. Park in deep sleep with NO wake source — the device
    // stays dark until USB is unplugged/replugged.
    Serial.println("[power] on USB; deep sleep (cannot truly power off until on battery)");
    esp_deep_sleep_start();
  } else {
    // On battery: really cut the rail. Only a PWRKEY hold powers back on.
    Serial.println("[power] on battery; AXP2101 shutdown");
    BoardConfig::pmuShutdown();
    esp_deep_sleep_start();  // fallback if shutdown somehow returns
  }
#else
  // Bar board: real PWR button on a GPIO. Cut the soft-latch and wake on it.
  BoardConfig::pmuShutdown();
  BoardConfig::releaseBatteryPowerHold();
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BoardConfig::PIN_PWR_BUTTON), 0);
  esp_deep_sleep_start();
#endif
}

void App::enterSleep(uint32_t nowMs) {
  Serial.println("[app] entering light sleep; press BOOT to wake");
  saveReadingPosition(true);
  setState(AppState::Sleeping, nowMs);
  Serial.flush();
  delay(200);

  display_.prepareForSleep();
  activeBookStore_.close();
  storage_.end();
  touch_.end();
  touchInitialized_ = false;

  BoardConfig::lightSleepUntilBootButton();
  wakeFromSleep();
}

void App::wakeFromSleep() {
  const uint32_t nowMs = millis();
  Serial.println("[app] woke from light sleep");

  BoardConfig::begin();
  button_.begin();
  powerButton_.begin();
  bootButtonReleasedSinceBoot_ = !button_.isHeld();
  bootButtonLongPressHandled_ = false;
  powerButtonReleasedSinceBoot_ = !powerButton_.isHeld();
  powerButtonLongPressHandled_ = false;
  powerOffStarted_ = false;
  updateBatteryStatus(nowMs, true);
  storage_.setStatusCallback(&App::handleStorageStatus, this);
  pausedTouch_.active = false;
  pausedTouchIntent_ = TouchIntent::None;
  wpmFeedbackVisible_ = false;
  menuScreen_ = MenuScreen::Main;
  lastStateLogMs_ = nowMs;
  lastUserActivityMs_ = nowMs;
  state_ = AppState::Paused;
  applyStateCpuFrequency();

  const bool displayReady = display_.wakeFromSleep();
  touchInitialized_ = touch_.begin();
  storageReady_ = storage_.begin();

  if (storageReady_ && usingStorageBook_ && !currentBookPath_.isEmpty()) {
    const size_t resumeIndex = reader_.currentIndex();
    const int refreshedBookIndex = library_.findIndexByPath(currentBookPath_);
    if (refreshedBookIndex >= 0 &&
        loadBookAtIndex(static_cast<size_t>(refreshedBookIndex), nowMs, false, false, false,
                        false)) {
      reader_.seekTo(resumeIndex);
    } else {
      Serial.println("[app] current indexed book unavailable after wake");
      usingStorageBook_ = false;
      currentBookPath_ = "";
      currentBookTitle_ = "Demo";
      reader_.clearLoadedBook(nowMs);
      reader_.begin(nowMs);
    }
  }

  if (displayReady) {
    renderActiveReader(nowMs);
  }
}

bool App::restoreSavedBook(uint32_t nowMs) {
  const String savedPath = preferences_.getString(kPrefBookPath, "");
  if (savedPath.isEmpty()) {
    return false;
  }

  const int bookIndex = library_.findIndexByPath(savedPath);
  if (bookIndex < 0) {
    Serial.printf("[app] saved book not found: %s\n", savedPath.c_str());
    return false;
  }

  if (!loadBookAtIndex(static_cast<size_t>(bookIndex), nowMs, true, false, false, false)) {
    return false;
  }

  Serial.printf("[app] restored %s at word %u\n", savedPath.c_str(),
                static_cast<unsigned int>(reader_.currentIndex()));
  return true;
}

bool App::prepareBootBookLoad() {
  pendingBootBookIndex_ = 0;
  pendingBootBookLegacyFallback_ = false;

  if (!storageReady_ || storage_.bookCount() == 0) {
    return false;
  }

  const String savedPath = preferences_.getString(kPrefBookPath, "");
  if (!savedPath.isEmpty()) {
    const int savedBookIndex = library_.findIndexByPath(savedPath);
    if (savedBookIndex >= 0) {
      pendingBootBookIndex_ = static_cast<size_t>(savedBookIndex);
      pendingBootBookLegacyFallback_ = true;
      Serial.printf("[app] deferred saved book load: %s\n", savedPath.c_str());
      return true;
    }

    Serial.printf("[app] saved book not found: %s\n", savedPath.c_str());
  }

  pendingBootBookIndex_ = 0;
  pendingBootBookLegacyFallback_ = false;
  Serial.println("[app] deferred first book load");
  return true;
}

void App::loadPendingBootBook(uint32_t nowMs) {
  if (!pendingBootBookLoad_ || state_ != AppState::Paused) {
    return;
  }

  pendingBootBookLoad_ = false;
  display_.renderStatus("Loading book", currentBookTitle_, "Please wait");
  const uint32_t startedMs = millis();
  const bool allowIndexBuild = pendingBootBookLegacyFallback_;
  const bool loaded = loadBookAtIndex(pendingBootBookIndex_, nowMs,
                                      pendingBootBookLegacyFallback_, allowIndexBuild, false,
                                      false);
  const uint32_t elapsedMs = millis() - startedMs;
  Serial.printf("[app] deferred book load %s in %lu ms\n", loaded ? "ok" : "failed",
                static_cast<unsigned long>(elapsedMs));

  if (loaded) {
    usingStorageBook_ = true;
    renderActiveReader(millis());
    return;
  }

  usingStorageBook_ = false;
  chapterMarkers_.clear();
  paragraphStarts_.clear();
  currentBookPath_ = "";
  currentBookTitle_ = "Demo";
  reader_.begin(millis());
  invalidateContextPreviewWindow();
  Serial.println("[app] using built-in demo text");
  renderActiveReader(millis());
}

void App::saveReadingPosition(bool force) {
  if (!usingStorageBook_ || currentBookPath_.isEmpty()) {
    return;
  }

  const size_t wordIndex = reader_.currentIndex();
  if (!force && wordIndex == lastSavedWordIndex_) {
    return;
  }

  preferences_.putString(kPrefBookPath, currentBookPath_);
  library_.rememberPosition(currentBookPath_, static_cast<uint32_t>(wordIndex));
  library_.rememberWordCount(currentBookPath_, static_cast<uint32_t>(reader_.wordCount()));
  preferences_.putUShort(kPrefWpm, reader_.wpm());
  library_.markRecent(currentBookPath_);
  lastSavedWordIndex_ = wordIndex;
  Serial.printf("[app] saved position word=%u book=%s\n", static_cast<unsigned int>(wordIndex),
                currentBookPath_.c_str());
}

bool App::loadBookAtIndex(size_t index, uint32_t nowMs, bool allowLegacyPositionFallback,
                          bool allowIndexBuild, bool allowEpubConversion,
                          bool rebuildTimeEstimate) {
  BookMetadata book;
  String loadedPath;
  size_t loadedIndex = index;
  const String initialLabel = storage_.bookDisplayName(index);
  renderStorageStatus("Opening book", initialLabel.c_str(),
                      allowIndexBuild ? "Checking index" : "Checking saved index", 5);
  if (!storage_.loadIndexedBook(index, activeBookStore_, book, &loadedPath, &loadedIndex,
                                allowIndexBuild, allowEpubConversion)) {
    return false;
  }

  const String loadedTitle = book.title.isEmpty() ? displayNameForPath(loadedPath) : book.title;
  renderStorageStatus("Opening book", loadedTitle.c_str(), "Loading word cache", 70);

  const bool keepingExistingTimeCache =
      !rebuildTimeEstimate && timeEstimate_.cacheValid() && currentBookPath_ == loadedPath;
  reader_.setWordSource(&activeBookStore_, nowMs);
  if (reader_.wordCount() == 0 || reader_.currentWord().isEmpty()) {
    Serial.printf("[app] failed to read first indexed word from %s\n", loadedPath.c_str());
    activeBookStore_.close();
    reader_.clearLoadedBook(nowMs);
    renderStorageStatus("Book open failed", loadedTitle.c_str(), "Word cache unreadable", 100);
    return false;
  }

  chapterMarkers_ = std::move(book.chapters);
  paragraphStarts_ = std::move(book.paragraphStarts);
  invalidateContextPreviewWindow();
  currentBookIndex_ = loadedIndex;
  currentBookPath_ = loadedPath;
  currentBookTitle_ = loadedTitle;
  lastSavedWordIndex_ = static_cast<size_t>(-1);
  readingSessionMs_ = 0;
  wordsReadThisSession_ = 0;
  playingStartedMs_ = nowMs;
  usingStorageBook_ = true;
  preferences_.putString(kPrefBookPath, currentBookPath_);
  library_.rememberWordCount(currentBookPath_, static_cast<uint32_t>(reader_.wordCount()));
  library_.markRecent(currentBookPath_);

  // Re-opening a finished book restarts it from the beginning (its saved
  // position is the last word, which would otherwise instantly re-finish).
  if (library_.isFinished(currentBookPath_)) {
    library_.setFinished(currentBookPath_, false);
    reader_.seekTo(0);
    lastSavedWordIndex_ = reader_.currentIndex();
    saveReadingPosition(true);
    Serial.printf("[app] reopened finished book, restarting from start: %s\n",
                  currentBookPath_.c_str());
  } else {
    const uint32_t savedWordIndex =
        library_.savedWordIndex(currentBookPath_, allowLegacyPositionFallback);
    if (savedWordIndex != BookLibraryStore::kNoSavedWordIndex) {
      renderStorageStatus("Opening book", currentBookTitle_.c_str(), "Restoring position", 78);
      reader_.seekTo(savedWordIndex);
      lastSavedWordIndex_ = reader_.currentIndex();
      Serial.printf("[app] restored book position word=%u book=%s\n",
                    static_cast<unsigned int>(reader_.currentIndex()),
                    currentBookPath_.c_str());
    }
  }

  if (rebuildTimeEstimate) {
    timeEstimate_.rebuild(currentBookPath_, currentBookTitle_);
  } else if (!keepingExistingTimeCache) {
    timeEstimate_.invalidate();
  } else {
    renderStorageStatus("Opening book", currentBookTitle_.c_str(), "Using cached estimate", 92);
  }

  lastProgressSaveMs_ = nowMs;
  Serial.printf("[app] loaded SD book[%u/%u]: %s (%u chapters, %u paragraphs)\n",
                static_cast<unsigned int>(loadedIndex + 1),
                static_cast<unsigned int>(storage_.bookCount()), loadedPath.c_str(),
                static_cast<unsigned int>(chapterMarkers_.size()),
                static_cast<unsigned int>(paragraphStarts_.size()));
  return true;
}

bool App::bookProgressPercent(size_t bookIndex, uint8_t &percent) {
  if (usingStorageBook_ && bookIndex == currentBookIndex_) {
    const size_t wordCount = reader_.wordCount();
    if (wordCount <= 1) {
      return false;
    }
    const size_t wordIndex = std::min(reader_.currentIndex(), wordCount - 1);
    const size_t progress = (wordIndex * static_cast<size_t>(100)) / (wordCount - 1);
    percent = static_cast<uint8_t>(std::min(static_cast<size_t>(100), progress));
    return true;
  }
  return library_.savedProgressPercent(storage_.bookPath(bookIndex), percent);
}

void App::renderMenu() {
  if (!isFocusTimerMenuScreen(menuScreen_)) {
    applyReaderUiOrientation();
  }

  if (menuScreen_ == MenuScreen::SettingsHome || menuScreen_ == MenuScreen::SettingsDisplay ||
      menuScreen_ == MenuScreen::SettingsPacing || menuScreen_ == MenuScreen::SettingsBattery ||
      menuScreen_ == MenuScreen::SettingsSound ||
      menuScreen_ == MenuScreen::SettingsClock ||
      menuScreen_ == MenuScreen::WifiSettings) {
    renderSettings();
  } else if (menuScreen_ == MenuScreen::WifiNetworks) {
    renderWifiNetworks();
  } else if (menuScreen_ == MenuScreen::TextEntry) {
    textEntry_.render();
  } else if (menuScreen_ == MenuScreen::TypographyTuning) {
    renderTypographyTuning();
  } else if (menuScreen_ == MenuScreen::BookPicker) {
    renderBookPicker();
  } else if (menuScreen_ == MenuScreen::ChapterPicker) {
    renderChapterPicker();
  } else if (menuScreen_ == MenuScreen::ReadingStats) {
    renderReadingStats();
  } else if (menuScreen_ == MenuScreen::RestartConfirm) {
    renderRestartConfirm();
  } else if (menuScreen_ == MenuScreen::SdCardRepairConfirm) {
    renderSdCardRepairConfirm();
  } else if (menuScreen_ == MenuScreen::UpdateConfirm) {
    renderUpdateConfirm();
  } else if (menuScreen_ == MenuScreen::PowerOffConfirm) {
    renderPowerOffConfirm();
  } else if (menuScreen_ == MenuScreen::FocusTimerPresets) {
    renderFocusTimerPresets();
  } else if (menuScreen_ == MenuScreen::FocusTimerSession) {
    renderFocusTimerSession();
  } else {
    renderMainMenu();
  }
}

void App::renderMainMenu() {
  std::vector<String> items;
  items.reserve(MenuItemCount);
  items.push_back(uiText(UiText::Resume));
  items.push_back(uiText(UiText::Chapters));
  items.push_back("Books");
  items.push_back("Articles");
  items.push_back("Reading stats");
  items.push_back("Focus Timer");
  items.push_back(uiText(UiText::Settings));
  items.push_back("SD card check");
  items.push_back("RSS feeds");
  items.push_back("Companion sync");
#if RSVP_USB_TRANSFER_ENABLED
  items.push_back(uiText(UiText::UsbTransfer));
#endif
  items.push_back(uiText(UiText::PowerOff));
  display_.renderMenu(items, menuSelectedIndex_);
}

void App::renderSettings() {
  if (settingsMenuItems_.empty()) {
    rebuildSettingsMenuItems();
  }
  display_.renderMenu(settingsMenuItems_, settingsSelectedIndex_);
}

void App::renderTypographyTuning() {
  if (kTypographyPreviewWordCount == 0) {
    display_.renderStatus(uiText(UiText::Typography), uiText(UiText::NoSamples), "");
    return;
  }

  if (typographyPreviewSampleIndex_ >= kTypographyPreviewWordCount) {
    typographyPreviewSampleIndex_ = 0;
  }
  if (typographyTuningSelectedIndex_ >= TypographyTuningItemCount) {
    typographyTuningSelectedIndex_ = TypographyTuningFontSize;
  }

  const size_t index = typographyPreviewSampleIndex_;
  const size_t beforeIndex =
      index == 0 ? kTypographyPreviewWordCount - 1 : index - 1;
  const size_t afterIndex =
      (index + 1 >= kTypographyPreviewWordCount) ? 0 : index + 1;
  const String beforeText = phantomWordsEnabled_ ? kTypographyPreviewWords[beforeIndex] : "";
  const String afterText = phantomWordsEnabled_ ? kTypographyPreviewWords[afterIndex] : "";
  const String line1 = typographyTuningLabel() + ": " + typographyTuningValueLabel();
  const String title =
      uiText(UiText::Typography) + " " + String(static_cast<unsigned int>(index + 1)) + "/" +
      String(static_cast<unsigned int>(kTypographyPreviewWordCount));
  String line2 = uiText(UiText::TapChangeSample);
  if (typographyTuningSelectedIndex_ == TypographyTuningBack) {
    line2 = uiText(UiText::TapExitSample);
  } else if (typographyTuningSelectedIndex_ == TypographyTuningPhantomWords ||
             typographyTuningSelectedIndex_ == TypographyTuningFocusHighlight) {
    line2 = uiText(UiText::TapToggleSample);
  } else if (typographyTuningSelectedIndex_ == TypographyTuningFontSize ||
             typographyTuningSelectedIndex_ == TypographyTuningTypeface) {
    line2 = uiText(UiText::TapCycleSample);
  } else if (typographyTuningSelectedIndex_ == TypographyTuningReset) {
    line2 = uiText(UiText::TapToReset);
  }

  display_.renderTypographyPreview(beforeText,
                                   kTypographyPreviewWords[index],
                                   afterText,
                                   readerFontSizeIndex_, title, line1, line2);
}

void App::renderBookPicker() {
  display_.renderLibrary(bookMenuItems_, bookPickerSelectedIndex_);
}

void App::enterBookFinished(uint32_t nowMs) {
  // Persist the final position and the read flag before showing the summary.
  saveReadingPosition(true);
  library_.setFinished(currentBookPath_, true);
  ++lifetimeBooksFinished_;
  lifetimeStatsDirty_ = true;
  saveLifetimeStats();
  Serial.printf("[app] book finished: %s (%u words, %lus session)\n",
                currentBookTitle_.c_str(), static_cast<unsigned int>(reader_.wordCount()),
                static_cast<unsigned long>(readingSessionMs_ / 1000UL));
  // setState() folds the trailing Playing segment into readingSessionMs_.
  setState(AppState::Finished, nowMs);
}

void App::renderBookFinished() {
  // Title (truncated for the tiny line) plus a one-line stats summary.
  String title = currentBookTitle_;
  if (title.isEmpty()) {
    title = "Book";
  }
  constexpr size_t kMaxTitleChars = 40;
  if (title.length() > kMaxTitleChars) {
    title = title.substring(0, kMaxTitleChars - 3) + "...";
  }

  const size_t totalWords = reader_.wordCount();
  String stats = String(static_cast<unsigned long>(totalWords)) + " words";

  uint32_t avgWpm = 0;
  if (readingSessionMs_ >= 1000UL && wordsReadThisSession_ > 0) {
    avgWpm = static_cast<uint32_t>(
        (static_cast<uint64_t>(wordsReadThisSession_) * 60000ULL) / readingSessionMs_);
  }
  if (avgWpm > 0) {
    stats += "  -  " + String(avgWpm) + " wpm";
  }

  const uint32_t totalSeconds = readingSessionMs_ / 1000UL;
  String timeStr;
  if (totalSeconds >= 3600UL) {
    timeStr = String(totalSeconds / 3600UL) + "h " + String((totalSeconds % 3600UL) / 60UL) + "m";
  } else if (totalSeconds >= 60UL) {
    timeStr = String(totalSeconds / 60UL) + "m " + String(totalSeconds % 60UL) + "s";
  } else {
    timeStr = String(totalSeconds) + "s";
  }
  stats += "  -  " + timeStr;

  display_.renderStatus("Finished", title, stats);
}

void App::loadLifetimeStats() {
  lifetimeWordsRead_ = preferences_.getUInt(kPrefLifetimeWords, 0);
  lifetimeReadMs_ = preferences_.getULong64(kPrefLifetimeMs, 0);
  lifetimeBooksFinished_ = preferences_.getUInt(kPrefLifetimeBooks, 0);
  streakDays_ = preferences_.getUInt(kPrefStreakDays, 0);
  streakLastDay_ = preferences_.getInt(kPrefStreakLastDay, 0);
  lifetimeStatsDirty_ = false;
}

void App::updateStreakForToday() {
  // Count today (local) as a reading day. Needs a valid RTC; if the clock was
  // never set we silently skip until the time is available.
  BoardConfig::RtcDateTime local;
  int32_t today = 0;
  if (!clock_.localNow(local, today)) {
    return;
  }

  if (streakLastDay_ == 0) {
    streakDays_ = 1;  // first ever reading day
  } else if (today == streakLastDay_) {
    return;  // already counted today
  } else if (today == streakLastDay_ + 1) {
    ++streakDays_;  // consecutive day
  } else {
    streakDays_ = 1;  // gap (or clock moved backwards) -> restart
  }

  streakLastDay_ = today;
  preferences_.putUInt(kPrefStreakDays, streakDays_);
  preferences_.putInt(kPrefStreakLastDay, streakLastDay_);
  Serial.printf("[streak] day=%ld streak=%lu\n", static_cast<long>(today),
                static_cast<unsigned long>(streakDays_));
}

void App::saveLifetimeStats() {
  if (!lifetimeStatsDirty_) {
    return;
  }
  preferences_.putUInt(kPrefLifetimeWords, lifetimeWordsRead_);
  preferences_.putULong64(kPrefLifetimeMs, lifetimeReadMs_);
  preferences_.putUInt(kPrefLifetimeBooks, lifetimeBooksFinished_);
  lifetimeStatsDirty_ = false;
}

void App::openReadingStats() {
  saveLifetimeStats();  // flush any in-RAM deltas so the numbers are current
  readingStatsItems_.clear();
  readingStatsItems_.push_back(uiText(UiText::Back));

  // Words read, grouped with thousands separators.
  char numBuf[16];
  std::snprintf(numBuf, sizeof(numBuf), "%lu", static_cast<unsigned long>(lifetimeWordsRead_));
  String digits = numBuf;
  String grouped;
  int counted = 0;
  for (int i = static_cast<int>(digits.length()) - 1; i >= 0; --i) {
    grouped = String(digits[i]) + grouped;
    if (++counted % 3 == 0 && i > 0) {
      grouped = "," + grouped;
    }
  }
  readingStatsItems_.push_back(String("Words: ") + grouped);

  // Total reading time.
  const uint32_t totalMin = static_cast<uint32_t>(lifetimeReadMs_ / 60000ULL);
  String timeStr;
  if (totalMin >= 60) {
    timeStr = String(totalMin / 60) + "h " + String(totalMin % 60) + "m";
  } else {
    timeStr = String(totalMin) + "m";
  }
  readingStatsItems_.push_back(String("Time: ") + timeStr);

  readingStatsItems_.push_back(String("Books read: ") + String(lifetimeBooksFinished_));

  // Lifetime average reading speed.
  uint32_t avgWpm = 0;
  if (lifetimeReadMs_ >= 60000ULL) {
    avgWpm = static_cast<uint32_t>(
        (static_cast<uint64_t>(lifetimeWordsRead_) * 60000ULL) / lifetimeReadMs_);
  }
  readingStatsItems_.push_back(String("Avg speed: ") + String(avgWpm) + " wpm");

  // Reading streak + clock state (PCF85063 RTC, shown in local time).
  BoardConfig::RtcDateTime now;
  int32_t today = 0;
  const bool haveClock = clock_.localNow(now, today);
  if (haveClock) {
    readingStatsItems_.push_back(String("Streak: ") + String(streakDays_) + " days");
    char clockBuf[24];
    std::snprintf(clockBuf, sizeof(clockBuf), "%04u-%02u-%02u %02u:%02u", now.year, now.month,
                  now.day, now.hour, now.minute);
    readingStatsItems_.push_back(String("Clock: ") + clockBuf);
    readingStatsItems_.push_back(String("Zone: ") + clock_.timezoneLabel());
  } else {
    readingStatsItems_.push_back("Streak: clock not set");
    readingStatsItems_.push_back("Set in Settings > Clock");
  }

  readingStatsSelectedIndex_ = 0;  // "Back" highlighted
  menuScreen_ = MenuScreen::ReadingStats;
  renderReadingStats();
}

void App::renderReadingStats() {
  display_.renderMenu(readingStatsItems_, readingStatsSelectedIndex_);
}

void App::selectReadingStatsItem(uint32_t nowMs) {
  // Read-only screen: only "Back" (row 0) acts. Clock setup lives in Settings.
  (void)nowMs;
  if (readingStatsSelectedIndex_ != 0) {
    return;
  }
  menuScreen_ = MenuScreen::Main;
  renderMainMenu();
}

void App::openClockSettings() {
  BoardConfig::RtcDateTime &clockEdit = clock_.clockEdit();
  BoardConfig::RtcDateTime local;
  int32_t day = 0;
  if (clock_.localNow(local, day)) {
    clockEdit = local;
  } else {
    clockEdit = BoardConfig::RtcDateTime{};
    clockEdit.year = 2026;
    clockEdit.month = 1;
    clockEdit.day = 1;
    clockEdit.hour = 12;
  }
  clockEdit.second = 0;
  menuScreen_ = MenuScreen::SettingsClock;
  settingsSelectedIndex_ = kSettingsClockSyncIndex;
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::selectClockSettingsItem(uint32_t nowMs) {
  BoardConfig::RtcDateTime &clockEdit = clock_.clockEdit();
  switch (settingsSelectedIndex_) {
    case kSettingsBackIndex:
      settingsSelectedIndex_ = kSettingsHomeClockIndex;
      menuScreen_ = MenuScreen::SettingsHome;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    case kSettingsClockSyncIndex: {
      clock_.syncFromNetwork(nowMs);
      BoardConfig::RtcDateTime local;
      int32_t day = 0;
      if (clock_.localNow(local, day)) {
        clockEdit = local;
      }
      menuScreen_ = MenuScreen::SettingsClock;
      settingsSelectedIndex_ = kSettingsClockSyncIndex;
      rebuildSettingsMenuItems();
      renderSettings();
      return;
    }
    case kSettingsClockAutoIndex:
      clock_.setAutoSyncEnabled(!clock_.autoSyncEnabled());
      break;
    case kSettingsClockTimezoneIndex:
      clock_.cycleTimezone();
      break;
    case kSettingsClockYearIndex:
      clockEdit.year = (clockEdit.year >= 2099) ? 2020 : clockEdit.year + 1;
      break;
    case kSettingsClockMonthIndex:
      clockEdit.month = (clockEdit.month >= 12) ? 1 : clockEdit.month + 1;
      break;
    case kSettingsClockDayIndex:
      clockEdit.day = (clockEdit.day >= daysInMonth(clockEdit.year, clockEdit.month))
                          ? 1
                          : clockEdit.day + 1;
      break;
    case kSettingsClockHourIndex:
      clockEdit.hour = (clockEdit.hour >= 23) ? 0 : clockEdit.hour + 1;
      break;
    case kSettingsClockMinuteIndex:
      clockEdit.minute = (clockEdit.minute >= 59) ? 0 : clockEdit.minute + 1;
      break;
    default:
      return;  // status row, etc.
  }

  // For the manual date/time rows, keep the day valid and write the RTC live.
  if (settingsSelectedIndex_ >= kSettingsClockYearIndex &&
      settingsSelectedIndex_ <= kSettingsClockMinuteIndex) {
    const uint8_t maxDay = daysInMonth(clockEdit.year, clockEdit.month);
    if (clockEdit.day > maxDay) {
      clockEdit.day = maxDay;
    }
    clock_.writeLocalToRtc(clockEdit);
    updateStreakForToday();
  }
  rebuildSettingsMenuItems();
  renderSettings();
}

void App::renderChapterPicker() {
  display_.renderMenu(chapterMenuItems_, chapterPickerSelectedIndex_);
}

void App::renderRestartConfirm() {
  std::vector<String> items;
  items.reserve(RestartConfirmItemCount);
  items.push_back(uiText(UiText::AreYouSure));
  items.push_back(uiText(UiText::NoKeepPlace));
  items.push_back(uiText(UiText::YesRestart));

  display_.renderMenu(items, restartConfirmSelectedIndex_ + kRestartConfirmHeaderRows);
}

void App::renderSdCardRepairConfirm() {
  std::vector<String> items;
  items.reserve(SdCardRepairConfirmItemCount + kSdCardRepairConfirmHeaderRows);
  items.push_back("Repair folders?");
  items.push_back("Not now");
  items.push_back("Create folders");

  display_.renderMenu(items, sdCardRepairConfirmSelectedIndex_ + kSdCardRepairConfirmHeaderRows);
}

void App::renderUpdateConfirm() {
  std::vector<String> items;
  items.reserve(UpdateConfirmItemCount + kUpdateConfirmHeaderRows);
  items.push_back("Update available");
  items.push_back(ota_.pendingCurrentVersion() + " -> " + ota_.pendingNewVersion());
  items.push_back("Skip for now");
  items.push_back("Update");

  display_.renderMenu(items, updateConfirmSelectedIndex_ + kUpdateConfirmHeaderRows);
}

void App::renderFocusTimerPresets() {
  applyReaderUiOrientation();
  if (focusTimerPresetMenuItems_.empty()) {
    rebuildFocusTimerPresetMenuItems();
  }
  display_.renderMenu(focusTimerPresetMenuItems_, focusTimerPresetSelectedIndex_);
}

void App::renderFocusTimerSession() {
  applyReaderUiOrientation();
  const uint32_t now = millis();
  const String remainingLabel = formatFocusTimerRemaining(now);

  String roundLabel = "Round " + String(focusTimer_.currentRound()) + "/" +
                      String(focusTimer_.totalRounds());

  switch (focusTimer_.state()) {
    case FocusTimer::State::PresetSelect:
      renderFocusTimerPresets();
      return;

    case FocusTimer::State::Setup: {
      const FocusTimer::Config c = focusTimer_.config();
      const FocusTimer::Field sel = focusTimer_.selectedField();
      auto row = [&](FocusTimer::Field f, const char *name, const String &value) {
        const char marker = (f == sel) ? '>' : ' ';
        return String(marker) + " " + name + "  " + value + "\n";
      };
      String body;
      body += row(FocusTimer::Field::Work, "Work", String(c.workMin) + "m");
      body += row(FocusTimer::Field::Break, "Break", String(c.breakMin) + "m");
      body += row(FocusTimer::Field::Rounds, "Rounds", String(c.rounds));
      body += row(FocusTimer::Field::LongBreak, "Long break", String(c.longBreakMin) + "m");
      body += (sel == FocusTimer::Field::Begin ? "> Begin" : "  Begin");
      String mode = FocusTimer::presetLabel(focusTimer_.preset());
      mode.toUpperCase();
      display_.renderFocusTimerScreen(mode, "Setup", "", body,
                                      "Swipe up/down - row\nSwipe left/right - value\nTap Begin - Hold back");
      return;
    }

    case FocusTimer::State::WorkRunning:
      display_.renderFocusTimerScreen("WORK", roundLabel, remainingLabel,
                                      "Tap pause - swipe skip\nHold cancel", "",
                                      focusTimer_.progressPercent(now));
      return;

    case FocusTimer::State::WorkPaused:
      display_.renderFocusTimerScreen("PAUSED", roundLabel, remainingLabel,
                                      "Tap resume / hold cancel\nLong edge: auto-pause only", "",
                                      focusTimer_.progressPercent(now));
      return;

    case FocusTimer::State::BreakRunning:
      display_.renderFocusTimerScreen(focusTimer_.isLongBreak() ? "LONG BREAK" : "BREAK",
                                      roundLabel, remainingLabel,
                                      "Tap pause - swipe skip\nHold cancel", "",
                                      focusTimer_.progressPercent(now), true);
      return;

    case FocusTimer::State::BreakPaused:
      display_.renderFocusTimerScreen("PAUSED", roundLabel, remainingLabel,
                                      "Tap resume / hold cancel\nLong edge: auto-pause only", "",
                                      focusTimer_.progressPercent(now), true);
      return;

    case FocusTimer::State::WaitWorkStart:
      display_.renderFocusTimerScreen("WORK", "Next: round " + String(focusTimer_.currentRound() + 1),
                                      "", "Tap start - stand on side",
                                      "Hold cancel");
      return;

    case FocusTimer::State::Complete:
      display_.renderFocusTimerScreen("DONE", "", "", "Session complete\nTap to choose another timer");
      return;

    case FocusTimer::State::Cancelled:
      display_.renderFocusTimerScreen("DONE", "", "", "Cancelled");
      return;
  }
}

bool App::updateChapterTransition(uint32_t nowMs) {
  if (!chapterTransitionVisible_) {
    return false;
  }

  if (nowMs < chapterTransitionUntilMs_) {
    return true;
  }

  chapterTransitionVisible_ = false;
  reader_.start(nowMs);
  renderActiveReader(nowMs);
  return true;
}

bool App::maybeStartChapterTransition(size_t previousWordIndex, size_t currentWordIndex,
                                      uint32_t nowMs) {
  if (chapterMarkers_.empty() || currentWordIndex <= previousWordIndex) {
    return false;
  }

  for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
    const size_t chapterWordIndex = chapterMarkers_[i].wordIndex;
    if (chapterWordIndex == 0 || chapterWordIndex <= previousWordIndex ||
        chapterWordIndex > currentWordIndex) {
      continue;
    }

    chapterTransitionIndex_ = i;
    chapterTransitionVisible_ = true;
    chapterTransitionUntilMs_ = nowMs + kChapterTransitionMs;
    contextViewVisible_ = false;
    wpmFeedbackVisible_ = false;
    reader_.seekTo(chapterWordIndex);
    renderChapterTransition();
    Serial.printf("[chapter] transition %u/%u word=%u title=%s\n",
                  static_cast<unsigned int>(i + 1),
                  static_cast<unsigned int>(chapterMarkers_.size()),
                  static_cast<unsigned int>(chapterWordIndex),
                  chapterMarkers_[i].title.c_str());
    return true;
  }

  return false;
}

void App::renderChapterTransition() {
  if (!chapterTransitionVisible_ || chapterTransitionIndex_ >= chapterMarkers_.size()) {
    return;
  }

  applyReaderUiOrientation();
  const String title = String("CHAPTER ") + String(chapterTransitionIndex_ + 1);
  String subtitle = chapterMarkers_[chapterTransitionIndex_].title;
  if (subtitle.length() > 42) {
    subtitle = subtitle.substring(0, 42) + "...";
  }
  display_.renderStatus(title, subtitle, "");
}

DisplayManager::LibraryItem App::libraryItemForBook(size_t bookIndex) {
  DisplayManager::LibraryItem item;
  item.title = storage_.bookDisplayName(bookIndex);
  item.subtitle = storage_.bookAuthorName(bookIndex);

  uint8_t percent = 0;
  const bool hasProgress = bookProgressPercent(bookIndex, percent);
  if (library_.isFinished(storage_.bookPath(bookIndex))) {
    if (!item.subtitle.isEmpty()) {
      item.subtitle += " - ";
    }
    item.subtitle += "Read";
  } else if (hasProgress) {
    if (!item.subtitle.isEmpty()) {
      item.subtitle += " - ";
    }
    item.subtitle += String(percent) + "%";
  }

  if (item.subtitle.isEmpty() && usingStorageBook_ && bookIndex == currentBookIndex_) {
    item.subtitle = uiText(UiText::CurrentBook);
  }

  return item;
}

String App::chapterMenuLabel(size_t chapterIndex) const {
  if (chapterIndex >= chapterMarkers_.size()) {
    return "";
  }

  String label = String(chapterIndex + 1) + " " + chapterMarkers_[chapterIndex].title;
  if (label.length() > 36) {
    label = label.substring(0, 36) + "...";
  }

  const size_t currentIndex = reader_.currentIndex();
  const size_t startIndex = chapterMarkers_[chapterIndex].wordIndex;
  const size_t endIndex = (chapterIndex + 1 < chapterMarkers_.size())
                              ? chapterMarkers_[chapterIndex + 1].wordIndex
                              : reader_.wordCount();
  if (currentIndex >= startIndex && currentIndex < endIndex) {
    label += " *";
  }
  return label;
}

size_t App::currentChapterIndex() const {
  if (chapterMarkers_.empty()) {
    return static_cast<size_t>(-1);
  }

  size_t currentChapter = 0;
  const size_t currentIndex = reader_.currentIndex();
  for (size_t i = 0; i < chapterMarkers_.size(); ++i) {
    if (chapterMarkers_[i].wordIndex <= currentIndex) {
      currentChapter = i;
    }
  }

  return currentChapter;
}

String App::currentChapterLabel() const {
  const size_t chapterIndex = currentChapterIndex();
  const String fallback = currentBookTitle_.isEmpty() ? uiText(UiText::Start) : currentBookTitle_;
  if (chapterIndex >= chapterMarkers_.size()) {
    return fallback;
  }
  return cleanedChapterTitle(chapterMarkers_[chapterIndex].title, fallback);
}

String App::cleanedChapterTitle(const String &raw, const String &fallback) const {
  if (raw.isEmpty()) return fallback;
  // Strip leading "N." prefix (e.g. "2.EbookTitle" -> "EbookTitle")
  size_t i = 0;
  while (i < (size_t)raw.length() && isDigit(raw[i])) i++;
  if (i > 0 && i < (size_t)raw.length() && raw[i] == '.') {
    String cleaned = raw.substring(i + 1);
    cleaned.trim();
    return cleaned.isEmpty() ? fallback : cleaned;
  }
  return raw;
}

const char *App::chapterLabelPrefKey() const {
  return (readerMode_ == ReaderMode::Scroll) ? kPrefChapterLabelScroll : kPrefChapterLabelRsvp;
}

bool App::chapterLabelDefaultForMode(ReaderMode mode) {
  return mode != ReaderMode::Scroll;
}

String App::currentFooterMetricLabel() const {
  if (footerMetricMode_ == FooterMetricMode::Percentage) {
    return String(readingProgressPercent()) + "%";
  }

  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    return "0%";
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  size_t endIndex = wordCount;
  const bool generatingEstimate = timeEstimate_.accurateEstimate() &&
                                  timeEstimate_.buildInProgress() &&
                                  timeEstimate_.buildMatchesCurrentBook(currentBookPath_);
  const int generatingPercent = generatingEstimate ? timeEstimate_.buildProgressPercent() : 0;

  if (footerMetricMode_ == FooterMetricMode::ChapterTime) {
    const size_t chapterIndex = currentChapterIndex();
    if (chapterIndex < chapterMarkers_.size() && chapterIndex + 1 < chapterMarkers_.size()) {
      endIndex = chapterMarkers_[chapterIndex + 1].wordIndex;
    }
    if (generatingEstimate) {
      return String("CH ") + String(generatingPercent) + "% gen";
    }
    return String("CH ") +
           TimeEstimateEngine::formatReadingTimeRemaining(
               timeEstimate_.estimatedReadingTimeRemainingMs(currentIndex, endIndex));
  }

  if (generatingEstimate) {
    return String("BOOK ") + String(generatingPercent) + "% gen";
  }
  return String("BOOK ") +
         TimeEstimateEngine::formatReadingTimeRemaining(
             timeEstimate_.estimatedReadingTimeRemainingMs(currentIndex, endIndex));
}

String App::currentBatteryLabel() const {
  if (!batteryPresent_ || !batterySampleInitialized_) {
    return "";
  }

  if (batteryLabelMode_ == BatteryLabelMode::TimeRemaining) {
    return batteryTimeRemainingLabel();
  }

  if (batteryLabelMode_ == BatteryLabelMode::Voltage) {
    return batteryVoltageLabel();
  }

  return String(static_cast<unsigned int>(batteryDisplayedPercent_)) + "%";
}

String App::footerMetricModeLabel() const {
  switch (footerMetricMode_) {
    case FooterMetricMode::ChapterTime:
      return "Chapter time";
    case FooterMetricMode::BookTime:
      return "Book time";
    case FooterMetricMode::Percentage:
    default:
      return "Percent read";
  }
}

String App::batteryLabelModeLabel() const {
  switch (batteryLabelMode_) {
    case BatteryLabelMode::TimeRemaining:
      return "Time remaining";
    case BatteryLabelMode::Voltage:
      return "Voltage";
    case BatteryLabelMode::Percent:
    default:
      return "Percentage";
  }
}

String App::batteryTimeRemainingLabel() const {
  if (batteryRuntimeEstimateReady_) {
    return formatBatteryTimeRemaining(batteryRuntimeMinutesRemaining_);
  }

  const uint32_t nominal = nominalBatteryRuntimeMinutes();
  const uint32_t estimatedMinutes =
      (static_cast<uint32_t>(batteryDisplayedPercent_) * nominal) / 100UL;
  return formatBatteryTimeRemaining(estimatedMinutes);
}

String App::batteryVoltageLabel() const { return String(batteryFilteredVoltage_, 2) + "V"; }

uint32_t App::nominalBatteryRuntimeMinutes() const {
  // Weighted estimate: play CPU dominates (~60%), others contribute ~10% each.
  // Base at 160 MHz play = 450 min.
  auto mhzFactor = [](uint32_t mhz) -> int32_t {
    if (mhz <= 80) return 90;    // extra minutes saved vs 160
    if (mhz >= 240) return -60;  // extra minutes lost vs 160
    return 0;
  };
  int32_t base = static_cast<int32_t>(kNominalBatteryRuntimeMinutes);  // 450 min at 160 MHz
  // Weight only the CPUs that are actually applied in the current mode by applyStateCpuFrequency().
  // Scroll mode: both Playing and Paused use cpuMhzScroll_ — cpuMhzPlay_/cpuMhzPaused_ unused.
  // RSVP mode:   Playing→cpuMhzPlay_, Paused→cpuMhzPaused_ — cpuMhzScroll_ unused.
  // Menu and Standby apply in both modes.
  if (scrollModeEnabled()) {
    base += static_cast<int32_t>(mhzFactor(cpuMhzScroll_));
  } else {
    base += static_cast<int32_t>(mhzFactor(cpuMhzPlay_));
    base += static_cast<int32_t>(mhzFactor(cpuMhzPaused_)) / 4;
  }
  base += static_cast<int32_t>(mhzFactor(cpuMhzMenu_)) / 4;
  base += static_cast<int32_t>(mhzFactor(cpuMhzStandby_)) / 4;
  if (!cachedOtaAutoCheck_) {
    base += 20;
  }
  return static_cast<uint32_t>(base < 60 ? 60 : base);
}

String App::cpuMhzLabel(uint32_t mhz) {
  return String(mhz) + " MHz";
}

String App::autoDimDelayLabel() const {
  if (autoDimDelayMs_ == 0) {
    return "Off";
  }
  if (autoDimDelayMs_ <= 30000) {
    return "30s";
  }
  if (autoDimDelayMs_ <= 60000) {
    return "60s";
  }
  return "2min";
}

String App::autoDimBrightnessLabel() const {
  if (autoDimBrightnessPercent_ == 0) return "Screen off";
  return String(autoDimBrightnessPercent_) + "%";
}

String App::formatBatteryTimeRemaining(uint32_t minutes) const {
  if (minutes < 1) {
    return "0m";
  }

  if (minutes < 60) {
    return String(minutes) + "m";
  }

  const uint32_t hours = minutes / 60;
  const uint32_t remainder = minutes % 60;
  if (hours >= 10 || remainder < 10) {
    return String(hours) + "h";
  }

  return String(hours) + "h" + String(remainder / 10) + "0";
}

String App::timeEstimateModeLabel() const {
  return uiText(timeEstimate_.accurateEstimate() ? UiText::TimeEstimateAccurate
                                                 : UiText::TimeEstimateFast);
}

uint8_t App::readingProgressPercent() const {
  const size_t count = reader_.wordCount();
  if (count <= 1) {
    return 0;
  }

  const size_t index = std::min(reader_.currentIndex(), count - 1);
  const size_t percent = (index * 100UL) / (count - 1);
  return static_cast<uint8_t>(std::min(static_cast<size_t>(100), percent));
}

bool App::isFocusTimerMenuScreen(MenuScreen screen) const {
  return screen == MenuScreen::FocusTimerPresets || screen == MenuScreen::FocusTimerSession;
}

void App::applyUiOrientation(BoardConfig::UiOrientation orientation) {
  touch_.setUiOrientation(orientation);
  display_.setUiOrientation(orientation);
}

void App::applyReaderUiOrientation() {
  applyUiOrientation(readerUiOrientation());
}

BoardConfig::UiOrientation App::readerUiOrientation() const {
  return uiRotated180() ? BoardConfig::UiOrientation::LandscapeFlipped
                        : BoardConfig::UiOrientation::Landscape;
}

String App::orientationLockLabel() const { return onOffLabel(orientationLockEnabled_); }

void App::cycleOrientationLock(uint32_t nowMs) {
  orientationLockEnabled_ = !orientationLockEnabled_;
  preferences_.putBool(kPrefOrientLock, orientationLockEnabled_);
  // Locking freezes at the handedness baseline: drop any active auto flip.
  if (orientationLockEnabled_ && autoFlip180_) {
    autoFlip180_ = false;
    applyReaderUiOrientation();
  }
  autoFlipCandidate_ = autoFlip180_;
  Serial.printf("[orient] lock=%u flip=%u\n", orientationLockEnabled_ ? 1U : 0U,
                autoFlip180_ ? 1U : 0U);
  (void)nowMs;
}

void App::updateAutoOrientation(uint32_t nowMs) {
  if (orientationLockEnabled_ || !motion_.available()) {
    return;
  }
  // Only evaluate in interactive, drawable states. Freeze while RSVP words are
  // actively flashing (Playing) so a flip never interrupts a reading run.
  if (state_ != AppState::Menu && state_ != AppState::Paused &&
      state_ != AppState::Finished) {
    return;
  }

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!motion_.readAccel(x, y, z)) {
    return;
  }
  const float axes[3] = {x, y, z};
  const uint8_t idx = BoardConfig::IMU_FLIP_AXIS < 3 ? BoardConfig::IMU_FLIP_AXIS : 1;
  const float c = axes[idx] * BoardConfig::IMU_FLIP_UPRIGHT_SIGN;

  // Throttled calibration log (read via console while flipping the device).
  if (nowMs - lastOrientLogMs_ >= 1000) {
    lastOrientLogMs_ = nowMs;
    Serial.printf("[orient] x=%.2f y=%.2f z=%.2f c=%.2f flip=%u lock=%u\n", x, y, z, c,
                  autoFlip180_ ? 1U : 0U, orientationLockEnabled_ ? 1U : 0U);
  }

  bool desired;
  if (c <= -BoardConfig::IMU_FLIP_THRESHOLD) {
    desired = true;  // upside-down
  } else if (c >= BoardConfig::IMU_FLIP_THRESHOLD) {
    desired = false;  // upright
  } else {
    return;  // inside the hysteresis band: hold the current orientation
  }

  if (desired == autoFlip180_) {
    autoFlipCandidate_ = desired;
    return;
  }
  if (desired != autoFlipCandidate_) {
    autoFlipCandidate_ = desired;
    autoFlipCandidateSinceMs_ = nowMs;
    return;
  }
  constexpr uint32_t kAutoFlipStableMs = 700;
  if (nowMs - autoFlipCandidateSinceMs_ < kAutoFlipStableMs) {
    return;
  }

  autoFlip180_ = desired;
  applyReaderUiOrientation();
  refreshCurrentScreen(nowMs);
  Serial.printf("[orient] applied flip=%u\n", autoFlip180_ ? 1U : 0U);
}

void App::refreshCurrentScreen(uint32_t nowMs) {
  switch (state_) {
    case AppState::Menu:
      renderMenu();
      break;
    case AppState::Paused:
      renderActiveReader(nowMs);
      break;
    case AppState::Finished:
      renderBookFinished();
      break;
    default:
      break;
  }
}

String App::formatFocusTimerDuration(uint32_t durationMs) const {
  const uint32_t totalSeconds = durationMs / 1000UL;
  const uint32_t minutes = totalSeconds / 60UL;
  const uint32_t seconds = totalSeconds % 60UL;
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
                static_cast<unsigned long>(minutes),
                static_cast<unsigned long>(seconds));
  return String(buffer);
}

String App::formatFocusTimerRemaining(uint32_t nowMs) const {
  const uint32_t remainingMs = focusTimer_.remainingMs(nowMs);
  const uint32_t totalSeconds = remainingMs / 1000UL;
  const uint32_t minutes = totalSeconds / 60UL;
  const uint32_t seconds = totalSeconds % 60UL;
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
                static_cast<unsigned long>(minutes),
                static_cast<unsigned long>(seconds));
  return String(buffer);
}

String App::focusTimerChimeLabel() const {
  switch (focusTimerChime_) {
    case FocusTimerChime::Off:
      return "Off";
    case FocusTimerChime::SoftBell:
      return "Soft bell";
    case FocusTimerChime::SoftDouble:
      return "Soft double";
    case FocusTimerChime::SharpBell:
      return "Sharp bell";
    case FocusTimerChime::HardHorn:
      return "Hard horn";
    case FocusTimerChime::Melody:
      return "Little tune";
    case FocusTimerChime::Startup:
      return "8-bit startup";
    default:
      return "Soft bell";
  }
}

void App::cycleFocusTimerChime(uint32_t nowMs) {
  uint8_t next = static_cast<uint8_t>(focusTimerChime_) + 1U;
  if (next > static_cast<uint8_t>(FocusTimerChime::Startup)) {
    next = static_cast<uint8_t>(FocusTimerChime::Off);
  }
  focusTimerChime_ = static_cast<FocusTimerChime>(next);
  preferences_.putUChar(kPrefTimerChimeStyle, next);
  preferences_.putBool(kPrefTimerChime, focusTimerChime_ != FocusTimerChime::Off);
  Serial.printf("[timer] chime=%s\n", focusTimerChimeLabel().c_str());
  if (menuScreen_ == MenuScreen::FocusTimerPresets) {
    rebuildFocusTimerPresetMenuItems();
    renderFocusTimerPresets();
  } else if (menuScreen_ == MenuScreen::SettingsSound) {
    rebuildSettingsMenuItems();
    renderSettings();
  }
  if (focusTimerChime_ != FocusTimerChime::Off) {
    playFocusTimerCue(FocusTimer::Cue::WorkComplete);
  }
  lastFocusTimerActionMs_ = nowMs;
}

void App::playFocusTimerCue(FocusTimer::Cue cue) {
  if (cue == FocusTimer::Cue::None || focusTimerChime_ == FocusTimerChime::Off) {
    return;
  }
  if (!audio_.available()) {
    audio_.begin();
  }

  constexpr int16_t kSoft = 9000;
  constexpr int16_t kTick = 7000;
  constexpr int16_t kSharp = 12000;
  constexpr int16_t kHard = 15000;

  bool played = false;
  int flashCount = 1;
  switch (cue) {
    case FocusTimer::Cue::Start:
      played = audio_.tone(1040, 45, kTick);
      flashCount = 1;
      break;
    case FocusTimer::Cue::Pause:
      played = audio_.tone(660, 45, kTick);
      flashCount = 1;
      break;
    case FocusTimer::Cue::Resume:
      played = audio_.tone(990, 45, kTick);
      flashCount = 1;
      break;
    case FocusTimer::Cue::WorkComplete:
    case FocusTimer::Cue::BreakComplete:
    case FocusTimer::Cue::SessionComplete: {
      const int resolve = cue == FocusTimer::Cue::BreakComplete ? -1 :
                          cue == FocusTimer::Cue::SessionComplete ? 1 : 0;
      switch (focusTimerChime_) {
        case FocusTimerChime::SoftBell:
          played = audio_.tone(resolve < 0 ? 880 : 1320, 130, kSoft);
          break;
        case FocusTimerChime::SoftDouble:
          played = audio_.tone(resolve < 0 ? 880 : 1175, 85, kSoft) &&
                   (delay(45), audio_.tone(resolve < 0 ? 740 : 1480, 115, kSoft));
          break;
        case FocusTimerChime::SharpBell:
          played = audio_.tone(resolve < 0 ? 988 : 1568, 70, kSharp) &&
                   (delay(35), audio_.tone(resolve < 0 ? 784 : 2093, 85, kSharp));
          break;
        case FocusTimerChime::HardHorn:
          played = audio_.tone(resolve < 0 ? 330 : 392, 135, kHard) &&
                   (delay(55), audio_.tone(resolve < 0 ? 294 : 523, 145, kHard));
          break;
        case FocusTimerChime::Melody:
          played = audio_.tone(523, 80, kSoft) && (delay(35), audio_.tone(659, 80, kSoft)) &&
                   (delay(35), audio_.tone(784, 90, kSoft)) &&
                   (delay(40), audio_.tone(resolve < 0 ? 587 : 1047, 130, kSoft));
          break;
        case FocusTimerChime::Startup:
          played = audio_.tone(resolve < 0 ? 392 : 659, 55, kSharp) &&
                   (delay(22), audio_.tone(resolve < 0 ? 330 : 784, 55, kSharp)) &&
                   (delay(22), audio_.tone(resolve < 0 ? 294 : 988, 65, kSharp)) &&
                   (delay(35), audio_.tone(resolve < 0 ? 262 : 1319, 105, kSharp));
          break;
        case FocusTimerChime::Off:
        default:
          return;
      }
      flashCount = cue == FocusTimer::Cue::SessionComplete ? 3 : 2;
      break;
    }
    case FocusTimer::Cue::Cancelled:
      played = audio_.tone(440, 110, kTick);
      flashCount = 2;
      break;
    case FocusTimer::Cue::None:
    default:
      return;
  }

  if (played) {
    return;
  }

  // Audio path unavailable: fall back to a brief backlight flash where a real backlight exists.
#if defined(BOARD_AMOLED_18)
  Serial.println("[timer] chime unavailable");
#else
  for (int i = 0; i < flashCount; ++i) {
    digitalWrite(BoardConfig::PIN_LCD_BACKLIGHT, HIGH);
    delay(55);
    digitalWrite(BoardConfig::PIN_LCD_BACKLIGHT, LOW);
    delay(45);
  }
#endif
}

bool App::scrollModeEnabled() const { return readerMode_ == ReaderMode::Scroll; }

bool App::uiRotated180() const {
  const bool base = handednessMode_ == HandednessMode::Right ? BoardConfig::UI_ROTATED_180
                                                            : !BoardConfig::UI_ROTATED_180;
  // App-wide auto-rotate layers a 180 flip on top of the handedness base.
  return base != autoFlip180_;
}

uint8_t App::effectiveAnchorPercent() const {
  return handednessMode_ == HandednessMode::Left
             ? static_cast<uint8_t>(typographyConfig_.anchorPercent + kLeftHandAnchorOffset)
             : typographyConfig_.anchorPercent;
}

DisplayManager::TypographyConfig App::effectiveTypographyConfig() const {
  DisplayManager::TypographyConfig config = typographyConfig_;
  config.anchorPercent = effectiveAnchorPercent();
  return config;
}

uint32_t App::currentReaderContentToken() const {
  return BookLibraryStore::hashBookPath(currentBookPath_.isEmpty() ? String("__demo__")
                                                                       : currentBookPath_);
}

size_t App::phantomBeforeCharTarget() const {
  uint8_t levelIndex = readerFontSizeIndex_;
  if (levelIndex >= kReaderFontSizeCount) {
    levelIndex = 0;
  }
  return kPhantomBeforeCharTargets[levelIndex];
}

size_t App::phantomAfterCharTarget() const {
  uint8_t levelIndex = readerFontSizeIndex_;
  if (levelIndex >= kReaderFontSizeCount) {
    levelIndex = 0;
  }
  return kPhantomAfterCharTargets[levelIndex];
}

String App::collectPhantomBeforeText(size_t currentIndex, size_t charTarget) const {
  if (currentIndex == 0 || charTarget == 0) {
    return "";
  }

  size_t startIndex = currentIndex;
  size_t totalChars = 0;
  while (startIndex > 0 && totalChars < charTarget) {
    --startIndex;
    const String word = reader_.wordAt(startIndex);
    totalChars += word.length();
    if (startIndex + 1 < currentIndex) {
      ++totalChars;
    }
  }

  String text;
  for (size_t index = startIndex; index < currentIndex; ++index) {
    if (!text.isEmpty()) {
      text += ' ';
    }
    text += reader_.wordAt(index);
  }
  return text;
}

String App::collectPhantomAfterText(size_t currentIndex, size_t charTarget) const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0 || currentIndex + 1 >= wordCount || charTarget == 0) {
    return "";
  }

  size_t endIndex = currentIndex + 1;
  size_t totalChars = 0;
  while (endIndex < wordCount && totalChars < charTarget) {
    const String word = reader_.wordAt(endIndex);
    totalChars += word.length();
    if (endIndex > currentIndex + 1) {
      ++totalChars;
    }
    ++endIndex;
  }

  String text;
  for (size_t index = currentIndex + 1; index < endIndex; ++index) {
    if (!text.isEmpty()) {
      text += ' ';
    }
    text += reader_.wordAt(index);
  }
  return text;
}

String App::phantomBeforeText() const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    return "";
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  return collectPhantomBeforeText(currentIndex, phantomBeforeCharTarget());
}

String App::phantomAfterText() const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    return "";
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  return collectPhantomAfterText(currentIndex, phantomAfterCharTarget());
}

void App::renderActiveReader(uint32_t nowMs) {
  if (pendingBootBookLoad_) {
    display_.renderStatus("Loading book", currentBookTitle_, "Please wait");
    return;
  }

  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }

  if (chapterTransitionVisible_) {
    renderChapterTransition();
    return;
  }

  applyReaderUiOrientation();
  if (scrollModeEnabled()) {
    if (wpmFeedbackVisible_) {
      renderScrollReader(nowMs, String(reader_.wpm()) + " WPM");
    } else {
      renderScrollReader(nowMs);
    }
    return;
  }

  if (contextViewVisible_) {
    renderContextPreview();
  } else if (wpmFeedbackVisible_) {
    renderWpmFeedback(nowMs);
  } else {
    renderReaderWord();
  }
}

bool App::ensureCurrentBookWordAvailable(uint32_t nowMs) {
  if (!usingStorageBook_ || reader_.wordCount() == 0 || !reader_.currentWord().isEmpty()) {
    return true;
  }

  handleCurrentBookReadFailure(nowMs, "Word cache unreadable");
  return false;
}

void App::handleCurrentBookReadFailure(uint32_t nowMs, const char *detail) {
  const String failedTitle = currentBookTitle_.isEmpty() ? String("Current book") : currentBookTitle_;
  const String failedPath = currentBookPath_;
  const bool articlesOnly =
      currentBookIndex_ < storage_.bookCount() && storage_.bookIsArticle(currentBookIndex_);

  Serial.printf("[app] active book read failed word=%u book=%s detail=%s\n",
                static_cast<unsigned int>(reader_.currentIndex()), failedPath.c_str(),
                detail == nullptr ? "" : detail);

  saveReadingPosition(true);
  activeBookStore_.close();
  reader_.clearLoadedBook(nowMs);
  chapterMarkers_.clear();
  paragraphStarts_.clear();
  currentBookPath_ = "";
  currentBookTitle_ = "Demo";
  usingStorageBook_ = false;
  contextViewVisible_ = false;
  wpmFeedbackVisible_ = false;
  invalidateContextPreviewWindow();
  timeEstimate_.invalidate();

  setState(AppState::Menu, nowMs);
  display_.renderStatus("Book read failed", failedTitle,
                        detail == nullptr ? "Reopen from library" : detail);
  delay(1800);
  openBookPicker(articlesOnly);
}

void App::renderReaderWord() {
  applyReaderUiOrientation();
  contextViewVisible_ = false;
  const String beforeText = phantomWordsEnabled_ ? phantomBeforeText() : "";
  const String afterText = phantomWordsEnabled_ ? phantomAfterText() : "";
  const DisplayManager::ReaderChrome chrome = readerChrome();
  const bool showReaderFooter = readerFooterVisible();
  const String footerMetricLabel = readerFooterStatusLabel();
  display_.renderPhantomRsvpWord(beforeText, reader_.currentWord(), afterText,
                                 readerFontSizeIndex_, currentChapterLabel(),
                                 readingProgressPercent(), showReaderFooter, footerMetricLabel,
                                 chrome);
}

bool App::isParagraphStart(size_t wordIndex) const {
  if (wordIndex == 0) {
    return true;
  }

  return std::binary_search(paragraphStarts_.begin(), paragraphStarts_.end(), wordIndex);
}

size_t App::paragraphStartAtOrBefore(size_t wordIndex) const {
  if (wordIndex == 0 || paragraphStarts_.empty()) {
    return 0;
  }

  const auto it = std::upper_bound(paragraphStarts_.begin(), paragraphStarts_.end(), wordIndex);
  if (it == paragraphStarts_.begin()) {
    return 0;
  }

  return *std::prev(it);
}

size_t App::contextPreviewAnchorIndex(size_t currentIndex) const {
  if (currentIndex <= kContextPreviewAnchorLeadWords) {
    return 0;
  }

  const size_t anchorTarget = currentIndex - kContextPreviewAnchorLeadWords;
  const size_t paragraphStart = paragraphStartAtOrBefore(anchorTarget);
  if (anchorTarget - paragraphStart <= kContextPreviewMaxParagraphSnapWords) {
    return paragraphStart;
  }

  return anchorTarget;
}

void App::updateContextPreviewWindow(size_t currentIndex) {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    contextPreviewWords_.clear();
    contextPreviewWindowValid_ = false;
    contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
    return;
  }

  size_t startIndex = contextPreviewStartIndex_;
  size_t endIndex = 0;
  bool rebuildWindow = !contextPreviewWindowValid_ || contextPreviewWords_.empty();
  if (!rebuildWindow) {
    endIndex = std::min(wordCount, startIndex + kContextPreviewWindowWords);
    rebuildWindow = currentIndex < startIndex || currentIndex >= endIndex ||
                    (currentIndex + 1 >= endIndex && endIndex < wordCount);
  }

  if (rebuildWindow) {
    startIndex = contextPreviewAnchorIndex(currentIndex);
    endIndex = std::min(wordCount, startIndex + kContextPreviewWindowWords);
    contextPreviewStartIndex_ = startIndex;
    contextPreviewWindowValid_ = true;
    contextPreviewWords_.clear();
    contextPreviewWords_.reserve(endIndex - startIndex);
    for (size_t index = startIndex; index < endIndex; ++index) {
      DisplayManager::ContextWord word;
      word.text = reader_.wordAt(index);
      word.paragraphStart = isParagraphStart(index);
      word.current = index == currentIndex;
      contextPreviewWords_.push_back(word);
    }
    contextPreviewCurrentLocalIndex_ =
        currentIndex >= startIndex ? currentIndex - startIndex : static_cast<size_t>(-1);
    return;
  }

  const size_t nextLocalIndex = currentIndex - startIndex;
  if (contextPreviewCurrentLocalIndex_ < contextPreviewWords_.size()) {
    contextPreviewWords_[contextPreviewCurrentLocalIndex_].current = false;
  }
  if (nextLocalIndex < contextPreviewWords_.size()) {
    contextPreviewWords_[nextLocalIndex].current = true;
    contextPreviewCurrentLocalIndex_ = nextLocalIndex;
  } else {
    contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
  }
}

void App::invalidateContextPreviewWindow() {
  contextPreviewWindowValid_ = false;
  contextPreviewWords_.clear();
  contextPreviewCurrentLocalIndex_ = static_cast<size_t>(-1);
}

void App::renderContextPreview() {
  applyReaderUiOrientation();
  applyScrollConfig();
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    renderReaderWord();
    return;
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  updateContextPreviewWindow(currentIndex);

  contextViewVisible_ = true;
  const DisplayManager::ReaderChrome chrome = readerChrome();
  display_.renderScrollView(contextPreviewWords_, currentReaderContentToken(),
                            contextPreviewStartIndex_, currentIndex, 0,
                            currentChapterLabel(), readingProgressPercent(), "",
                            readerFooterStatusLabel(), chrome);
}

void App::renderScrollReader(uint32_t nowMs, const String &overlayText) {
  applyReaderUiOrientation();
  applyScrollConfig();
  contextViewVisible_ = false;
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0) {
    renderReaderWord();
    return;
  }

  const size_t currentIndex = std::min(reader_.currentIndex(), wordCount - 1);
  updateContextPreviewWindow(currentIndex);

  uint16_t scrollProgressPermille = 0;
  if (state_ == AppState::Playing && currentIndex + 1 < wordCount) {
    const uint32_t durationMs = reader_.currentWordDurationMs();
    if (durationMs > 0) {
      const uint32_t elapsedMs = reader_.elapsedInCurrentWordMs(nowMs);
      scrollProgressPermille = static_cast<uint16_t>(
          std::min<uint32_t>(1000UL, (elapsedMs * 1000UL) / durationMs));
    }
  }

  String effectiveOverlay = overlayText;

  const DisplayManager::ReaderChrome chrome = readerChrome();
  display_.renderScrollView(contextPreviewWords_, currentReaderContentToken(),
                            contextPreviewStartIndex_, currentIndex, scrollProgressPermille,
                            currentChapterLabel(), readingProgressPercent(), effectiveOverlay,
                            readerFooterStatusLabel(), chrome);
}

void App::renderWpmFeedback(uint32_t nowMs) {
  if (!ensureCurrentBookWordAvailable(nowMs)) {
    return;
  }

  applyReaderUiOrientation();
  wpmFeedbackVisible_ = true;
  wpmFeedbackUntilMs_ = nowMs + kWpmFeedbackMs;
  if (scrollModeEnabled()) {
    renderScrollReader(nowMs, String(reader_.wpm()) + " WPM");
    return;
  }

  contextViewVisible_ = false;
  const String beforeText = phantomWordsEnabled_ ? phantomBeforeText() : "";
  const String afterText = phantomWordsEnabled_ ? phantomAfterText() : "";
  const DisplayManager::ReaderChrome chrome = readerChrome();
  const String footerMetricLabel = readerFooterStatusLabel();
  display_.renderPhantomRsvpWordWithWpm(beforeText, reader_.currentWord(), afterText,
                                        readerFontSizeIndex_, reader_.wpm(),
                                        currentChapterLabel(), readingProgressPercent(),
                                        readerFooterVisible(), footerMetricLabel, chrome);
}

void App::renderStorageStatus(const char *title, const char *line1, const char *line2,
                              int progressPercent) {
  applyReaderUiOrientation();
  display_.renderProgress(title == nullptr ? "SD" : title, line1 == nullptr ? "" : line1,
                          line2 == nullptr ? "" : line2, progressPercent);
}

void App::handleStorageStatus(void *context, const char *title, const char *line1,
                              const char *line2, int progressPercent) {
  if (context == nullptr) {
    return;
  }

  static_cast<App *>(context)->renderStorageStatus(title, line1, line2, progressPercent);
  delay(0);
}
