#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

class DisplayManager;

// Owns the standby screensaver animations (Conway's Life, maze carving, Voronoi
// shimmer) and the screen-off mode. App decides *when* to run it (tied to
// AppState::Standby); this class owns *what* it draws and all of its state.
class StandbyScreensaver {
 public:
  enum class Mode : uint8_t {
    Life = 0,
    Maze = 2,
    Voronoi = 3,
    ScreenOff = 6,
  };

  StandbyScreensaver(DisplayManager &display,
                     std::function<uint32_t()> readerIndexProvider,
                     std::function<uint8_t()> batteryPercentProvider);

  void setMode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  Mode cycleMode();  // advance Life -> Maze -> Voronoi -> ScreenOff -> Life, returns the new mode
  String modeLabel() const;
  static Mode modeFromValue(uint8_t value);

  void seed(uint32_t nowMs);                        // reseed the active animation
  void update(uint32_t nowMs, bool force = false);  // step + render one frame (caller gates on Standby)

  void resetFrameTimer() { lastFrameMs_ = 0; }
  bool screenOffActive() const { return screenOffActive_; }
  void wakeIfScreenOff();  // wake the panel and clear the screen-off flag if it was set
  void clearScreenOff() { screenOffActive_ = false; }

 private:
  void step(uint32_t nowMs);
  void seedLife(uint32_t nowMs);
  void stepLife();
  void seedMaze(uint32_t nowMs);
  void stepMaze();
  void seedVoronoi(uint32_t nowMs);
  void stepVoronoi();
  void renderVoronoi();
  void seedScreenOff(uint32_t nowMs);

  DisplayManager &display_;
  std::function<uint32_t()> readerIndex_;
  std::function<uint8_t()> batteryPercent_;

  Mode mode_ = Mode::Life;
  std::vector<uint32_t> lifeCells_;
  std::vector<uint32_t> lifeNextCells_;
  std::vector<uint32_t> dimCells_;
  std::vector<uint8_t> mazeVisited_;
  std::vector<uint16_t> mazeStack_;
  std::vector<int16_t> voronoiX_;
  std::vector<int16_t> voronoiY_;
  std::vector<int16_t> voronoiDx_;
  std::vector<int16_t> voronoiDy_;
  uint32_t generation_ = 0;
  uint32_t rng_ = 1;
  uint32_t lastFrameMs_ = 0;
  bool screenOffActive_ = false;
};
