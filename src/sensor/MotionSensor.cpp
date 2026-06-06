#include "sensor/MotionSensor.h"

#include <Wire.h>

#include "board/BoardConfig.h"

#if defined(BOARD_AMOLED_18)
#define IMU_WIRE Wire  // QMI8658 shares the touch I2C bus (SDA15/SCL14) on the AMOLED board
#else
#define IMU_WIRE Wire1
#endif

namespace {

constexpr uint8_t kImuAddress = 0x6B;
constexpr uint8_t kImuWhoAmIReg = 0x00;
constexpr uint8_t kImuCtrl1Reg = 0x02;
constexpr uint8_t kImuCtrl2Reg = 0x03;
constexpr uint8_t kImuCtrl5Reg = 0x06;
constexpr uint8_t kImuCtrl7Reg = 0x08;
constexpr uint8_t kImuCtrl8Reg = 0x09;
constexpr uint8_t kImuAccelStartReg = 0x35;
constexpr uint8_t kImuResetReg = 0x60;
constexpr uint8_t kImuResetValue = 0xB0;
constexpr uint8_t kImuResetResultReg = 0x4D;
constexpr uint8_t kImuResetResultValue = 0x80;
constexpr uint8_t kImuWhoAmIValue = 0x05;

}  // namespace

bool MotionSensor::begin() { return initImu(); }

bool MotionSensor::initImu() {
  if (available_) {
    return true;
  }

  IMU_WIRE.beginTransmission(kImuAddress);
  if (IMU_WIRE.endTransmission(true) != 0) {
    available_ = false;
    return false;
  }

  if (!writeRegister(kImuResetReg, kImuResetValue)) {
    available_ = false;
    return false;
  }

  const uint32_t waitStartedMs = millis();
  uint8_t resetResult = 0;
  bool resetReady = false;
  while (millis() - waitStartedMs < 500) {
    if (readRegister(kImuResetResultReg, resetResult) &&
        resetResult == kImuResetResultValue) {
      resetReady = true;
      break;
    }
    delay(10);
  }

  if (!resetReady) {
    available_ = false;
    return false;
  }

  uint8_t whoAmI = 0;
  if (!readRegister(kImuWhoAmIReg, whoAmI) || whoAmI != kImuWhoAmIValue) {
    available_ = false;
    return false;
  }

  if (!updateRegister(kImuCtrl1Reg, 0x40, 0x40) ||
      !writeRegister(kImuCtrl8Reg, 0x80) ||
      !writeRegister(kImuCtrl2Reg, 0x16) ||
      !updateRegister(kImuCtrl5Reg, 0x07, 0x07) ||
      !updateRegister(kImuCtrl7Reg, 0x01, 0x01)) {
    available_ = false;
    return false;
  }

  accelScale_ = 4.0f / 32768.0f;
  available_ = true;
  return true;
}

bool MotionSensor::readRegister(uint8_t reg, uint8_t &value) {
  IMU_WIRE.beginTransmission(kImuAddress);
  IMU_WIRE.write(reg);
  if (IMU_WIRE.endTransmission(false) != 0) {
    return false;
  }
  if (IMU_WIRE.requestFrom(static_cast<int>(kImuAddress), 1, 1) != 1) {
    return false;
  }
  value = IMU_WIRE.read();
  return true;
}

bool MotionSensor::writeRegister(uint8_t reg, uint8_t value) {
  IMU_WIRE.beginTransmission(kImuAddress);
  IMU_WIRE.write(reg);
  IMU_WIRE.write(value);
  return IMU_WIRE.endTransmission(true) == 0;
}

bool MotionSensor::readRegisters(uint8_t startReg, uint8_t *buffer, size_t len) {
  if (buffer == nullptr || len == 0 || len > 32) {
    return false;
  }
  IMU_WIRE.beginTransmission(kImuAddress);
  IMU_WIRE.write(startReg);
  if (IMU_WIRE.endTransmission(false) != 0) {
    return false;
  }
  if (IMU_WIRE.requestFrom(static_cast<int>(kImuAddress), static_cast<int>(len), 1) !=
      static_cast<int>(len)) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    buffer[i] = IMU_WIRE.read();
  }
  return true;
}

bool MotionSensor::updateRegister(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!readRegister(reg, current)) {
    return false;
  }
  current = static_cast<uint8_t>((current & static_cast<uint8_t>(~mask)) | (value & mask));
  return writeRegister(reg, current);
}

bool MotionSensor::readAccel(float &x, float &y, float &z) {
  if (!available_) {
    return false;
  }
  uint8_t buffer[6] = {0};
  if (!readRegisters(kImuAccelStartReg, buffer, sizeof(buffer))) {
    return false;
  }
  const int16_t rawX = static_cast<int16_t>((buffer[1] << 8) | buffer[0]);
  const int16_t rawY = static_cast<int16_t>((buffer[3] << 8) | buffer[2]);
  const int16_t rawZ = static_cast<int16_t>((buffer[5] << 8) | buffer[4]);
  x = rawX * accelScale_;
  y = rawY * accelScale_;
  z = rawZ * accelScale_;
  return true;
}
