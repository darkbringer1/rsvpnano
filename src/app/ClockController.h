#pragma once

#include <Arduino.h>
#include <Preferences.h>

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

  int timezoneOffsetMinutes() const { return timezoneOffsetMinutes_; }
  void cycleTimezone();  // +1h, wrapping -12h..+14h; persists

  BoardConfig::RtcDateTime &clockEdit() { return clockEdit_; }
  const BoardConfig::RtcDateTime &clockEdit() const { return clockEdit_; }

 private:
  bool fetchTimezoneOffsetMinutes(int &outMinutes);  // geo-IP lookup

  DisplayManager &display_;
  Preferences &preferences_;
  NetworkGuard networkBlocked_;
  WifiConfigProvider wifiConfig_;
  ClockSetCallback onClockSet_;
  int timezoneOffsetMinutes_ = 0;  // RTC holds UTC; this offset is applied on read
  BoardConfig::RtcDateTime clockEdit_{};  // in-progress manual clock edit (local time)
};
