#include "app/TextEntryController.h"

#include <algorithm>

#include "board/BoardConfig.h"

namespace {

constexpr uint16_t kKeyboardMarginX = 8;
constexpr uint16_t kKeyboardTopY = 48;
constexpr uint16_t kKeyboardRowGap = 4;
constexpr uint16_t kKeyboardRowHeight = 27;

String maskedValue(const String &value) {
  String masked;
  masked.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    masked += '*';
  }
  return masked;
}

const char *keyboardRowText(uint8_t modeValue, size_t rowIndex) {
  static constexpr const char *kLowerRows[] = {
      "qwertyuiop",
      "asdfghjkl",
      "zxcvbnm",
  };
  static constexpr const char *kUpperRows[] = {
      "QWERTYUIOP",
      "ASDFGHJKL",
      "ZXCVBNM",
  };
  static constexpr const char *kSymbolRows[] = {
      "1234567890",
      "!@#$%^&*?",
      "-_=+/:;.,",
  };

  if (rowIndex >= 3) {
    return "";
  }

  switch (modeValue) {
    case 1:
      return kUpperRows[rowIndex];
    case 2:
      return kSymbolRows[rowIndex];
    default:
      return kLowerRows[rowIndex];
  }
}

}  // namespace

TextEntryController::TextEntryController(DisplayManager &display, SubmitHandler onSubmit,
                                        CancelHandler onCancel)
    : display_(display), onSubmit_(std::move(onSubmit)), onCancel_(std::move(onCancel)) {}

void TextEntryController::open(const String &title, const String &prompt, const String &helperText,
                              const String &initialValue, const String &contextValue, bool masked,
                              size_t maxLength) {
  session_ = Session();
  session_.active = true;
  session_.mode = KeyboardMode::Lower;
  session_.title = title;
  session_.prompt = prompt;
  session_.helperText = helperText;
  session_.value = initialValue;
  session_.contextValue = contextValue;
  session_.maxLength = maxLength;
  session_.masked = masked;
  session_.revealValue = false;
  rebuildButtons();
  render();
}

void TextEntryController::close() {
  session_ = Session();
  buttons_.clear();
}

void TextEntryController::rebuildButtons() {
  buttons_.clear();
  if (!session_.active) {
    return;
  }

  const uint16_t rowPitch = kKeyboardRowHeight + kKeyboardRowGap;
  for (size_t rowIndex = 0; rowIndex < 3; ++rowIndex) {
    const String rowChars = keyboardRowText(static_cast<uint8_t>(session_.mode), rowIndex);
    const size_t keyCount = rowChars.length();
    if (keyCount == 0) {
      continue;
    }

    const int availableWidth =
        BoardConfig::DISPLAY_WIDTH - (2 * kKeyboardMarginX) -
        static_cast<int>((keyCount - 1) * kKeyboardRowGap);
    const int keyWidth = std::max(28, availableWidth / static_cast<int>(keyCount));
    const int totalWidth =
        keyWidth * static_cast<int>(keyCount) + static_cast<int>((keyCount - 1) * kKeyboardRowGap);
    int x = std::max(0, (BoardConfig::DISPLAY_WIDTH - totalWidth) / 2);
    const int y = kKeyboardTopY + static_cast<int>(rowIndex * rowPitch);

    for (size_t charIndex = 0; charIndex < keyCount; ++charIndex) {
      Button button;
      button.view.label = String(rowChars[charIndex]);
      button.view.x = static_cast<uint16_t>(x);
      button.view.y = static_cast<uint16_t>(y);
      button.view.width = static_cast<uint16_t>(keyWidth);
      button.view.height = kKeyboardRowHeight;
      button.action = Action::Insert;
      button.payload = String(rowChars[charIndex]);
      buttons_.push_back(button);
      x += keyWidth + kKeyboardRowGap;
    }
  }

  struct ControlButtonDef {
    String label;
    Action action;
    uint16_t units;
    bool accent;
    bool active;
  };

  const bool revealActive = session_.masked && session_.revealValue;
  const ControlButtonDef controls[] = {
      {"abc", Action::SetLower, 11, false, session_.mode == KeyboardMode::Lower},
      {"ABC", Action::SetUpper, 11, false, session_.mode == KeyboardMode::Upper},
      {"123", Action::SetSymbols, 11, false, session_.mode == KeyboardMode::Symbols},
      {"space", Action::Space, 24, false, false},
      {"back", Action::Backspace, 13, false, false},
      {session_.masked ? (revealActive ? "hide" : "show") : "clear",
       session_.masked ? Action::ToggleMask : Action::Clear, 13, false, revealActive},
      {"save", Action::Save, 12, true, false},
      {"cancel", Action::Cancel, 14, false, false},
  };

  uint16_t totalUnits = 0;
  for (const ControlButtonDef &control : controls) {
    totalUnits += control.units;
  }

  const size_t controlCount = sizeof(controls) / sizeof(controls[0]);
  const int totalGapWidth = static_cast<int>((controlCount - 1) * kKeyboardRowGap);
  const int availableWidth = BoardConfig::DISPLAY_WIDTH - (2 * kKeyboardMarginX) - totalGapWidth;
  int remainingWidth = availableWidth;
  uint16_t x = kKeyboardMarginX;
  const uint16_t y = kKeyboardTopY + static_cast<uint16_t>(3 * rowPitch);

  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButtonDef &control = controls[i];
    int width = remainingWidth;
    if (i + 1 < controlCount) {
      width = (availableWidth * control.units) / totalUnits;
      remainingWidth -= width;
    }

    Button button;
    button.view.label = control.label;
    button.view.x = x;
    button.view.y = y;
    button.view.width = static_cast<uint16_t>(std::max(28, width));
    button.view.height = kKeyboardRowHeight;
    button.view.accent = control.accent;
    button.view.active = control.active;
    button.action = control.action;
    buttons_.push_back(button);

    x = static_cast<uint16_t>(x + button.view.width + kKeyboardRowGap);
  }
}

void TextEntryController::render() {
  if (!session_.active) {
    return;
  }

  const String visibleValue = (session_.masked && !session_.revealValue)
                                  ? maskedValue(session_.value)
                                  : session_.value;

  std::vector<DisplayManager::Button> buttons;
  buttons.reserve(buttons_.size());
  for (const Button &button : buttons_) {
    buttons.push_back(button.view);
  }

  display_.renderTextEntry(session_.title, session_.prompt, visibleValue, session_.helperText,
                           buttons);
}

bool TextEntryController::handleTap(uint16_t x, uint16_t y, uint32_t nowMs) {
  if (!session_.active) {
    return false;
  }

  for (size_t i = 0; i < buttons_.size(); ++i) {
    const DisplayManager::Button &button = buttons_[i].view;
    const uint16_t maxX = button.x + button.width;
    const uint16_t maxY = button.y + button.height;
    if (x < button.x || x > maxX || y < button.y || y > maxY) {
      continue;
    }

    activateButton(i, nowMs);
    return true;
  }

  return false;
}

void TextEntryController::activateButton(size_t buttonIndex, uint32_t nowMs) {
  if (buttonIndex >= buttons_.size()) {
    return;
  }

  Button &button = buttons_[buttonIndex];
  switch (button.action) {
    case Action::Insert:
      if (session_.value.length() < session_.maxLength) {
        session_.value += button.payload;
      }
      break;
    case Action::SetLower:
      session_.mode = KeyboardMode::Lower;
      break;
    case Action::SetUpper:
      session_.mode = KeyboardMode::Upper;
      break;
    case Action::SetSymbols:
      session_.mode = KeyboardMode::Symbols;
      break;
    case Action::Space:
      if (session_.value.length() < session_.maxLength) {
        session_.value += ' ';
      }
      break;
    case Action::Backspace:
      if (!session_.value.isEmpty()) {
        session_.value.remove(session_.value.length() - 1);
      }
      break;
    case Action::Clear:
      session_.value = "";
      break;
    case Action::ToggleMask:
      if (session_.masked) {
        session_.revealValue = !session_.revealValue;
      }
      break;
    case Action::Save:
      if (onSubmit_) {
        onSubmit_(nowMs);
      }
      return;
    case Action::Cancel:
      if (onCancel_) {
        onCancel_();
      }
      return;
  }

  rebuildButtons();
  render();
}
