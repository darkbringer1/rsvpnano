#include "timer/FocusTimer.h"

#include <math.h>

#include "board/BoardConfig.h"

namespace {

#if defined(BOARD_AMOLED_18)
constexpr uint32_t kOrientationStableMs = 1400;  // less twitchy: tilting to read won't trigger
#else
constexpr uint32_t kOrientationStableMs = 700;
#endif
// After any FSM transition, ignore orientation intents for a moment so the
// gesture that caused the change does not immediately re-trigger.
constexpr uint32_t kOrientationGraceMs = 1200;
constexpr uint32_t kOrientationResumeStableMs = 450;

// Setup field value ranges.
constexpr uint16_t kWorkMinMin = 5;
constexpr uint16_t kWorkMinMax = 120;
constexpr uint16_t kWorkMinStep = 5;
constexpr uint16_t kBreakMinMin = 1;
constexpr uint16_t kBreakMinMax = 30;
constexpr uint16_t kBreakMinStep = 1;
constexpr uint8_t kRoundsMin = 1;
constexpr uint8_t kRoundsMax = 8;
constexpr uint16_t kLongBreakMinMin = 5;
constexpr uint16_t kLongBreakMinMax = 60;
constexpr uint16_t kLongBreakMinStep = 5;

constexpr float kSideAxisThreshold = 0.78f;
constexpr float kCrossAxisLimit = 0.42f;
constexpr float kFlatAxisThreshold = 0.84f;

uint16_t clampStep(uint16_t value, int direction, uint16_t step, uint16_t lo, uint16_t hi) {
  int next = static_cast<int>(value) + direction * static_cast<int>(step);
  if (next < static_cast<int>(lo)) {
    next = lo;
  } else if (next > static_cast<int>(hi)) {
    next = hi;
  }
  return static_cast<uint16_t>(next);
}

}  // namespace

bool FocusTimer::begin(MotionSensor *motion) {
  motion_ = motion;
  for (uint8_t i = 0; i < kPresetCount; ++i) {
    presets_[i] = defaultConfig(static_cast<Preset>(i));
  }
  return imuAvailable();
}

void FocusTimer::open(uint32_t nowMs) {
  clearSession();
  resetOrientationStability();
  state_ = State::PresetSelect;
  stateStartedMs_ = nowMs;
}

void FocusTimer::update(uint32_t nowMs) {
  if (imuAvailable()) {
    updateOrientation(nowMs);
  }

  switch (state_) {
    case State::WorkRunning:
    case State::BreakRunning:
      if (timerExpired(nowMs)) {
        completePhase(nowMs);
      }
      break;

    case State::Complete:
    case State::Cancelled:
      // Transient: App observes these and returns to the preset picker.
      break;

    default:
      break;
  }

  applyOrientation(nowMs);
}

// ----- Preset selection -----

const char *FocusTimer::presetLabel(Preset preset) {
  switch (preset) {
    case Preset::Classic:
      return "Classic";
    case Preset::Deep:
      return "Deep";
    case Preset::Quick:
      return "Quick";
    case Preset::Custom:
      return "Custom";
    case Preset::None:
    default:
      return "";
  }
}

void FocusTimer::selectPreset(Preset preset, uint32_t nowMs) {
  if (preset == Preset::None || static_cast<uint8_t>(preset) >= kPresetCount) {
    return;
  }
  preset_ = preset;
  cfg_ = presets_[static_cast<uint8_t>(preset)];
  currentRound_ = 0;
  completedWorkBlocks_ = 0;
  completedBreakBlocks_ = 0;
  selectedField_ = Field::Work;
  transitionTo(State::Setup, nowMs);
}

// ----- Setup editing -----

void FocusTimer::selectField(Field field) {
  if (static_cast<uint8_t>(field) < kFieldCount) {
    selectedField_ = field;
  }
}

void FocusTimer::nextField() {
  selectedField_ = static_cast<Field>((static_cast<uint8_t>(selectedField_) + 1) % kFieldCount);
}

void FocusTimer::stepField(int direction) {
  if (direction == 0 || state_ != State::Setup) {
    return;
  }
  int next = static_cast<int>(selectedField_) + direction;
  if (next < 0) {
    next = kFieldCount - 1;
  } else if (next >= kFieldCount) {
    next = 0;
  }
  selectedField_ = static_cast<Field>(next);
}

void FocusTimer::stepFieldValue(int direction) {
  if (direction == 0 || state_ != State::Setup) {
    return;
  }
  switch (selectedField_) {
    case Field::Work:
      cfg_.workMin = clampStep(cfg_.workMin, direction, kWorkMinStep, kWorkMinMin, kWorkMinMax);
      break;
    case Field::Break:
      cfg_.breakMin = clampStep(cfg_.breakMin, direction, kBreakMinStep, kBreakMinMin, kBreakMinMax);
      break;
    case Field::Rounds:
      cfg_.rounds = static_cast<uint8_t>(
          clampStep(cfg_.rounds, direction, 1, kRoundsMin, kRoundsMax));
      break;
    case Field::LongBreak:
      cfg_.longBreakMin =
          clampStep(cfg_.longBreakMin, direction, kLongBreakMinStep, kLongBreakMinMin, kLongBreakMinMax);
      break;
    case Field::Begin:
      break;
  }
  // Persist edits back into the in-RAM preset so they survive re-selection.
  if (static_cast<uint8_t>(preset_) < kPresetCount) {
    presets_[static_cast<uint8_t>(preset_)] = cfg_;
  }
}

void FocusTimer::beginSession(uint32_t nowMs) {
  if (cfg_.rounds < kRoundsMin) {
    cfg_.rounds = kRoundsMin;
  }
  currentRound_ = 1;
  completedWorkBlocks_ = 0;
  completedBreakBlocks_ = 0;
  startPhase(Phase::Work, nowMs, minutesToMs(cfg_.workMin));
  transitionTo(State::WorkRunning, nowMs);
}

// ----- Running controls -----

void FocusTimer::tap(uint32_t nowMs) {
  switch (state_) {
    case State::Setup:
      if (selectedField_ == Field::Begin) {
        beginSession(nowMs);
      }
      break;
    case State::WorkRunning:
    case State::BreakRunning:
      pauseTimer(nowMs);
      break;
    case State::WorkPaused:
    case State::BreakPaused:
      resumeTimer(nowMs);
      break;
    case State::WaitWorkStart:
      ++currentRound_;
      startPhase(Phase::Work, nowMs, minutesToMs(cfg_.workMin));
      transitionTo(State::WorkRunning, nowMs);
      break;
    case State::Complete:
    case State::Cancelled:
      // Handled by App (returns to presets).
      break;
    default:
      break;
  }
}

void FocusTimer::swipe(int direction, uint32_t nowMs) {
  switch (state_) {
    case State::Setup:
      stepFieldValue(direction);
      break;
    case State::WorkRunning:
    case State::BreakRunning:
    case State::WorkPaused:
    case State::BreakPaused:
      // Skip the current phase as if it had completed.
      completePhase(nowMs);
      break;
    default:
      break;
  }
}

void FocusTimer::hold(uint32_t nowMs) {
  switch (state_) {
    case State::WorkRunning:
    case State::BreakRunning:
    case State::WorkPaused:
    case State::BreakPaused:
    case State::WaitWorkStart:
      cancel(nowMs);
      break;
    case State::Setup:
      abandon(nowMs);
      break;
    default:
      break;
  }
}

void FocusTimer::abandon(uint32_t nowMs) {
  clearSession();
  resetOrientationStability();
  state_ = State::PresetSelect;
  stateStartedMs_ = nowMs;
}

// ----- Getters -----

bool FocusTimer::isRunning() const {
  return state_ == State::WorkRunning || state_ == State::BreakRunning;
}

bool FocusTimer::isPaused() const {
  return state_ == State::WorkPaused || state_ == State::BreakPaused;
}

uint32_t FocusTimer::remainingMs(uint32_t nowMs) const {
  if (isPaused()) {
    return pausedRemainingMs_;
  }
  if (!timerRunning_) {
    return 0;
  }
  const uint32_t elapsed = nowMs - timerStartedMs_;
  return (elapsed >= timerDurationMs_) ? 0 : (timerDurationMs_ - elapsed);
}

uint8_t FocusTimer::progressPercent(uint32_t nowMs) const {
  if (timerDurationMs_ == 0) {
    return 0;
  }
  uint32_t elapsed;
  if (isPaused()) {
    elapsed = timerDurationMs_ - pausedRemainingMs_;
  } else if (timerRunning_) {
    elapsed = nowMs - timerStartedMs_;
  } else {
    return 0;
  }
  const uint32_t clamped = (elapsed >= timerDurationMs_) ? timerDurationMs_ : elapsed;
  return static_cast<uint8_t>((clamped * 100U) / timerDurationMs_);
}

// ----- Per-preset persistence -----

FocusTimer::Config FocusTimer::presetConfig(Preset preset) const {
  const uint8_t i = static_cast<uint8_t>(preset);
  return i < kPresetCount ? presets_[i] : defaultConfig(Preset::Classic);
}

void FocusTimer::setPresetConfig(Preset preset, const Config &cfg) {
  const uint8_t i = static_cast<uint8_t>(preset);
  if (i >= kPresetCount) {
    return;
  }
  Config c = cfg;
  c.workMin = clampStep(c.workMin == 0 ? kWorkMinMin : c.workMin, 0, kWorkMinStep, kWorkMinMin, kWorkMinMax);
  c.breakMin = clampStep(c.breakMin == 0 ? kBreakMinMin : c.breakMin, 0, kBreakMinStep, kBreakMinMin, kBreakMinMax);
  if (c.rounds < kRoundsMin) c.rounds = kRoundsMin;
  if (c.rounds > kRoundsMax) c.rounds = kRoundsMax;
  c.longBreakMin = clampStep(c.longBreakMin == 0 ? kLongBreakMinMin : c.longBreakMin, 0,
                             kLongBreakMinStep, kLongBreakMinMin, kLongBreakMinMax);
  presets_[i] = c;
}

FocusTimer::Cue FocusTimer::consumeCue() {
  const Cue cue = pendingCue_;
  pendingCue_ = Cue::None;
  return cue;
}

// ----- Timer mechanics -----

void FocusTimer::startPhase(Phase phase, uint32_t nowMs, uint32_t durationMs) {
  phase_ = phase;
  timerStartedMs_ = nowMs;
  timerDurationMs_ = durationMs;
  timerRunning_ = true;
  pausedRemainingMs_ = 0;
  pausedElapsedMs_ = 0;
  pendingCue_ = Cue::Start;
}

void FocusTimer::pauseTimer(uint32_t nowMs) {
  if (!timerRunning_) {
    return;
  }
  pausedRemainingMs_ = remainingMs(nowMs);
  pausedElapsedMs_ =
      timerDurationMs_ > pausedRemainingMs_ ? timerDurationMs_ - pausedRemainingMs_ : 0;
  timerRunning_ = false;
  pendingCue_ = Cue::Pause;
  transitionTo(phase_ == Phase::Work ? State::WorkPaused : State::BreakPaused, nowMs);
}

void FocusTimer::resumeTimer(uint32_t nowMs) {
  if (pausedRemainingMs_ == 0) {
    pausedRemainingMs_ = minutesToMs(phase_ == Phase::Work ? cfg_.workMin : cfg_.breakMin);
    timerDurationMs_ = pausedRemainingMs_;
    pausedElapsedMs_ = 0;
  }
  timerStartedMs_ = nowMs - pausedElapsedMs_;
  timerRunning_ = true;
  pausedRemainingMs_ = 0;
  pausedElapsedMs_ = 0;
  pendingCue_ = Cue::Resume;
  transitionTo(phase_ == Phase::Work ? State::WorkRunning : State::BreakRunning, nowMs);
}

void FocusTimer::completePhase(uint32_t nowMs) {
  const Phase finished = phase_;
  timerRunning_ = false;
  pausedRemainingMs_ = 0;
  pausedElapsedMs_ = 0;

  if (finished == Phase::Work) {
    ++completedWorkBlocks_;
    // Breaks auto-start. Last round earns the long break.
    if (currentRound_ >= cfg_.rounds) {
      startPhase(Phase::LongBreak, nowMs, minutesToMs(cfg_.longBreakMin));
    } else {
      startPhase(Phase::ShortBreak, nowMs, minutesToMs(cfg_.breakMin));
    }
    pendingCue_ = Cue::WorkComplete;  // override the Start cue startPhase set
    transitionTo(State::BreakRunning, nowMs);
    return;
  }

  // A break finished.
  ++completedBreakBlocks_;
  if (finished == Phase::LongBreak) {
    phase_ = Phase::None;
    pendingCue_ = Cue::SessionComplete;
    transitionTo(State::Complete, nowMs);
  } else {
    phase_ = Phase::None;
    pendingCue_ = Cue::BreakComplete;
    transitionTo(State::WaitWorkStart, nowMs);
  }
}

void FocusTimer::cancel(uint32_t nowMs) {
  timerRunning_ = false;
  pausedRemainingMs_ = 0;
  pausedElapsedMs_ = 0;
  phase_ = Phase::None;
  pendingCue_ = Cue::Cancelled;
  transitionTo(State::Cancelled, nowMs);
}

void FocusTimer::clearSession() {
  phase_ = Phase::None;
  currentRound_ = 0;
  completedWorkBlocks_ = 0;
  completedBreakBlocks_ = 0;
  timerStartedMs_ = 0;
  timerDurationMs_ = 0;
  pausedRemainingMs_ = 0;
  pausedElapsedMs_ = 0;
  timerRunning_ = false;
  pendingCue_ = Cue::None;
  // preset_ / cfg_ / presets_ intentionally preserved.
}

void FocusTimer::transitionTo(State next, uint32_t nowMs) {
  state_ = next;
  stateStartedMs_ = nowMs;
}

bool FocusTimer::timerExpired(uint32_t nowMs) const {
  return timerRunning_ && (nowMs - timerStartedMs_ >= timerDurationMs_);
}

uint32_t FocusTimer::minutesToMs(uint16_t minutes) {
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

FocusTimer::Config FocusTimer::defaultConfig(Preset preset) {
  switch (preset) {
    case Preset::Classic:
      return Config{25, 5, 4, 15};
    case Preset::Deep:
      return Config{50, 10, 3, 20};
    case Preset::Quick:
      return Config{15, 3, 4, 15};
    case Preset::Custom:
      return Config{30, 5, 4, 20};
    default:
      return Config{25, 5, 4, 15};
  }
}

uint8_t FocusTimer::presetIdx() const {
  const uint8_t i = static_cast<uint8_t>(preset_);
  return i < kPresetCount ? i : 0;
}

// ----- Orientation layer -----

void FocusTimer::applyOrientation(uint32_t nowMs) {
  if (!imuAvailable() || !orientationArmed(nowMs)) {
    return;
  }
  const uint32_t resumeStableMs =
      (state_ == State::WorkPaused || state_ == State::BreakPaused ||
       state_ == State::WaitWorkStart)
          ? kOrientationResumeStableMs
          : kOrientationStableMs;
  const bool orientationStableEnough =
      candidateOrientation_ == stableOrientation_ && nowMs - candidateSinceMs_ >= resumeStableMs;

  switch (state_) {
    case State::WorkRunning:
    case State::BreakRunning:
      // Lay the device flat (face down on a desk) to pause and step away.
      if (stableOrientation_ == OrientationState::Flat) {
        pauseTimer(nowMs);
      }
      break;
    case State::WorkPaused:
    case State::BreakPaused:
      // Stand it back up on a short edge to resume.
      if (stableOrientation_ == OrientationState::Edge && orientationStableEnough) {
        resumeTimer(nowMs);
      }
      break;
    case State::WaitWorkStart:
      // Stand on a short edge to start the next work block.
      if (stableOrientation_ == OrientationState::Edge && orientationStableEnough) {
        ++currentRound_;
        startPhase(Phase::Work, nowMs, minutesToMs(cfg_.workMin));
        transitionTo(State::WorkRunning, nowMs);
      }
      break;
    default:
      break;
  }
}

bool FocusTimer::orientationArmed(uint32_t nowMs) const {
  return (nowMs - stateStartedMs_) >= kOrientationGraceMs;
}

FocusTimer::OrientationState FocusTimer::classify(float x, float y, float z) const {
  if (fabsf(z) >= kFlatAxisThreshold && fabsf(x) <= 0.30f && fabsf(y) <= 0.30f) {
    return OrientationState::Flat;
  }
  if (fabsf(x) >= kSideAxisThreshold && fabsf(y) <= kCrossAxisLimit &&
      fabsf(z) <= kCrossAxisLimit) {
    return OrientationState::Edge;  // standing on a short side (either end)
  }
  if (fabsf(y) >= kSideAxisThreshold && fabsf(x) <= kCrossAxisLimit &&
      fabsf(z) <= kCrossAxisLimit) {
    return OrientationState::Other;  // long side upright
  }
  return OrientationState::Other;
}

// ----- Orientation (reads the shared MotionSensor) -----

void FocusTimer::updateOrientation(uint32_t nowMs) {
  if (!imuAvailable()) {
    rawOrientation_ = OrientationState::Unknown;
    stableOrientation_ = OrientationState::Unknown;
    return;
  }

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!motion_->readAccel(x, y, z)) {
    return;
  }

  rawOrientation_ = classify(x, y, z);
  if (rawOrientation_ != candidateOrientation_) {
    candidateOrientation_ = rawOrientation_;
    candidateSinceMs_ = nowMs;
    return;
  }
  const uint32_t stableMs =
      ((state_ == State::WorkPaused || state_ == State::BreakPaused ||
        state_ == State::WaitWorkStart) &&
       candidateOrientation_ == OrientationState::Edge)
          ? kOrientationResumeStableMs
          : kOrientationStableMs;
  if ((nowMs - candidateSinceMs_) >= stableMs) {
    stableOrientation_ = candidateOrientation_;
  }
}

void FocusTimer::resetOrientationStability() {
  rawOrientation_ = OrientationState::Unknown;
  stableOrientation_ = OrientationState::Unknown;
  candidateOrientation_ = OrientationState::Unknown;
  candidateSinceMs_ = 0;
}
