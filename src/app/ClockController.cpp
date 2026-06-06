#include "app/ClockController.h"

#include <WiFi.h>
#include <HTTPClient.h>

#include <cstdio>
#include <ctime>
#include <utility>

#include "display/DisplayManager.h"

namespace {

constexpr const char *kPrefTimezoneOffset = "tz_off";

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). Used to
// convert between UTC and local time.
int32_t civilDayNumber(int year, int month, int day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

// Inverse of civilDayNumber: day-count since 1970-01-01 -> civil date.
void civilFromDayNumber(int32_t z, int &year, int &month, int &day) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = y + (month <= 2);
}

// Floor division (rounds toward negative infinity) for time-of-day west of UTC.
int64_t floorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

}  // namespace

ClockController::ClockController(DisplayManager &display, Preferences &preferences,
                                NetworkGuard networkBlocked, WifiConfigProvider wifiConfig,
                                ClockSetCallback onClockSet)
    : display_(display),
      preferences_(preferences),
      networkBlocked_(std::move(networkBlocked)),
      wifiConfig_(std::move(wifiConfig)),
      onClockSet_(std::move(onClockSet)) {}

void ClockController::loadSettings() {
  timezoneOffsetMinutes_ = preferences_.getInt(kPrefTimezoneOffset, 0);
}

String ClockController::timezoneLabel() const {
  if (timezoneOffsetMinutes_ == 0) {
    return "UTC";
  }
  const int absMin = timezoneOffsetMinutes_ < 0 ? -timezoneOffsetMinutes_ : timezoneOffsetMinutes_;
  String s = String("UTC") + (timezoneOffsetMinutes_ > 0 ? "+" : "-") + String(absMin / 60);
  if (absMin % 60 != 0) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), ":%02d", absMin % 60);
    s += buf;
  }
  return s;
}

void ClockController::cycleTimezone() {
  // Cycle UTC offset by 1h, -12h..+14h wrapping. Applied on read -> instant.
  timezoneOffsetMinutes_ =
      (timezoneOffsetMinutes_ >= 14 * 60) ? -12 * 60 : timezoneOffsetMinutes_ + 60;
  preferences_.putInt(kPrefTimezoneOffset, timezoneOffsetMinutes_);
}

bool ClockController::localNow(BoardConfig::RtcDateTime &outLocal, int32_t &outDayNumber) const {
  // RTC stores UTC; apply the timezone offset here so the offset can change
  // without rewriting the clock.
  BoardConfig::RtcDateTime utc;
  if (!BoardConfig::rtcRead(utc) || !utc.valid) {
    return false;
  }
  int64_t epoch = static_cast<int64_t>(civilDayNumber(utc.year, utc.month, utc.day)) * 86400 +
                  utc.hour * 3600 + utc.minute * 60 + utc.second;
  epoch += static_cast<int64_t>(timezoneOffsetMinutes_) * 60;
  outDayNumber = static_cast<int32_t>(floorDiv(epoch, 86400));
  const int32_t secOfDay = static_cast<int32_t>(epoch - static_cast<int64_t>(outDayNumber) * 86400);
  int y, m, d;
  civilFromDayNumber(outDayNumber, y, m, d);
  outLocal.year = static_cast<uint16_t>(y);
  outLocal.month = static_cast<uint8_t>(m);
  outLocal.day = static_cast<uint8_t>(d);
  outLocal.hour = static_cast<uint8_t>(secOfDay / 3600);
  outLocal.minute = static_cast<uint8_t>((secOfDay % 3600) / 60);
  outLocal.second = static_cast<uint8_t>(secOfDay % 60);
  outLocal.valid = true;
  return true;
}

void ClockController::writeLocalToRtc(const BoardConfig::RtcDateTime &local) {
  int64_t epoch = static_cast<int64_t>(civilDayNumber(local.year, local.month, local.day)) * 86400 +
                  local.hour * 3600 + local.minute * 60 + local.second;
  epoch -= static_cast<int64_t>(timezoneOffsetMinutes_) * 60;  // local -> UTC
  const int32_t day = static_cast<int32_t>(floorDiv(epoch, 86400));
  const int32_t secOfDay = static_cast<int32_t>(epoch - static_cast<int64_t>(day) * 86400);
  int y, m, d;
  civilFromDayNumber(day, y, m, d);
  BoardConfig::RtcDateTime utc;
  utc.year = static_cast<uint16_t>(y);
  utc.month = static_cast<uint8_t>(m);
  utc.day = static_cast<uint8_t>(d);
  utc.hour = static_cast<uint8_t>(secOfDay / 3600);
  utc.minute = static_cast<uint8_t>((secOfDay % 3600) / 60);
  utc.second = static_cast<uint8_t>(secOfDay % 60);
  utc.valid = true;
  BoardConfig::rtcWrite(utc);
}

bool ClockController::fetchTimezoneOffsetMinutes(int &outMinutes) {
  // ip-api.com returns the UTC offset (seconds, includes DST) for the device's
  // public IP. Free, HTTP, no key. Hand-parse the "offset" integer.
  HTTPClient http;
  WiFiClient client;
  if (!http.begin(client, "http://ip-api.com/json/?fields=status,offset")) {
    return false;
  }
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  const int key = body.indexOf("\"offset\"");
  if (key < 0) {
    return false;
  }
  int i = body.indexOf(':', key);
  if (i < 0) {
    return false;
  }
  ++i;
  while (i < static_cast<int>(body.length()) && (body[i] == ' ' || body[i] == '\t')) {
    ++i;
  }
  int sign = 1;
  if (i < static_cast<int>(body.length()) && (body[i] == '-' || body[i] == '+')) {
    if (body[i] == '-') {
      sign = -1;
    }
    ++i;
  }
  long seconds = 0;
  bool sawDigit = false;
  while (i < static_cast<int>(body.length()) && body[i] >= '0' && body[i] <= '9') {
    seconds = seconds * 10 + (body[i] - '0');
    sawDigit = true;
    ++i;
  }
  if (!sawDigit) {
    return false;
  }
  outMinutes = static_cast<int>((sign * seconds) / 60);
  return true;
}

bool ClockController::syncFromNetwork(uint32_t nowMs) {
  if (networkBlocked_ && networkBlocked_("Clock", nowMs)) {
    return false;
  }
  String ssid;
  String password;
  if (!wifiConfig_ || !wifiConfig_(ssid, password) || ssid.isEmpty()) {
    display_.renderStatus("Clock", "No Wi-Fi set", "Configure Wi-Fi first");
    delay(1500);
    return false;
  }

  display_.renderProgress("Clock", "Connecting Wi-Fi", ssid, 10);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 8000) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_OFF);
    display_.renderStatus("Clock", "Wi-Fi failed", "");
    delay(1500);
    return false;
  }

  // Auto-detect the timezone from the network (best effort); falls back to the
  // current/manual offset if the lookup fails.
  display_.renderProgress("Clock", "Detecting timezone", "", 45);
  int detectedMinutes = 0;
  if (fetchTimezoneOffsetMinutes(detectedMinutes)) {
    timezoneOffsetMinutes_ = detectedMinutes;
    preferences_.putInt(kPrefTimezoneOffset, timezoneOffsetMinutes_);
    Serial.printf("[clock] timezone auto-detected: %s\n", timezoneLabel().c_str());
  }

  display_.renderProgress("Clock", "Getting time", "", 60);
  // RTC stores UTC; the offset above is applied on read.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo;
  bool got = false;
  startMs = millis();
  while (millis() - startMs < 8000) {
    if (getLocalTime(&timeInfo, 200)) {
      got = true;
      break;
    }
    delay(100);
  }
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  if (!got || timeInfo.tm_year < 120) {  // tm_year is years since 1900; <2020 = bad
    display_.renderStatus("Clock", "Time sync failed", "");
    delay(1500);
    return false;
  }

  BoardConfig::RtcDateTime rtc;
  rtc.year = static_cast<uint16_t>(timeInfo.tm_year + 1900);
  rtc.month = static_cast<uint8_t>(timeInfo.tm_mon + 1);
  rtc.day = static_cast<uint8_t>(timeInfo.tm_mday);
  rtc.hour = static_cast<uint8_t>(timeInfo.tm_hour);
  rtc.minute = static_cast<uint8_t>(timeInfo.tm_min);
  rtc.second = static_cast<uint8_t>(timeInfo.tm_sec);
  rtc.valid = true;

  if (!BoardConfig::rtcWrite(rtc)) {
    display_.renderStatus("Clock", "RTC write failed", "");
    delay(1500);
    return false;
  }

  if (onClockSet_) {
    onClockSet_();  // start the streak now that the clock is set
  }

  // Show the local time (RTC holds UTC; localNow applies the offset).
  BoardConfig::RtcDateTime local;
  int32_t day = 0;
  char buf[24];
  if (localNow(local, day)) {
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", local.year, local.month, local.day,
                  local.hour, local.minute);
  } else {
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", rtc.year, rtc.month, rtc.day,
                  rtc.hour, rtc.minute);
  }
  Serial.printf("[clock] RTC set (UTC) from NTP; local %s %s\n", buf, timezoneLabel().c_str());
  display_.renderStatus("Clock set", buf, timezoneLabel());
  delay(1500);
  return true;
}
