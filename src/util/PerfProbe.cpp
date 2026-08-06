#include "util/PerfProbe.h"

#if defined(RSVP_PERF_PROBE) && RSVP_PERF_PROBE

#include <Arduino.h>
#include <esp_timer.h>

namespace perf {
namespace {

constexpr uint32_t kReportIntervalMs = 3000;

struct Accumulator {
  uint32_t count = 0;
  uint64_t totalUs = 0;
  uint32_t maxUs = 0;
};

// Index-aligned with the Probe enum.
const char *const kProbeNames[kProbeCount] = {
    "flush.convert", "flush.push",  "render.word",  "render.menu", "app.loop",
    "ph.battery",    "ph.orient",   "ph.touch",     "ph.focus",    "ph.timeEst",
    "ph.buttons",    "ph.reader",   "sd.wordWindow",
};

Accumulator gAccumulators[kProbeCount];
uint32_t gLastReportMs = 0;

}  // namespace

int64_t nowUs() { return esp_timer_get_time(); }

void add(Probe probe, uint32_t microseconds) {
  if (probe >= kProbeCount) {
    return;
  }
  Accumulator &accumulator = gAccumulators[probe];
  accumulator.count++;
  accumulator.totalUs += microseconds;
  if (microseconds > accumulator.maxUs) {
    accumulator.maxUs = microseconds;
  }
}

void reset() {
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    gAccumulators[i] = Accumulator();
  }
}

void report(uint32_t nowMs) {
  const uint32_t windowMs = nowMs - gLastReportMs;
  if (windowMs < kReportIntervalMs) {
    return;
  }
  gLastReportMs = nowMs;

  Serial.printf("[perf] cpu=%uMHz window=%lums\n", getCpuFrequencyMhz(),
                static_cast<unsigned long>(windowMs));
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    const Accumulator &accumulator = gAccumulators[i];
    if (accumulator.count == 0) {
      continue;
    }
    const uint32_t averageUs = static_cast<uint32_t>(accumulator.totalUs / accumulator.count);
    // Share of wall-clock time spent inside this probe over the window.
    const uint32_t loadPercent =
        static_cast<uint32_t>(accumulator.totalUs * 100ULL / (windowMs * 1000ULL));
    Serial.printf("[perf]   %-14s n=%-5lu avg=%-7luus max=%-7luus load=%lu%%\n", kProbeNames[i],
                  static_cast<unsigned long>(accumulator.count),
                  static_cast<unsigned long>(averageUs),
                  static_cast<unsigned long>(accumulator.maxUs),
                  static_cast<unsigned long>(loadPercent));
  }
  reset();
}

}  // namespace perf

#endif  // RSVP_PERF_PROBE
