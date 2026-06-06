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
  Sleeping,
};
