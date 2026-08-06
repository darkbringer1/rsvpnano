#include <Arduino.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "app/App.h"
#include "board/BoardConfig.h"
#include "util/PerfProbe.h"

App app;

void setup() {
  // Capture why we (re)booted before anything else can change it. This is key to
  // debugging the power-off self-reopen: distinguishes a fresh power-on from a
  // deep-sleep wake (our ext0 fallback) or a brownout.
  const int resetReason = static_cast<int>(esp_reset_reason());
  const int wakeCause = static_cast<int>(esp_sleep_get_wakeup_cause());

  Serial.begin(115200);
  esp_log_level_set("*", ESP_LOG_INFO);
  delay(50);
  BoardConfig::begin();
  const uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) {
    delay(10);
  }
  Serial.println("[main] app setup");
  app.setBootReason(resetReason, wakeCause);
  app.begin();
}

void loop() {
  const uint32_t now = millis();
  {
    RSVP_PERF_SCOPE(perf::kAppLoop);
    app.update(now);
  }
  // Reported here rather than inside update() because update() has many early
  // returns (standby, power-saving, battery overlay) that would skip the report.
  perf::report(now);
  // Yield to the RTOS idle task so the CPU is not 100% busy-spinning.
  // All timing in update() is millis()-based so 1 ms is imperceptible.
  delay(1);
}
