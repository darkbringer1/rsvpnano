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
  PowerSaving,  // AMOLED deep standby: screen + touch off, wakes only on PWR tap.
  Sleeping,
};
