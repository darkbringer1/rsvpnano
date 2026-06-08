#pragma once

enum class AppState {
  Booting,
  Paused,
  Playing,
  Finished,
  Menu,
  CompanionSync,
  UsbTransfer,
  Standby,
  PowerSaving,  // AMOLED screen-off standby; may escalate to PMU shutdown on battery.
  Sleeping,
};
