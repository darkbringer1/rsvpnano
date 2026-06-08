#pragma once

#include <Arduino.h>
#include <driver/i2s.h>

class AudioManager {
 public:
  bool begin();
  bool beep();
  bool tone(uint32_t frequencyHz, uint32_t durationMs);
  bool tone(uint32_t frequencyHz, uint32_t durationMs, int16_t amplitude);
  bool available() const;
  void setVolumePercent(uint8_t percent);
  uint8_t volumePercent() const;

 private:
  static constexpr uint32_t kSampleRateHz = 16000;
  static constexpr uint32_t kMaxToneDurationMs = 180;
  static constexpr uint32_t kDefaultBeepDurationMs = 120;
  static constexpr uint32_t kDefaultBeepFrequencyHz = 1320;
  static constexpr int16_t kDefaultToneAmplitude = 12000;
  static constexpr size_t kMaxToneFrames =
      (static_cast<size_t>(kSampleRateHz) * kMaxToneDurationMs) / 1000U;
  static constexpr size_t kMaxToneSamples = kMaxToneFrames * 2U;
  static constexpr i2s_port_t kI2sPort = I2S_NUM_0;

  bool enableAudioRail();
  bool initI2s();
  bool initCodec();
  bool configureCodec();
  bool configureCodecSampleFormat();
  bool startCodec();
  uint8_t codecVolumeRegister() const;
  bool prepareForBeep();
  bool recoverOutputPath();
  bool writeToneBuffer(size_t frames);
  bool readIoRegister(uint8_t reg, uint8_t &value);
  bool writeIoRegister(uint8_t reg, uint8_t value);
  bool readCodecRegister(uint8_t reg, uint8_t &value);
  bool writeCodecRegister(uint8_t reg, uint8_t value);
  size_t fillToneBuffer(uint32_t frequencyHz, uint32_t durationMs, int16_t amplitude);

  bool available_ = false;
  bool i2sInitialized_ = false;
  uint8_t volumePercent_ = 100;
  int16_t toneBuffer_[kMaxToneSamples] = {};
};
