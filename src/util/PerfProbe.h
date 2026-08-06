#pragma once

#include <stdint.h>

// Lightweight timing probes for the render/loop hot path.
//
// Compiled out entirely unless RSVP_PERF_PROBE=1 (see the `amoled_perf` env in
// platformio.ini), so the shipping firmware pays nothing: nowUs() folds to a
// constant 0 and every accumulator becomes dead code.
//
// Timing uses esp_timer_get_time() rather than the CPU cycle counter because
// App::applyStateCpuFrequency() rescales the clock per state, which would make
// cycle counts incomparable across Playing/Paused/Menu.

namespace perf {

enum Probe : uint8_t {
  kFlushConvert = 0,  // virtualFrame_ -> txBuffer_ pixel transform
  kFlushPush,         // txBuffer_ -> panel over QSPI
  kRenderWord,        // full reader-word render: layout + draw + flush
  kRenderMenu,        // full menu render
  kAppLoop,           // one App::update() tick
  // App::update() phase breakdown, to locate per-tick cost outside rendering.
  kPhaseBattery,      // updateBatteryStatus
  kPhaseOrientation,  // updateAutoOrientation (IMU over I2C, ungated)
  kPhaseTouch,        // handleTouch
  kPhaseFocusTimer,   // updateFocusTimer
  kPhaseTimeEstimate, // TimeEstimateEngine::update
  kPhaseButtons,      // dispatchButtons
  kProbeCount,
};

#if defined(RSVP_PERF_PROBE) && RSVP_PERF_PROBE

int64_t nowUs();
void add(Probe probe, uint32_t microseconds);
void reset();
// Prints the accumulated table every kReportIntervalMs, then clears the window.
void report(uint32_t nowMs);

// RAII timer: records the enclosing scope's duration into `probe`.
class Scope {
 public:
  explicit Scope(Probe probe) : probe_(probe), startUs_(nowUs()) {}
  ~Scope() { add(probe_, static_cast<uint32_t>(nowUs() - startUs_)); }

 private:
  Probe probe_;
  int64_t startUs_;
};

#else

inline int64_t nowUs() { return 0; }
inline void add(Probe, uint32_t) {}
inline void reset() {}
inline void report(uint32_t) {}

#endif

}  // namespace perf

#if defined(RSVP_PERF_PROBE) && RSVP_PERF_PROBE
#define RSVP_PERF_CONCAT_INNER(a, b) a##b
#define RSVP_PERF_CONCAT(a, b) RSVP_PERF_CONCAT_INNER(a, b)
#define RSVP_PERF_SCOPE(probe) ::perf::Scope RSVP_PERF_CONCAT(perfScope_, __LINE__)(probe)
#else
#define RSVP_PERF_SCOPE(probe) ((void)0)
#endif
