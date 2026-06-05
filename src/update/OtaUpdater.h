#pragma once

#include <Arduino.h>

class OtaUpdater {
 public:
  using StatusCallback = void (*)(void *context, const char *title, const char *line1,
                                  const char *line2, int progressPercent);

  // OTA source is hard-locked to this fork. The AMOLED port runs different
  // hardware than the upstream bar-board build; pulling a foreign firmware
  // image would re-brick the display. Owner/repo are NOT overridable via
  // ota.conf (those keys are ignored). Only Wi-Fi / auto_check may be customized.
  static constexpr const char *kLockedOwner = "darkbringer1";
  static constexpr const char *kLockedRepo = "rsvpnano";
  static constexpr const char *kLockedAssetName = "rsvp-nano-amoled-ota.bin";

  struct Config {
    String wifiSsid;
    String wifiPassword;
    String githubOwner = kLockedOwner;
    String githubRepo = kLockedRepo;
    String assetName = kLockedAssetName;
    bool autoCheck = false;
  };

  enum class ResultCode : uint8_t {
    Success,
    NoUpdate,
    UpdateAvailable,
    NotConfigured,
    ConnectFailed,
    MetadataFailed,
    AssetMissing,
    InstallFailed,
  };

  struct Result {
    ResultCode code = ResultCode::MetadataFailed;
    String currentVersion;
    String latestVersion;
    String summary;
    String detail;
    bool rebootRequired = false;
  };

  bool loadConfig(Config &config) const;
  bool isConfigured(const Config &config) const;
  String currentVersion() const;
  Result checkOnly(const Config &config, StatusCallback callback = nullptr,
                   void *context = nullptr) const;
  Result checkAndInstall(const Config &config, StatusCallback callback = nullptr,
                         void *context = nullptr) const;

 private:
  struct LatestRelease {
    String tagName;
    String assetUrl;
  };

  bool loadConfigFromPath(const char *path, Config &config) const;
  bool connectWiFi(const Config &config, StatusCallback callback, void *context) const;
  void disconnectWiFi() const;
  bool fetchLatestRelease(const Config &config, LatestRelease &release, String &errorDetail,
                          StatusCallback callback, void *context) const;
  bool resolveDownloadUrl(const String &assetUrl, const String &version, String &resolvedUrl,
                          String &errorDetail, StatusCallback callback, void *context) const;
  Config lockedConfig(Config config) const;
  void reportStatus(StatusCallback callback, void *context, const char *title,
                    const String &line1, const String &line2, int progressPercent) const;
};
