#include "input/TouchHandler.h"

#include <algorithm>
#include <Wire.h>

#include "board/BoardConfig.h"

namespace {

constexpr uint8_t kReadTouchCommand[] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
};
constexpr uint32_t kPollIntervalMs = 20;
constexpr uint32_t kFailureBackoffMs = 250;
constexpr uint8_t kReleaseConfirmSamples = 2;

uint16_t clampDisplayX(uint16_t x) {
  return std::min<uint16_t>(x, static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 1));
}

uint16_t clampDisplayY(uint16_t y) {
  return std::min<uint16_t>(y, static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - 1));
}

uint16_t clampPhysicalX(uint16_t x) {
  return std::min<uint16_t>(x, static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_WIDTH - 1));
}

uint16_t clampPhysicalY(uint16_t y) {
  return std::min<uint16_t>(y, static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_HEIGHT - 1));
}

}  // namespace

bool TouchHandler::begin() {
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  touchActive_ = false;
  lastX_ = 0;
  lastY_ = 0;
  Wire.beginTransmission(kAddress);
  const uint8_t error = Wire.endTransmission();
  initialized_ = (error == 0);

  if (!initialized_) {
    Serial.printf("[touch] Controller not detected at 0x%02X\n", kAddress);
  } else {
#if defined(BOARD_AMOLED_18)
    Serial.println("[touch] Initialized (FT3168)");
#else
    Serial.println("[touch] Initialized (AXS15231B)");
#endif
  }

  return initialized_;
}

void TouchHandler::end() {
  cancel();
  initialized_ = false;
  Wire.end();
}

void TouchHandler::cancel() {
  touchActive_ = false;
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
}

void TouchHandler::setUiOrientation(BoardConfig::UiOrientation orientation) {
  if (uiOrientation_ == orientation) {
    return;
  }

  uiOrientation_ = orientation;
  cancel();
}

void TouchHandler::setUiRotated180(bool rotated180) {
  setUiOrientation(rotated180 ? BoardConfig::UiOrientation::LandscapeFlipped
                              : BoardConfig::UiOrientation::Landscape);
}

bool TouchHandler::readTouchPacket(uint8_t *buffer, size_t len) {
#if defined(BOARD_AMOLED_18)
  // FT3168: set register pointer to 0x02 (TD_STATUS) with a STOP, then read
  // regs 0x02..0x06 -> [points, X1H, X1L, Y1H, Y1L]. Repeated-start fails on this chip.
  (void)len;
  constexpr size_t kFtReadLen = 5;
  Wire.beginTransmission(kAddress);
  Wire.write(0x02);
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  const size_t readLen =
      Wire.requestFrom(static_cast<uint8_t>(kAddress), static_cast<size_t>(kFtReadLen), true);
  if (readLen != kFtReadLen) {
    return false;
  }
  for (size_t i = 0; i < kFtReadLen; ++i) {
    buffer[i] = Wire.read();
  }
  return true;
#else
  Wire.beginTransmission(kAddress);
  Wire.write(kReadTouchCommand, sizeof(kReadTouchCommand));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const size_t readLen =
      Wire.requestFrom(static_cast<uint8_t>(kAddress), static_cast<size_t>(len), true);
  if (readLen != len) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    buffer[i] = Wire.read();
  }
  return true;
#endif
}

bool TouchHandler::poll(TouchEvent &event) {
  event = TouchEvent{};

  if (!initialized_) {
    return false;
  }

  const uint32_t now = millis();
  if (now < backoffUntilMs_) {
    return false;
  }

  if (now - lastPollMs_ < kPollIntervalMs) {
    return false;
  }
  lastPollMs_ = now;

  uint8_t data[8] = {0};
  if (!readTouchPacket(data, sizeof(data))) {
    backoffUntilMs_ = now + kFailureBackoffMs;
    if (++consecutiveReadFailures_ >= 5) {
      initialized_ = false;
      Serial.println("[touch] Read failed repeatedly, disabling touch polling");
    }
    return false;
  }
  consecutiveReadFailures_ = 0;

#if defined(BOARD_AMOLED_18)
  const uint8_t points = data[0] & 0x0F;  // FT3168 TD_STATUS
#else
  const uint8_t points = data[1];
#endif
  if (points == 0 || points >= 5) {
    if (touchActive_) {
      ++emptyTouchSamples_;
      if (emptyTouchSamples_ < kReleaseConfirmSamples) {
        return false;
      }

      touchActive_ = false;
      emptyTouchSamples_ = 0;
      event.touched = false;
      event.x = lastX_;
      event.y = lastY_;
      event.phase = TouchPhase::End;
      return true;
    }
    return false;
  }

  backoffUntilMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  lastTouchSampleMs_ = now;

  event.touched = true;
  event.gesture = 0;
  event.phase = touchActive_ ? TouchPhase::Move : TouchPhase::Start;
#if defined(BOARD_AMOLED_18)
  // FT3168 reports panel-native coordinates directly: X 0..367, Y 0..447.
  const uint16_t ftX = static_cast<uint16_t>(((data[1] & 0x0F) << 8) | data[2]);
  const uint16_t ftY = static_cast<uint16_t>(((data[3] & 0x0F) << 8) | data[4]);
  const uint16_t physicalX = clampPhysicalX(ftX);
  const uint16_t physicalY = clampPhysicalY(ftY);
#else
  const uint16_t rawLongAxis = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
  const uint16_t rawShortAxis = static_cast<uint16_t>(((data[4] & 0x0F) << 8) | data[5]);
  const uint16_t physicalX = clampPhysicalX(rawShortAxis);
  const uint16_t physicalY =
      clampPhysicalY(rawLongAxis >= BoardConfig::PANEL_NATIVE_HEIGHT
                         ? 0
                         : static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_HEIGHT - 1 - rawLongAxis));
#endif

  switch (uiOrientation_) {
    case BoardConfig::UiOrientation::Portrait:
      event.x = physicalX;
      event.y = physicalY;
      break;
    case BoardConfig::UiOrientation::PortraitFlipped:
      event.x = static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_WIDTH - 1 - physicalX);
      event.y = static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_HEIGHT - 1 - physicalY);
      break;
    case BoardConfig::UiOrientation::Landscape:
      event.x = clampDisplayX(static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_HEIGHT - 1 - physicalY));
      event.y = clampDisplayY(physicalX);
      break;
    case BoardConfig::UiOrientation::LandscapeFlipped:
    default:
      event.x = clampDisplayX(physicalY);
      event.y = clampDisplayY(
          static_cast<uint16_t>(BoardConfig::PANEL_NATIVE_WIDTH - 1 - physicalX));
      break;
  }
  touchActive_ = true;
  lastX_ = event.x;
  lastY_ = event.y;

  return true;
}
