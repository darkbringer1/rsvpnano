#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <functional>

#include "update/OtaUpdater.h"

class DisplayManager;

// Owns over-the-air update mechanics: the background metadata-check task and its
// result queue, the foreground download/install, the "update available" prompt
// state, and the network-busy guard other features call before going online.
//
// App keeps the pref-backed config assembly (preferredOtaConfig) and the
// UpdateConfirm menu screen; both call into this controller. App-side effects
// (menu redraw, reading-position save, CPU-frequency restore, post-update
// navigation) are injected as callbacks so the controller never sees App.
class OtaController {
 public:
  using RenderMenuFn = std::function<void()>;
  using SaveReadingFn = std::function<void()>;
  using ApplyCpuFrequencyFn = std::function<void()>;
  using ReturnToMenuFn = std::function<void(uint32_t nowMs)>;
  using ProgressFn =
      std::function<void(const char *title, const char *line1, const char *line2, int percent)>;

  OtaController(DisplayManager &display, RenderMenuFn renderMenu, SaveReadingFn saveReadingPosition,
                ApplyCpuFrequencyFn applyCpuFrequency, ReturnToMenuFn returnToMenu,
                ProgressFn progress);

  // OtaUpdater passthroughs (App assembles the merged Config from shared prefs).
  bool loadConfig(OtaUpdater::Config &config) const { return otaUpdater_.loadConfig(config); }
  bool isConfigured(const OtaUpdater::Config &config) const {
    return otaUpdater_.isConfigured(config);
  }
  String currentVersion() const { return otaUpdater_.currentVersion(); }
  String firmwareVersionLabel() const;

  bool startBackgroundCheck(const OtaUpdater::Config &config);
  void pollResult(uint32_t nowMs);
  bool checkInProgress() const { return checkInProgress_; }
  // Returns true (and shows a busy notice) if a background check is running.
  bool blockForCheck(const String &title, uint32_t nowMs);
  void runFirmwareUpdate(const OtaUpdater::Config &config, bool automatic, uint32_t nowMs);

  // "Update available" prompt, consumed by App's UpdateConfirm screen.
  bool updatePromptPending() const { return updatePromptPending_; }
  void clearUpdatePrompt() { updatePromptPending_ = false; }
  const String &pendingCurrentVersion() const { return pendingCurrentVersion_; }
  const String &pendingNewVersion() const { return pendingNewVersion_; }

 private:
  static constexpr size_t kVersionLabelMax = 32;
  static constexpr size_t kSummaryLabelMax = 40;
  static constexpr size_t kDetailLabelMax = 96;

  struct CheckResult {
    OtaUpdater::ResultCode code = OtaUpdater::ResultCode::MetadataFailed;
    char currentVersion[kVersionLabelMax] = {};
    char latestVersion[kVersionLabelMax] = {};
    char summary[kSummaryLabelMax] = {};
    char detail[kDetailLabelMax] = {};
  };

  struct CheckTaskParams {
    OtaUpdater::Config config;
    QueueHandle_t resultQueue = nullptr;
  };

  static void checkTask(void *params);
  static void statusTrampoline(void *context, const char *title, const char *line1,
                               const char *line2, int progressPercent);

  DisplayManager &display_;
  RenderMenuFn renderMenu_;
  SaveReadingFn saveReadingPosition_;
  ApplyCpuFrequencyFn applyCpuFrequency_;
  ReturnToMenuFn returnToMenu_;
  ProgressFn progress_;

  OtaUpdater otaUpdater_;
  QueueHandle_t checkQueue_ = nullptr;
  bool checkInProgress_ = false;
  bool updatePromptPending_ = false;
  String pendingCurrentVersion_;
  String pendingNewVersion_;
};
