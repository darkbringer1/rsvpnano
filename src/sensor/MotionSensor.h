#pragma once

#include <Arduino.h>
#include <stdint.h>

// Thin driver for the QMI8658 accelerometer shared across the app.
//
// On the AMOLED board the QMI8658 lives on the same I2C bus as the touch panel
// (Wire); on the other board it is on Wire1. A single instance is owned by App
// and borrowed by features that need motion (FocusTimer gestures, app-wide
// auto-rotate). Reads are cheap and side-effect free, so multiple consumers can
// poll the same instance each loop.
class MotionSensor {
 public:
  bool begin();
  bool available() const { return available_; }

  // Returns acceleration in g for each axis. False if the read failed or the
  // sensor is unavailable.
  bool readAccel(float &x, float &y, float &z);

 private:
  bool initImu();
  bool readRegister(uint8_t reg, uint8_t &value);
  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegisters(uint8_t startReg, uint8_t *buffer, size_t len);
  bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value);

  bool available_ = false;
  float accelScale_ = 4.0f / 32768.0f;
};
