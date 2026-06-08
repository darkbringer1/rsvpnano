#pragma once

#include <Arduino.h>

namespace BoardConfig {

enum class UiOrientation : uint8_t {
  Landscape = 0,
  LandscapeFlipped,
  Portrait,
  PortraitFlipped,
};

// App-wide 180 auto-rotate calibration (QMI8658 accelerometer).
// Only a 180 flip is ever applied (Landscape <-> LandscapeFlipped), so we look
// at a single in-screen axis whose sign distinguishes upright from upside-down.
// These are TUNED ON DEVICE: read the throttled "[orient]" console log while
// holding the device upright vs flipped, then set axis/sign so the decision is
// correct. IMU_FLIP_THRESHOLD is the hysteresis half-band in g.
constexpr uint8_t IMU_FLIP_AXIS = 1;            // 0=x, 1=y, 2=z (upright: y ~= -0.9g)
constexpr float IMU_FLIP_UPRIGHT_SIGN = -1.0f;  // sign of that axis when upright
constexpr float IMU_FLIP_THRESHOLD = 0.40f;     // g

#if defined(BOARD_AMOLED_18)
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (SH8601 368x448 QSPI, FT3168 touch).
constexpr int PIN_BOOT_BUTTON = 0;
constexpr int PIN_PWR_BUTTON = 18;  // No second hardware button; unused pin held by pull-up.
constexpr int PIN_BATTERY_ADC = 10;  // Battery is via AXP2101 PMU (deferred); ADC path unused.

constexpr int PIN_LCD_CS = 12;
constexpr int PIN_LCD_SCLK = 11;
constexpr int PIN_LCD_DATA0 = 4;
constexpr int PIN_LCD_DATA1 = 5;
constexpr int PIN_LCD_DATA2 = 6;
constexpr int PIN_LCD_DATA3 = 7;
constexpr int PIN_LCD_RST = -1;       // Reset is driven via the XCA9554 expander, not a GPIO.
constexpr int PIN_LCD_BACKLIGHT = 8;  // AMOLED has no backlight; value kept for compile only.

constexpr int PANEL_NATIVE_WIDTH = 368;   // physical panel columns (CASET)
constexpr int PANEL_NATIVE_HEIGHT = 448;  // physical panel rows (PASET)
// Logical UI is landscape; App always runs Landscape, so DISPLAY = swapped native.
constexpr int DISPLAY_WIDTH = 448;
constexpr int DISPLAY_HEIGHT = 368;
constexpr bool UI_ROTATED_180 = false;

constexpr int PIN_SD_CLK = 2;
constexpr int PIN_SD_CMD = 1;
constexpr int PIN_SD_D0 = 3;
constexpr int PIN_I2C_SDA = 47;  // Wire1 (unused on this board; left harmless).
constexpr int PIN_I2C_SCL = 48;
constexpr int PIN_TOUCH_SDA = 15;  // Wire: FT3168 touch + XCA9554 expander share this bus.
constexpr int PIN_TOUCH_SCL = 14;
#else
constexpr int PIN_BOOT_BUTTON = 0;
constexpr int PIN_PWR_BUTTON = 16;
constexpr int PIN_BATTERY_ADC = 4;

constexpr int PIN_LCD_CS = 9;
constexpr int PIN_LCD_SCLK = 10;
constexpr int PIN_LCD_DATA0 = 11;
constexpr int PIN_LCD_DATA1 = 12;
constexpr int PIN_LCD_DATA2 = 13;
constexpr int PIN_LCD_DATA3 = 14;
constexpr int PIN_LCD_RST = 21;
constexpr int PIN_LCD_BACKLIGHT = 8;

constexpr int PANEL_NATIVE_WIDTH = 172;
constexpr int PANEL_NATIVE_HEIGHT = 640;
constexpr int DISPLAY_WIDTH = 640;
constexpr int DISPLAY_HEIGHT = 172;
constexpr bool UI_ROTATED_180 = true;  // Keep BOOT/PWR at the top edge in landscape.

constexpr int PIN_SD_CLK = 41;
constexpr int PIN_SD_CMD = 39;
constexpr int PIN_SD_D0 = 40;
constexpr int PIN_I2C_SDA = 47;
constexpr int PIN_I2C_SCL = 48;
constexpr int PIN_TOUCH_SDA = 17;
constexpr int PIN_TOUCH_SCL = 18;
#endif

constexpr int TCA9554_ADDRESS = 0x20;
constexpr uint8_t TCA9554_PIN_BATTERY_ADC_ENABLE = 1;
constexpr uint8_t TCA9554_PIN_SYS_EN = 6;
constexpr uint8_t TCA9554_PIN_AUDIO_ENABLE = 7;

#if defined(BOARD_AMOLED_18)
constexpr int PIN_AUDIO_MCLK = 16;
constexpr int PIN_AUDIO_BCLK = 9;
constexpr int PIN_AUDIO_WS = 45;
constexpr int PIN_AUDIO_DIN = 10;
constexpr int PIN_AUDIO_DOUT = 8;
constexpr int PIN_AUDIO_PA = 46;
#else
constexpr int PIN_AUDIO_MCLK = 7;
constexpr int PIN_AUDIO_BCLK = 15;
constexpr int PIN_AUDIO_WS = 46;
constexpr int PIN_AUDIO_DIN = 6;
constexpr int PIN_AUDIO_DOUT = 45;
constexpr int PIN_AUDIO_PA = -1;
#endif
constexpr uint8_t ES8311_ADDRESS = 0x18;

struct BatteryStatus {
  bool present = false;
  float voltage = 0.0f;
  uint8_t percent = 0;
  bool charging = false;
};

// PCF85063 real-time clock (AMOLED board). `valid` is false if the oscillator
// stop flag is set (the clock has never been set since losing power).
struct RtcDateTime {
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  bool valid = false;
};

// AXP2101 PWRKEY (the physical PWR button) events, polled from the PMU IRQ.
enum class PowerKeyEvent : uint8_t {
  None,
  PressDown,   // button went down (negative edge) — show the confirm prompt
  ShortPress,  // quick tap and release
  LongPress,   // held past the PMU long-press threshold — commit to power off
};

void begin();
void lightSleepUntilBootButton();
void holdBacklightOffForDeepSleep();
bool readBatteryStatus(BatteryStatus &status);
bool releaseBatteryPowerHold();

// AXP2101 power-management interface (AMOLED board; no-ops elsewhere).
bool pmuPresent();
bool pmuVbusPresent();  // true while USB VBUS is supplying the PMU
PowerKeyEvent pmuPollPowerKey();
void pmuShutdown();
String pmuDebugSummary();  // e.g. "PMU VBUS:1 BAT:0 CHG:1" (diagnostics)

// PCF85063 RTC (AMOLED board; no-ops elsewhere).
bool rtcPresent();
bool rtcRead(RtcDateTime &out);
bool rtcWrite(const RtcDateTime &in);

}  // namespace BoardConfig
