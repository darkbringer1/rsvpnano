#include "board/BoardConfig.h"

#include <Wire.h>
#include <algorithm>
#include <driver/gpio.h>
#include <esp_sleep.h>

#if defined(BOARD_AMOLED_18)
#include <XPowersLib.h>
#endif

namespace BoardConfig {

namespace {

constexpr uint8_t kTca9554OutputReg = 0x01;
constexpr uint8_t kTca9554ConfigReg = 0x03;
bool gBatteryPowerHoldEnabled = false;
bool gBatteryAdcPathEnabled = false;
constexpr float kBatteryDividerRatio = 3.0f;
constexpr float kBatteryVoltageOffset = 0.0f;

bool tca9554Read(uint8_t reg, uint8_t &value) {
  Wire1.beginTransmission(TCA9554_ADDRESS);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) {
    return false;
  }

  if (Wire1.requestFrom(static_cast<uint8_t>(TCA9554_ADDRESS), static_cast<uint8_t>(1)) != 1) {
    return false;
  }

  value = Wire1.read();
  return true;
}

bool tca9554Write(uint8_t reg, uint8_t value) {
  Wire1.beginTransmission(TCA9554_ADDRESS);
  Wire1.write(reg);
  Wire1.write(value);
  return Wire1.endTransmission(true) == 0;
}

bool configureTca9554OutputPin(uint8_t pin, bool high) {
  uint8_t output = 0;
  if (!tca9554Read(kTca9554OutputReg, output)) {
    return false;
  }

  const uint8_t mask = static_cast<uint8_t>(1U << pin);
  if (high) {
    output |= mask;
  } else {
    output &= static_cast<uint8_t>(~mask);
  }
  if (!tca9554Write(kTca9554OutputReg, output)) {
    return false;
  }

  uint8_t config = 0xFF;
  if (!tca9554Read(kTca9554ConfigReg, config)) {
    return false;
  }

  config &= static_cast<uint8_t>(~mask);
  return tca9554Write(kTca9554ConfigReg, config);
}

void holdBatteryPowerIfAvailable() {
  if (gBatteryPowerHoldEnabled) {
    return;
  }

  if (!configureTca9554OutputPin(TCA9554_PIN_SYS_EN, true)) {
    Serial.println("[board] TCA9554 not detected; battery power hold not configured");
    return;
  }

  gBatteryPowerHoldEnabled = true;
  Serial.println("[board] Battery power hold enabled");
}

void enableBatteryAdcPathIfAvailable() {
  if (gBatteryAdcPathEnabled) {
    return;
  }

  if (!configureTca9554OutputPin(TCA9554_PIN_BATTERY_ADC_ENABLE, false)) {
    Serial.println("[board] TCA9554 battery ADC gate not configured");
    return;
  }

  gBatteryAdcPathEnabled = true;
  Serial.println("[board] Battery ADC path enabled");
}

void disableBatteryAdcPathIfAvailable() {
  // Keep the battery divider gate off outside short samples; it shares the board expander.
  if (!configureTca9554OutputPin(TCA9554_PIN_BATTERY_ADC_ENABLE, true)) {
    if (gBatteryAdcPathEnabled) {
      Serial.println("[board] TCA9554 battery ADC gate disable failed");
    }
    return;
  }

  gBatteryAdcPathEnabled = false;
}

uint8_t batteryPercentForVoltage(float voltage) {
  struct Point {
    float voltage;
    uint8_t percent;
  };

  constexpr Point kCurve[] = {
      {3.30f, 0},  {3.50f, 5},  {3.60f, 10}, {3.65f, 20},
      {3.70f, 30}, {3.75f, 40}, {3.79f, 50}, {3.85f, 60},
      {3.92f, 70}, {4.00f, 80}, {4.10f, 90}, {4.20f, 100},
  };

  if (voltage <= kCurve[0].voltage) {
    return kCurve[0].percent;
  }
  constexpr size_t curveSize = sizeof(kCurve) / sizeof(kCurve[0]);
  if (voltage >= kCurve[curveSize - 1].voltage) {
    return kCurve[curveSize - 1].percent;
  }

  for (size_t i = 1; i < curveSize; ++i) {
    const Point &upper = kCurve[i];
    const Point &lower = kCurve[i - 1];
    if (voltage > upper.voltage) {
      continue;
    }

    const float span = upper.voltage - lower.voltage;
    const float ratio = span <= 0.0f ? 0.0f : (voltage - lower.voltage) / span;
    const int percent =
        static_cast<int>(lower.percent + (upper.percent - lower.percent) * ratio + 0.5f);
    return static_cast<uint8_t>(std::max(0, std::min(100, percent)));
  }

  return 0;
}

#if defined(BOARD_AMOLED_18)
// --- AXP2101 PMU battery read (Waveshare ESP32-S3-Touch-AMOLED-1.8) ----------
// The AXP2101 shares the touch I2C bus (Wire, GPIO15/14) at address 0x34 and
// powers the whole board. We ONLY read ADC/status registers and, at most, do a
// read-modify-write to the ADC-enable register (0x30). We never touch DCDC/LDO
// power-rail registers: a bad write there would brown out the device.
//
// UNVERIFIED ON HARDWARE: written without a battery attached. If a battery is
// connected and this reads wrong, the suspect registers are noted inline.
constexpr uint8_t kAxp2101Address = 0x34;
constexpr uint8_t kAxpRegChipId = 0x03;          // expect 0x4A on AXP2101
constexpr uint8_t kAxpRegStatus1 = 0x00;         // bit3 = battery connected
constexpr uint8_t kAxpRegAdcEnable = 0x30;       // bit0 = VBAT voltage ADC
constexpr uint8_t kAxpRegVbatHigh = 0x34;        // [5:0] high bits of VBAT mV
constexpr uint8_t kAxpRegVbatLow = 0x35;         // [7:0] low bits of VBAT mV
bool gAxp2101Probed = false;
bool gAxp2101Present = false;

bool axpReadReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(kAxp2101Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(kAxp2101Address), static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

bool axpWriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAxp2101Address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

// Detect the PMU and enable the battery-voltage ADC. Power rails untouched.
void probeAxp2101IfNeeded() {
  if (gAxp2101Probed) {
    return;
  }
  gAxp2101Probed = true;

  uint8_t chipId = 0;
  if (!axpReadReg(kAxpRegChipId, chipId)) {
    Serial.println("[board] AXP2101 not detected on Wire (no battery telemetry)");
    return;
  }
  // AXP2101 chip ID is 0x4A; accept anything that ACKed but warn on mismatch.
  if (chipId != 0x4A) {
    Serial.printf("[board] AXP2101 unexpected chip id 0x%02X (continuing)\n", chipId);
  }

  // Read-modify-write only the ADC-enable register to turn on the VBAT ADC.
  uint8_t adcEnable = 0;
  if (axpReadReg(kAxpRegAdcEnable, adcEnable)) {
    const uint8_t want = adcEnable | 0x01;  // bit0 = VBAT voltage ADC
    if (want != adcEnable) {
      axpWriteReg(kAxpRegAdcEnable, want);
    }
  }

  gAxp2101Present = true;
  Serial.println("[board] AXP2101 detected; battery voltage ADC enabled");
}

bool readBatteryStatusAxp2101(BatteryStatus &status) {
  probeAxp2101IfNeeded();
  if (!gAxp2101Present) {
    return false;
  }

  uint8_t high = 0;
  uint8_t low = 0;
  if (!axpReadReg(kAxpRegVbatHigh, high) || !axpReadReg(kAxpRegVbatLow, low)) {
    return false;
  }

  // VBAT ADC is a 14-bit value in millivolts (1 mV / LSB) on the AXP2101.
  const uint16_t millivolts = static_cast<uint16_t>((high & 0x3F) << 8) | low;
  status.voltage = static_cast<float>(millivolts) / 1000.0f;

  // No battery (or ADC not yet settled) reads ~0. Treat as absent.
  status.present = status.voltage >= 2.5f && status.voltage <= 4.6f;
  if (!status.present) {
    status.percent = 0;
    return false;
  }

  status.percent = batteryPercentForVoltage(status.voltage);
  return true;
}

// --- AXP2101 PMU control via XPowersLib (PWR button + real power off) --------
// The hand-rolled reads above stay as a fallback; once the library PMU is up we
// prefer it because it also gives PWRKEY events, charging state, and shutdown().
XPowersAXP2101 gPmu;
bool gPmuReady = false;
bool gPmuBeginAttempted = false;

bool pmuBeginInternal() {
  if (gPmuBeginAttempted) {
    return gPmuReady;
  }
  gPmuBeginAttempted = true;

  // Wire is already started in begin(); XPowersLib re-begins harmlessly.
  if (!gPmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_TOUCH_SDA, PIN_TOUCH_SCL)) {
    Serial.println("[board] AXP2101 PMU begin failed (XPowersLib); using fallback reads");
    return false;
  }

  // Enable the fuel-gauge ADC so getBatteryPercent() is meaningful.
  gPmu.enableBattDetection();
  gPmu.enableBattVoltageMeasure();
  gPmu.enableVbusVoltageMeasure();
  gPmu.enableSystemVoltageMeasure();

  // PWRKEY timing: a long hold (~4s) powers off; while OFF, the PMU powers the
  // board back on only when PWRKEY is held ~2s (a tap will not power on).
  gPmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
  gPmu.setPowerKeyPressOnTime(XPOWERS_POWERON_2S);

  // PWRKEY events: short tap (ignored) and long press (commit to power off).
  // Clear any latched state from boot.
  gPmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  gPmu.clearIrqStatus();
  gPmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);

  gPmuReady = true;
  Serial.println("[board] AXP2101 PMU ready (XPowersLib)");
  return true;
}
#endif  // BOARD_AMOLED_18

}  // namespace

void begin() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_PWR_BUTTON, INPUT_PULLUP);
  gpio_deep_sleep_hold_dis();
#if !defined(BOARD_AMOLED_18)
  // AMOLED panel is self-emissive (no backlight pin); GPIO8 is an I2S line there.
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_LCD_BACKLIGHT));
  pinMode(PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LCD_BACKLIGHT, LOW);
#endif

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  Wire.setClock(300000);
  Wire.setTimeOut(10);

  Wire1.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire1.setClock(300000);
  Wire1.setTimeOut(10);
  holdBatteryPowerIfAvailable();
  disableBatteryAdcPathIfAvailable();

#if defined(BOARD_AMOLED_18)
  pmuBeginInternal();  // bring up the AXP2101 PMU (PWR button + battery + power off)
#endif

#if !defined(BOARD_AMOLED_18)
  // On AMOLED, PIN_BATTERY_ADC overlaps an LCD data line; battery comes from AXP2101 instead.
  pinMode(PIN_BATTERY_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
#endif
}

void lightSleepUntilBootButton() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  gpio_wakeup_enable(static_cast<gpio_num_t>(PIN_BOOT_BUTTON), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.flush();
  esp_light_sleep_start();
  gpio_wakeup_disable(static_cast<gpio_num_t>(PIN_BOOT_BUTTON));
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

void holdBacklightOffForDeepSleep() {
#if defined(BOARD_AMOLED_18)
  return;  // No backlight pin on the AMOLED board.
#else
  const gpio_num_t backlightPin = static_cast<gpio_num_t>(PIN_LCD_BACKLIGHT);

  // The LCD backlight is active-low. Hold the inactive level while the ESP32 is in deep sleep,
  // because PWM output stops there and can otherwise leave the backlight pin floating.
  analogWrite(PIN_LCD_BACKLIGHT, 255);
  pinMode(PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LCD_BACKLIGHT, HIGH);
  gpio_set_direction(backlightPin, GPIO_MODE_OUTPUT);
  gpio_set_level(backlightPin, 1);
  gpio_hold_en(backlightPin);
  gpio_deep_sleep_hold_en();
#endif
}

bool readBatteryStatus(BatteryStatus &status) {
  status = BatteryStatus{};
#if defined(BOARD_AMOLED_18)
  if (pmuBeginInternal()) {
    status.charging = gPmu.isCharging();
    status.voltage = static_cast<float>(gPmu.getBattVoltage()) / 1000.0f;
    const int pct = gPmu.getBatteryPercent();
    status.present = gPmu.isBatteryConnect();
    if (pct >= 0) {
      status.percent = static_cast<uint8_t>(pct);
    } else if (status.present) {
      status.percent = batteryPercentForVoltage(status.voltage);
    }
    return status.present;
  }
  return readBatteryStatusAxp2101(status);  // fallback: hand-rolled AXP2101 reads.
#else
  enableBatteryAdcPathIfAvailable();
  delay(12);

  constexpr uint8_t kMaxSamples = 24;
  uint32_t millivolts[kMaxSamples];
  uint8_t samples = 0;
  for (uint8_t i = 0; i < kMaxSamples + 2; ++i) {
    const uint32_t sample = analogReadMilliVolts(PIN_BATTERY_ADC);
    if (i >= 2 && sample > 0 && samples < kMaxSamples) {
      millivolts[samples] = sample;
      ++samples;
    }
    delayMicroseconds(500);
  }

  if (samples == 0) {
    uint32_t rawTotal = 0;
    constexpr uint8_t kRawSamples = 16;
    for (uint8_t i = 0; i < kRawSamples; ++i) {
      rawTotal += analogRead(PIN_BATTERY_ADC);
      delayMicroseconds(500);
    }
    const float pinMillivolts =
        (static_cast<float>(rawTotal) / static_cast<float>(kRawSamples)) * 3300.0f / 4095.0f;
    status.voltage = (pinMillivolts * kBatteryDividerRatio / 1000.0f) + kBatteryVoltageOffset;
  } else {
    std::sort(millivolts, millivolts + samples);
    const uint8_t trim = samples >= 10 ? 2 : 0;
    uint32_t trimmedTotal = 0;
    uint8_t trimmedSamples = 0;
    for (uint8_t i = trim; i < samples - trim; ++i) {
      trimmedTotal += millivolts[i];
      ++trimmedSamples;
    }
    const float pinMillivolts =
        static_cast<float>(trimmedTotal) / static_cast<float>(std::max<uint8_t>(1, trimmedSamples));
    status.voltage = (pinMillivolts * kBatteryDividerRatio / 1000.0f) + kBatteryVoltageOffset;
  }
  disableBatteryAdcPathIfAvailable();

  status.present = status.voltage >= 2.5f && status.voltage <= 4.6f;
  if (!status.present) {
    status.percent = 0;
    return false;
  }

  status.percent = batteryPercentForVoltage(status.voltage);
  return true;
#endif
}

bool releaseBatteryPowerHold() {
  if (!configureTca9554OutputPin(TCA9554_PIN_SYS_EN, false)) {
    Serial.println("[board] Battery power hold release failed");
    return false;
  }

  gBatteryPowerHoldEnabled = false;
  Serial.println("[board] Battery power hold released");
  return true;
}

bool pmuPresent() {
#if defined(BOARD_AMOLED_18)
  return pmuBeginInternal();
#else
  return false;
#endif
}

PowerKeyEvent pmuPollPowerKey() {
#if defined(BOARD_AMOLED_18)
  if (!pmuBeginInternal()) {
    return PowerKeyEvent::None;
  }
  gPmu.getIrqStatus();  // latch current IRQ flags
  PowerKeyEvent event = PowerKeyEvent::None;
  if (gPmu.isPekeyLongPressIrq()) {
    event = PowerKeyEvent::LongPress;
  } else if (gPmu.isPekeyShortPressIrq()) {
    event = PowerKeyEvent::ShortPress;
  }
  // clearIrqStatus() writes 0xFF to every INTSTS reg (write-1-to-clear), wiping
  // ALL pending bits, not just the ones we read above. The AXP2101 latches the
  // PWRKEY short-press IRQ on key *release*; if that release lands in the window
  // between getIrqStatus() and this clear, an unconditional clear would erase the
  // freshly-set bit before it is ever observed -> the wake tap is silently lost.
  // Only short/long PKEY IRQs are enabled, so on an idle poll there is nothing to
  // clear; skip it and let any bit set in the race window survive to the next
  // poll, where it gets read and handled.
  if (event != PowerKeyEvent::None) {
    gPmu.clearIrqStatus();
  }
  return event;
#else
  return PowerKeyEvent::None;
#endif
}

bool pmuVbusPresent() {
#if defined(BOARD_AMOLED_18)
  return pmuBeginInternal() && gPmu.isVbusIn();
#else
  return false;
#endif
}

void pmuShutdown() {
#if defined(BOARD_AMOLED_18)
  if (pmuBeginInternal()) {
    Serial.flush();
    gPmu.shutdown();  // cut the main rail; only PWRKEY can power back on
  }
#endif
}

// --- PCF85063 RTC (shared touch I2C bus, addr 0x51) -------------------------
#if defined(BOARD_AMOLED_18)
namespace {
constexpr uint8_t kPcf85063Address = 0x51;
constexpr uint8_t kPcfRegSeconds = 0x04;  // time block starts here (7 bytes)
inline uint8_t bcd2dec(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
inline uint8_t dec2bcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
}  // namespace
#endif

bool rtcPresent() {
#if defined(BOARD_AMOLED_18)
  Wire.beginTransmission(kPcf85063Address);
  return Wire.endTransmission() == 0;
#else
  return false;
#endif
}

bool rtcRead(RtcDateTime &out) {
#if defined(BOARD_AMOLED_18)
  Wire.beginTransmission(kPcf85063Address);
  Wire.write(kPcfRegSeconds);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(kPcf85063Address), static_cast<uint8_t>(7)) != 7) {
    return false;
  }
  const uint8_t rawSec = Wire.read();
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();
  const uint8_t rawDay = Wire.read();
  (void)Wire.read();  // weekday (unused)
  const uint8_t rawMonth = Wire.read();
  const uint8_t rawYear = Wire.read();

  out.valid = (rawSec & 0x80) == 0;  // bit7 = oscillator-stop (clock never set)
  out.second = bcd2dec(rawSec & 0x7F);
  out.minute = bcd2dec(rawMin & 0x7F);
  out.hour = bcd2dec(rawHour & 0x3F);
  out.day = bcd2dec(rawDay & 0x3F);
  out.month = bcd2dec(rawMonth & 0x1F);
  out.year = static_cast<uint16_t>(2000 + bcd2dec(rawYear));
  return true;
#else
  (void)out;
  return false;
#endif
}

bool rtcWrite(const RtcDateTime &in) {
#if defined(BOARD_AMOLED_18)
  Wire.beginTransmission(kPcf85063Address);
  Wire.write(kPcfRegSeconds);
  Wire.write(dec2bcd(in.second) & 0x7F);  // clearing bit7 also clears the OS flag
  Wire.write(dec2bcd(in.minute));
  Wire.write(dec2bcd(in.hour));
  Wire.write(dec2bcd(in.day));
  Wire.write(0);  // weekday (not tracked)
  Wire.write(dec2bcd(in.month));
  Wire.write(dec2bcd(static_cast<uint8_t>(in.year % 100)));
  return Wire.endTransmission() == 0;
#else
  (void)in;
  return false;
#endif
}

String pmuDebugSummary() {
#if defined(BOARD_AMOLED_18)
  if (!pmuBeginInternal()) {
    return String("PMU:absent");
  }
  String s = "PMU VBUS:";
  s += gPmu.isVbusIn() ? "1" : "0";
  s += " BAT:";
  s += gPmu.isBatteryConnect() ? "1" : "0";
  s += " CHG:";
  s += gPmu.isCharging() ? "1" : "0";
  return s;
#else
  return String("PMU:n/a");
#endif
}

}  // namespace BoardConfig
