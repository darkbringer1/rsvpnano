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
  PowerSaving,  // AMOLED deep-sleep standby path; BOOT wakes from GPIO0.
  Sleeping,
};
