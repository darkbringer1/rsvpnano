#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <functional>

#include "board/BoardConfig.h"

class DisplayManager;

// Owns the real-time clock: timezone offset, UTC<->local conversion, NTP sync
// over Wi-Fi, and the in-progress manual clock-edit buffer. The RTC chip stores
// UTC; the timezone offset is applied on read so it can change without
// rewriting the clock.
//
// Cross-cutting concerns are injected so the controller never sees App: the
// OTA-in-progress network guard, the Wi-Fi credentials provider, and the
// "clock was just set" callback (used to start the reading streak).
class ClockController {
 public:
  using NetworkGuard = std::function<bool(const char *label, uint32_t nowMs)>;  // true = blocked
  using WifiConfigProvider = std::function<bool(String &ssid, String &password)>;  // false = none
  using ClockSetCallback = std::function<void()>;

  ClockController(DisplayManager &display, Preferences &preferences, NetworkGuard networkBlocked,
                  WifiConfigProvider wifiConfig, ClockSetCallback onClockSet);

  void loadSettings();  // read the persisted timezone offset

  bool localNow(BoardConfig::RtcDateTime &outLocal, int32_t &outDayNumber) const;
  void writeLocalToRtc(const BoardConfig::RtcDateTime &local);  // local -> UTC -> RTC
  bool syncFromNetwork(uint32_t nowMs);  // NTP -> RTC over Wi-Fi; true if the clock was set
  String timezoneLabel() const;

  // Background ("auto") clock sync. The worker task only does network I/O
  // (Wi-Fi + geo-IP + NTP); the RTC write happens on the main thread in
  // pollSync() because the RTC shares the I2C touch bus and must not race the
  // touch poll. Mirrors OtaController's background-check pattern.
  bool autoSyncEnabled() const;
  void setAutoSyncEnabled(bool enabled);  // persists
  bool shouldAutoSync() const;            // auto on + Wi-Fi set + (RTC invalid || stale)
  bool startBackgroundSync(uint32_t nowMs);  // spawns the worker task; false if not started
  bool pollSync(uint32_t nowMs);  // main thread: apply a queued result -> RTC; true if one was consumed
  bool syncInProgress() const { return syncInProgress_; }

  int timezoneOffsetMinutes() const { return timezoneOffsetMinutes_; }
  void cycleTimezone();  // +1h, wrapping -12h..+14h; persists

  BoardConfig::RtcDateTime &clockEdit() { return clockEdit_; }
  const BoardConfig::RtcDateTime &clockEdit() const { return clockEdit_; }

 private:
  static bool fetchTimezoneOffsetMinutes(int &outMinutes);  // geo-IP lookup
  void persistSyncEpoch();  // store the current RTC (UTC) epoch as the last-sync time

  // Result handed back from the worker task to the main thread. Holds the UTC
  // wall-clock from NTP (the RTC write happens on the main thread) plus any
  // geo-IP-detected timezone offset.
  struct SyncResult {
    bool ok = false;
    bool tzDetected = false;
    int tzOffsetMinutes = 0;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
  };

  struct SyncTaskParams {
    String ssid;
    String password;
    QueueHandle_t resultQueue = nullptr;
  };

  static void syncTask(void *params);

  DisplayManager &display_;
  Preferences &preferences_;
  NetworkGuard networkBlocked_;
  WifiConfigProvider wifiConfig_;
  ClockSetCallback onClockSet_;
  int timezoneOffsetMinutes_ = 0;  // RTC holds UTC; this offset is applied on read
  BoardConfig::RtcDateTime clockEdit_{};  // in-progress manual clock edit (local time)
  QueueHandle_t syncQueue_ = nullptr;
  bool syncInProgress_ = false;
};
