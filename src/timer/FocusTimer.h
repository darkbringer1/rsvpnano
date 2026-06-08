#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "board/BoardConfig.h"
#include "sensor/MotionSensor.h"

// Touch-first Pomodoro focus timer.
//
// The state machine is the single source of truth and is driven by the touch
// forwarders (tap / swipe / hold) plus timer expiry. The IMU/orientation layer
// is optional: when present it only *injects* the same high-level intents
// (pause, resume, start-next-work) into the FSM. The timer works fully without
// an IMU.
class FocusTimer {
 public:
  enum class Preset : uint8_t {
    Classic = 0,  // 25 / 5, 4 rounds, 15 long
    Deep,         // 50 / 10, 3 rounds, 20 long
    Quick,        // 15 / 3, 4 rounds, 15 long
    Custom,       // user-defined
    None = 0xFF,
  };
  static constexpr uint8_t kPresetCount = 4;

  enum class Phase : uint8_t {
    None = 0,
    Work,
    ShortBreak,
    LongBreak,
  };

  enum class State : uint8_t {
    PresetSelect = 0,  // choosing a preset (menu-driven)
    Setup,             // editing the chosen preset, tap Begin to start
    WorkRunning,
    WorkPaused,
    BreakRunning,
    BreakPaused,
    WaitWorkStart,  // a break finished; tap (or stand on edge) to start work
    Complete,       // all rounds done
    Cancelled,      // session abandoned (transient, App returns to presets)
  };

  enum class Cue : uint8_t {
    None = 0,
    Start,
    Pause,
    Resume,
    WorkComplete,
    BreakComplete,
    SessionComplete,
    Cancelled,
  };

  // Editable fields on the Setup screen. Begin is the action row.
  enum class Field : uint8_t {
    Work = 0,
    Break,
    Rounds,
    LongBreak,
    Begin,
  };
  static constexpr uint8_t kFieldCount = 5;

  struct Config {
    uint16_t workMin;
    uint16_t breakMin;
    uint8_t rounds;
    uint16_t longBreakMin;
  };

  bool begin(MotionSensor *motion);
  void open(uint32_t nowMs);  // -> PresetSelect

  void update(uint32_t nowMs);

  // ----- Preset selection (PresetSelect) -----
  static const char *presetLabel(Preset preset);
  void selectPreset(Preset preset, uint32_t nowMs);  // -> Setup

  // ----- Setup editing -----
  Field selectedField() const { return selectedField_; }
  void selectField(Field field);
  void nextField();                    // cycle field selection
  void stepField(int direction);       // change selected setup row
  void stepFieldValue(int direction);  // change value of the selected field
  void beginSession(uint32_t nowMs);   // start first work block

  // ----- Running controls (forwarded from touch and/or orientation) -----
  void tap(uint32_t nowMs);                   // pause/resume, start-next-work, begin-again
  void swipe(int direction, uint32_t nowMs);  // running: skip phase; setup: change value
  void hold(uint32_t nowMs);                  // cancel session
  void abandon(uint32_t nowMs);               // back to PresetSelect

  // ----- Getters -----
  State state() const { return state_; }
  Phase phase() const { return phase_; }
  Preset preset() const { return preset_; }
  bool isRunning() const;
  bool isPaused() const;
  bool imuAvailable() const { return motion_ != nullptr && motion_->available(); }
  uint8_t currentRound() const { return currentRound_; }
  uint8_t totalRounds() const { return cfg_.rounds; }
  const Config &config() const { return cfg_; }
  bool isLongBreak() const { return phase_ == Phase::LongBreak; }
  uint32_t remainingMs(uint32_t nowMs) const;
  uint8_t progressPercent(uint32_t nowMs) const;

  // ----- Per-preset config persistence (App <-> Preferences) -----
  Config presetConfig(Preset preset) const;
  void setPresetConfig(Preset preset, const Config &cfg);

  Cue consumeCue();

 private:
  enum class OrientationState : uint8_t {
    Edge = 0,  // standing on a short side (either A or B)
    Flat,      // lying face up or down on a surface
    Other,     // upright long edge / held / unknown
    Unknown,
  };

  enum class PauseSource : uint8_t {
    None = 0,
    Touch,
    Orientation,
  };

  // ----- Timer mechanics -----
  void startPhase(Phase phase, uint32_t nowMs, uint32_t durationMs);
  void pauseTimer(uint32_t nowMs, PauseSource source);
  void resumeTimer(uint32_t nowMs);
  void completePhase(uint32_t nowMs);  // advance to next phase / wait / complete
  void cancel(uint32_t nowMs);
  void clearSession();
  void transitionTo(State next, uint32_t nowMs);
  bool timerExpired(uint32_t nowMs) const;
  static uint32_t minutesToMs(uint16_t minutes);
  static Config defaultConfig(Preset preset);
  uint8_t presetIdx() const;

  // ----- Orientation layer (reads the shared MotionSensor) -----
  void applyOrientation(uint32_t nowMs);
  bool orientationArmed(uint32_t nowMs) const;
  OrientationState classify(float x, float y, float z) const;
  void updateOrientation(uint32_t nowMs);
  void resetOrientationStability();

  // ----- State -----
  State state_ = State::PresetSelect;
  Phase phase_ = Phase::None;
  Preset preset_ = Preset::None;
  Field selectedField_ = Field::Work;
  Config cfg_ = {25, 5, 4, 15};
  Config presets_[kPresetCount] = {};
  uint8_t currentRound_ = 0;
  uint8_t completedWorkBlocks_ = 0;
  uint8_t completedBreakBlocks_ = 0;

  uint32_t stateStartedMs_ = 0;
  uint32_t timerStartedMs_ = 0;
  uint32_t timerDurationMs_ = 0;
  uint32_t pausedRemainingMs_ = 0;
  uint32_t pausedElapsedMs_ = 0;
  bool timerRunning_ = false;
  PauseSource pauseSource_ = PauseSource::None;

  Cue pendingCue_ = Cue::None;

  // ----- IMU / orientation -----
  MotionSensor *motion_ = nullptr;
  OrientationState rawOrientation_ = OrientationState::Unknown;
  OrientationState stableOrientation_ = OrientationState::Unknown;
  OrientationState candidateOrientation_ = OrientationState::Unknown;
  uint32_t candidateSinceMs_ = 0;
};
