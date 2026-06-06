#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

#include "display/DisplayManager.h"

// Self-contained on-screen keyboard / text-entry modal. Owns the keyboard
// layout, the edit buffer, mode (lower/upper/symbols), and masking. It is
// purpose-agnostic: when the user taps Save or Cancel it invokes the handlers
// supplied by the caller, which decide what to do with the collected value
// (e.g. the Wi-Fi password flow persists it and returns to Wi-Fi settings).
class TextEntryController {
 public:
  // nowMs is forwarded so the submit handler can drive time-based UI.
  using SubmitHandler = std::function<void(uint32_t nowMs)>;
  using CancelHandler = std::function<void()>;

  TextEntryController(DisplayManager &display, SubmitHandler onSubmit, CancelHandler onCancel);

  void open(const String &title, const String &prompt, const String &helperText,
            const String &initialValue, const String &contextValue, bool masked,
            size_t maxLength);
  bool active() const { return session_.active; }
  const String &value() const { return session_.value; }
  const String &contextValue() const { return session_.contextValue; }

  void render();
  bool handleTap(uint16_t x, uint16_t y, uint32_t nowMs);
  void close();  // clear the session and buttons

 private:
  enum class KeyboardMode : uint8_t {
    Lower = 0,
    Upper = 1,
    Symbols = 2,
  };

  enum class Action : uint8_t {
    Insert,
    SetLower,
    SetUpper,
    SetSymbols,
    Space,
    Backspace,
    Clear,
    ToggleMask,
    Save,
    Cancel,
  };

  struct Button {
    DisplayManager::Button view;
    Action action = Action::Insert;
    String payload;
  };

  struct Session {
    bool active = false;
    KeyboardMode mode = KeyboardMode::Lower;
    String title;
    String prompt;
    String helperText;
    String value;
    String contextValue;
    size_t maxLength = 63;
    bool masked = false;
    bool revealValue = false;
  };

  void rebuildButtons();
  void activateButton(size_t buttonIndex, uint32_t nowMs);

  DisplayManager &display_;
  SubmitHandler onSubmit_;
  CancelHandler onCancel_;
  Session session_;
  std::vector<Button> buttons_;
};
