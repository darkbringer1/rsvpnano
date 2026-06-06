#include "app/OtaController.h"

#include <algorithm>
#include <utility>

#include "display/DisplayManager.h"

namespace {

constexpr uint32_t kOtaCheckTaskStackBytes = 10240;

void copyOtaLabel(char *destination, size_t destinationSize, const String &source) {
  if (destination == nullptr || destinationSize == 0) {
    return;
  }

  const size_t copyLength = std::min(destinationSize - 1, source.length());
  for (size_t i = 0; i < copyLength; ++i) {
    destination[i] = source[i];
  }
  destination[copyLength] = '\0';
}

}  // namespace

OtaController::OtaController(DisplayManager &display, RenderMenuFn renderMenu,
                            SaveReadingFn saveReadingPosition,
                            ApplyCpuFrequencyFn applyCpuFrequency, ReturnToMenuFn returnToMenu,
                            ProgressFn progress)
    : display_(display),
      renderMenu_(std::move(renderMenu)),
      saveReadingPosition_(std::move(saveReadingPosition)),
      applyCpuFrequency_(std::move(applyCpuFrequency)),
      returnToMenu_(std::move(returnToMenu)),
      progress_(std::move(progress)) {}

String OtaController::firmwareVersionLabel() const {
#ifdef RSVP_FIRMWARE_VERSION
  return String(RSVP_FIRMWARE_VERSION);
#else
  return "dev";
#endif
}

void OtaController::statusTrampoline(void *context, const char *title, const char *line1,
                                     const char *line2, int progressPercent) {
  if (context == nullptr) {
    return;
  }
  OtaController *self = static_cast<OtaController *>(context);
  if (self->progress_) {
    self->progress_(title, line1, line2, progressPercent);
  }
}

bool OtaController::startBackgroundCheck(const OtaUpdater::Config &config) {
  if (checkInProgress_) {
    Serial.println("[ota] background check already running");
    return false;
  }

  if (checkQueue_ == nullptr) {
    checkQueue_ = xQueueCreate(1, sizeof(CheckResult));
    if (checkQueue_ == nullptr) {
      Serial.println("[ota] could not create result queue");
      return false;
    }
  }
  xQueueReset(checkQueue_);

  CheckTaskParams *params = new CheckTaskParams();
  if (params == nullptr) {
    Serial.println("[ota] could not allocate task params");
    return false;
  }
  params->config = config;
  params->resultQueue = checkQueue_;

  checkInProgress_ = true;
  BaseType_t created = xTaskCreatePinnedToCore(checkTask, "ota_check", kOtaCheckTaskStackBytes,
                                               params, 1, nullptr, 0);
  if (created != pdPASS) {
    Serial.printf("[ota] background task create failed: %ld\n", static_cast<long>(created));
    checkInProgress_ = false;
    delete params;
    return false;
  }

  Serial.println("[ota] background check started");
  return true;
}

void OtaController::checkTask(void *params) {
  CheckTaskParams *taskParams = static_cast<CheckTaskParams *>(params);
  if (taskParams == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  CheckResult queuedResult;

  const OtaUpdater::Result result = OtaUpdater().checkOnly(taskParams->config, nullptr, nullptr);
  queuedResult.code = result.code;
  copyOtaLabel(queuedResult.currentVersion, sizeof(queuedResult.currentVersion),
               result.currentVersion);
  copyOtaLabel(queuedResult.latestVersion, sizeof(queuedResult.latestVersion),
               result.latestVersion);
  copyOtaLabel(queuedResult.summary, sizeof(queuedResult.summary), result.summary);
  copyOtaLabel(queuedResult.detail, sizeof(queuedResult.detail), result.detail);

  if (taskParams->resultQueue != nullptr) {
    xQueueOverwrite(taskParams->resultQueue, &queuedResult);
  }

  delete taskParams;
  vTaskDelete(nullptr);
}

void OtaController::pollResult(uint32_t nowMs) {
  (void)nowMs;
  if (checkQueue_ == nullptr) {
    return;
  }

  CheckResult result;
  while (xQueueReceive(checkQueue_, &result, 0) == pdTRUE) {
    checkInProgress_ = false;
    if (applyCpuFrequency_) {
      applyCpuFrequency_();
    }
    Serial.printf("[ota] background result code=%u current=%s latest=%s summary=%s detail=%s\n",
                  static_cast<unsigned int>(result.code), result.currentVersion,
                  result.latestVersion, result.summary, result.detail);

    if (result.code == OtaUpdater::ResultCode::UpdateAvailable) {
      pendingCurrentVersion_ = String(result.currentVersion);
      pendingNewVersion_ = String(result.latestVersion);
      updatePromptPending_ = true;
    }
  }
}

bool OtaController::blockForCheck(const String &title, uint32_t nowMs) {
  pollResult(nowMs);
  if (!checkInProgress_) {
    return false;
  }

  display_.renderStatus(title, "OTA check running", "Try again soon");
  delay(1200);
  if (renderMenu_) {
    renderMenu_();
  }
  return true;
}

void OtaController::runFirmwareUpdate(const OtaUpdater::Config &config, bool automatic,
                                     uint32_t nowMs) {
  if (blockForCheck("OTA", nowMs)) {
    return;
  }

  if (!automatic) {
    updatePromptPending_ = false;
  }

  if (!otaUpdater_.isConfigured(config)) {
    if (!automatic) {
      display_.renderStatus("OTA", "Wi-Fi not set", "Settings -> Wi-Fi");
      delay(1600);
      if (returnToMenu_) {
        returnToMenu_(nowMs);
      }
    }
    return;
  }

  if (saveReadingPosition_) {
    saveReadingPosition_();
  }
  const OtaUpdater::Result result = otaUpdater_.checkAndInstall(config, &statusTrampoline, this);

  Serial.printf("[ota] code=%u current=%s latest=%s summary=%s detail=%s\n",
                static_cast<unsigned int>(result.code), result.currentVersion.c_str(),
                result.latestVersion.c_str(), result.summary.c_str(), result.detail.c_str());

  if (result.rebootRequired) {
    display_.renderStatus("OTA", "Restarting", result.latestVersion);
    delay(300);
    ESP.restart();
    return;
  }

  if (automatic) {
    return;
  }

  const String line2 = result.detail.isEmpty() ? result.currentVersion : result.detail;
  display_.renderStatus("OTA", result.summary, line2);
  delay(1600);
  if (returnToMenu_) {
    returnToMenu_(nowMs);
  }
}
