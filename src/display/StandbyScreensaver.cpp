#include "display/StandbyScreensaver.h"

#include <algorithm>

#include "board/BoardConfig.h"
#include "display/DisplayManager.h"

namespace {

constexpr uint32_t kStandbyFrameMs = 160;
constexpr uint16_t kStandbyLifeCellPixels = 2;
constexpr uint16_t kStandbyLifeColumns = BoardConfig::DISPLAY_WIDTH / kStandbyLifeCellPixels;
constexpr uint16_t kStandbyLifeRows = BoardConfig::DISPLAY_HEIGHT / kStandbyLifeCellPixels;

size_t packedLifeWordCount(size_t cellCount) { return (cellCount + 31U) / 32U; }

bool packedLifeCellAlive(const std::vector<uint32_t> &cells, size_t index) {
  const size_t word = index / 32U;
  if (word >= cells.size()) {
    return false;
  }
  return (cells[word] & (1UL << (index % 32U))) != 0;
}

void setPackedLifeCell(std::vector<uint32_t> &cells, size_t index, bool alive) {
  const size_t word = index / 32U;
  if (word >= cells.size()) {
    return;
  }
  const uint32_t mask = 1UL << (index % 32U);
  if (alive) {
    cells[word] |= mask;
  } else {
    cells[word] &= ~mask;
  }
}

struct LifePoint {
  int8_t x;
  int8_t y;
};

void setPackedLifeCellAt(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows, int x,
                         int y, bool alive) {
  if (x < 0 || y < 0 || x >= static_cast<int>(columns) || y >= static_cast<int>(rows)) {
    return;
  }
  setPackedLifeCell(cells, static_cast<size_t>(y) * columns + static_cast<size_t>(x), alive);
}

void clearPackedLifeRect(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows, int x,
                         int y, int width, int height) {
  const int xEnd = std::min(static_cast<int>(columns), x + width);
  const int yEnd = std::min(static_cast<int>(rows), y + height);
  for (int cy = std::max(0, y); cy < yEnd; ++cy) {
    for (int cx = std::max(0, x); cx < xEnd; ++cx) {
      setPackedLifeCellAt(cells, columns, rows, cx, cy, false);
    }
  }
}

void stampPackedLifePattern(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows,
                            const LifePoint *points, size_t pointCount, int originX,
                            int originY) {
  for (size_t i = 0; i < pointCount; ++i) {
    setPackedLifeCellAt(cells, columns, rows, originX + points[i].x, originY + points[i].y, true);
  }
}

void clearAndStampPackedLifePattern(std::vector<uint32_t> &cells, uint16_t columns, uint16_t rows,
                                    const LifePoint *points, size_t pointCount, int originX,
                                    int originY, int width, int height) {
  if (originX < 0 || originY < 0 || originX + width > static_cast<int>(columns) ||
      originY + height > static_cast<int>(rows)) {
    return;
  }
  constexpr int kPatternMargin = 5;
  clearPackedLifeRect(cells, columns, rows, originX - kPatternMargin, originY - kPatternMargin,
                      width + kPatternMargin * 2, height + kPatternMargin * 2);
  stampPackedLifePattern(cells, columns, rows, points, pointCount, originX, originY);
}

constexpr LifePoint kLifeGlider[] = {
    {1, 0},
    {2, 1},
    {0, 2},
    {1, 2},
    {2, 2},
};

constexpr LifePoint kLifeLightweightSpaceship[] = {
    {1, 0}, {4, 0}, {0, 1}, {0, 2}, {4, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3},
};

constexpr LifePoint kLifePentadecathlon[] = {
    {2, 0}, {2, 1}, {1, 2}, {3, 2}, {2, 3}, {2, 4},
    {2, 5}, {2, 6}, {1, 7}, {3, 7}, {2, 8}, {2, 9},
};

constexpr LifePoint kLifePulsar[] = {
    {2, 0},  {3, 0},  {4, 0},  {8, 0},  {9, 0},  {10, 0}, {0, 2},  {5, 2},
    {7, 2},  {12, 2}, {0, 3},  {5, 3},  {7, 3},  {12, 3}, {0, 4},  {5, 4},
    {7, 4},  {12, 4}, {2, 5},  {3, 5},  {4, 5},  {8, 5},  {9, 5},  {10, 5},
    {2, 7},  {3, 7},  {4, 7},  {8, 7},  {9, 7},  {10, 7}, {0, 8},  {5, 8},
    {7, 8},  {12, 8}, {0, 9},  {5, 9},  {7, 9},  {12, 9}, {0, 10}, {5, 10},
    {7, 10}, {12, 10}, {2, 12}, {3, 12}, {4, 12}, {8, 12}, {9, 12}, {10, 12},
};

constexpr LifePoint kLifeGosperGliderGun[] = {
    {24, 0}, {22, 1}, {24, 1}, {12, 2}, {13, 2}, {20, 2}, {21, 2}, {34, 2}, {35, 2},
    {11, 3}, {15, 3}, {20, 3}, {21, 3}, {34, 3}, {35, 3}, {0, 4},  {1, 4},
    {10, 4}, {16, 4}, {20, 4}, {21, 4}, {0, 5},  {1, 5},  {10, 5}, {14, 5},
    {16, 5}, {17, 5}, {22, 5}, {24, 5}, {10, 6}, {16, 6}, {24, 6}, {11, 7},
    {15, 7}, {12, 8}, {13, 8},
};

uint32_t advanceStandbyRng(uint32_t &rng) {
  rng = (rng * 1664525UL) + 1013904223UL;
  return rng;
}

}  // namespace

StandbyScreensaver::StandbyScreensaver(DisplayManager &display,
                                       std::function<uint32_t()> readerIndexProvider,
                                       std::function<uint8_t()> batteryPercentProvider)
    : display_(display),
      readerIndex_(std::move(readerIndexProvider)),
      batteryPercent_(std::move(batteryPercentProvider)) {}

StandbyScreensaver::Mode StandbyScreensaver::cycleMode() {
  switch (mode_) {
    case Mode::Life:
      mode_ = Mode::Maze;
      break;
    case Mode::Maze:
      mode_ = Mode::Voronoi;
      break;
    case Mode::Voronoi:
      mode_ = Mode::ScreenOff;
      break;
    case Mode::ScreenOff:
    default:
      mode_ = Mode::Life;
      break;
  }
  return mode_;
}

String StandbyScreensaver::modeLabel() const {
  switch (mode_) {
    case Mode::Maze:
      return "Maze";
    case Mode::Voronoi:
      return "Voronoi";
    case Mode::ScreenOff:
      return "Screen off";
    case Mode::Life:
    default:
      return "Life";
  }
}

StandbyScreensaver::Mode StandbyScreensaver::modeFromValue(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(Mode::Maze):
      return Mode::Maze;
    case static_cast<uint8_t>(Mode::Voronoi):
      return Mode::Voronoi;
    case static_cast<uint8_t>(Mode::ScreenOff):
      return Mode::ScreenOff;
    case static_cast<uint8_t>(Mode::Life):
    default:
      return Mode::Life;
  }
}

void StandbyScreensaver::wakeIfScreenOff() {
  if (screenOffActive_) {
    display_.wakeFromSleep();
    screenOffActive_ = false;
  }
}

void StandbyScreensaver::seed(uint32_t nowMs) {
  if (mode_ != Mode::ScreenOff && screenOffActive_) {
    display_.wakeFromSleep();
    screenOffActive_ = false;
  }

  switch (mode_) {
    case Mode::Maze:
      seedMaze(nowMs);
      return;
    case Mode::Voronoi:
      seedVoronoi(nowMs);
      return;
    case Mode::ScreenOff:
      seedScreenOff(nowMs);
      return;
    case Mode::Life:
    default:
      seedLife(nowMs);
      return;
  }
}

void StandbyScreensaver::step(uint32_t nowMs) {
  (void)nowMs;
  switch (mode_) {
    case Mode::Maze:
      stepMaze();
      return;
    case Mode::Voronoi:
      stepVoronoi();
      return;
    case Mode::ScreenOff:
      return;
    case Mode::Life:
    default:
      stepLife();
      return;
  }
}

void StandbyScreensaver::seedLife(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  lifeCells_.assign(packedLifeWordCount(cellCount), 0);
  lifeNextCells_.assign(packedLifeWordCount(cellCount), 0);
  dimCells_.clear();
  mazeVisited_.clear();
  mazeStack_.clear();
  voronoiX_.clear();
  voronoiY_.clear();
  voronoiDx_.clear();
  voronoiDy_.clear();
  generation_ = 0;

  rng_ = nowMs ^ micros() ^ (static_cast<uint32_t>(readerIndex_() + 1) * 2654435761UL) ^
         (static_cast<uint32_t>(batteryPercent_()) << 24);
  for (size_t i = 0; i < cellCount; ++i) {
    setPackedLifeCell(lifeCells_, i, (advanceStandbyRng(rng_) >> 24) < 12);
  }

  clearAndStampPackedLifePattern(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifeGosperGliderGun,
                                 sizeof(kLifeGosperGliderGun) / sizeof(kLifeGosperGliderGun[0]),
                                 18, 18, 36, 9);
  clearAndStampPackedLifePattern(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifeGosperGliderGun,
                                 sizeof(kLifeGosperGliderGun) / sizeof(kLifeGosperGliderGun[0]),
                                 static_cast<int>(kStandbyLifeColumns) - 62,
                                 static_cast<int>(kStandbyLifeRows) - 34, 36, 9);
  clearAndStampPackedLifePattern(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifePulsar, sizeof(kLifePulsar) / sizeof(kLifePulsar[0]),
                                 static_cast<int>(kStandbyLifeColumns / 2) - 7,
                                 static_cast<int>(kStandbyLifeRows / 2) - 7, 13, 13);
  clearAndStampPackedLifePattern(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifePentadecathlon,
                                 sizeof(kLifePentadecathlon) / sizeof(kLifePentadecathlon[0]),
                                 static_cast<int>(kStandbyLifeColumns / 3),
                                 static_cast<int>(kStandbyLifeRows) - 42, 5, 10);
  clearAndStampPackedLifePattern(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                 kLifeLightweightSpaceship,
                                 sizeof(kLifeLightweightSpaceship) /
                                     sizeof(kLifeLightweightSpaceship[0]),
                                 static_cast<int>((kStandbyLifeColumns * 2) / 3),
                                 static_cast<int>(kStandbyLifeRows / 3), 5, 4);

  for (uint8_t i = 0; i < 10; ++i) {
    const int x =
        static_cast<int>((advanceStandbyRng(rng_) >> 8) %
                         std::max<uint16_t>(1, kStandbyLifeColumns - 6));
    const int y =
        static_cast<int>((advanceStandbyRng(rng_) >> 8) %
                         std::max<uint16_t>(1, kStandbyLifeRows - 6));
    clearAndStampPackedLifePattern(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                                   kLifeGlider, sizeof(kLifeGlider) / sizeof(kLifeGlider[0]), x,
                                   y, 3, 3);
  }
}

void StandbyScreensaver::stepLife() {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  if (lifeCells_.size() != wordCount || lifeNextCells_.size() != wordCount) {
    seedLife(millis());
    return;
  }

  std::fill(lifeNextCells_.begin(), lifeNextCells_.end(), 0);
  size_t aliveCount = 0;
  for (uint16_t y = 0; y < kStandbyLifeRows; ++y) {
    for (uint16_t x = 0; x < kStandbyLifeColumns; ++x) {
      uint8_t neighbours = 0;
      for (int8_t dy = -1; dy <= 1; ++dy) {
        for (int8_t dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) {
            continue;
          }
          const uint16_t nx =
              static_cast<uint16_t>((static_cast<int>(x) + dx + kStandbyLifeColumns) %
                                    kStandbyLifeColumns);
          const uint16_t ny =
              static_cast<uint16_t>((static_cast<int>(y) + dy + kStandbyLifeRows) %
                                    kStandbyLifeRows);
          neighbours += packedLifeCellAlive(
              lifeCells_, static_cast<size_t>(ny) * kStandbyLifeColumns + nx)
                            ? 1
                            : 0;
        }
      }

      const size_t index = static_cast<size_t>(y) * kStandbyLifeColumns + x;
      const bool alive = packedLifeCellAlive(lifeCells_, index);
      const bool nextAlive = alive ? (neighbours == 2 || neighbours == 3) : (neighbours == 3);
      setPackedLifeCell(lifeNextCells_, index, nextAlive);
      if (nextAlive) {
        ++aliveCount;
      }
    }
  }

  lifeCells_.swap(lifeNextCells_);
  ++generation_;
  if (aliveCount == 0 || aliveCount > (cellCount * 3) / 4) {
    seedLife(millis());
  }
}

void StandbyScreensaver::seedMaze(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const uint16_t mazeColumns = std::max<uint16_t>(1, (kStandbyLifeColumns - 1) / 2);
  const uint16_t mazeRows = std::max<uint16_t>(1, (kStandbyLifeRows - 1) / 2);
  lifeCells_.assign(packedLifeWordCount(cellCount), 0);
  lifeNextCells_.assign(packedLifeWordCount(cellCount), 0);
  dimCells_.clear();
  voronoiX_.clear();
  voronoiY_.clear();
  voronoiDx_.clear();
  voronoiDy_.clear();
  mazeVisited_.assign(static_cast<size_t>(mazeColumns) * mazeRows, 0);
  mazeStack_.clear();
  generation_ = 0;
  rng_ = nowMs ^ micros() ^ (static_cast<uint32_t>(readerIndex_() + 1) * 2246822519UL);

  const uint16_t startX = static_cast<uint16_t>((advanceStandbyRng(rng_) >> 8) % mazeColumns);
  const uint16_t startY = static_cast<uint16_t>((advanceStandbyRng(rng_) >> 8) % mazeRows);
  mazeVisited_[static_cast<size_t>(startY) * mazeColumns + startX] = 1;
  mazeStack_.push_back(static_cast<uint16_t>(startY * mazeColumns + startX));
  setPackedLifeCellAt(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                      static_cast<int>(startX) * 2 + 1, static_cast<int>(startY) * 2 + 1, true);
}

void StandbyScreensaver::stepMaze() {
  const uint16_t mazeColumns = std::max<uint16_t>(1, (kStandbyLifeColumns - 1) / 2);
  const uint16_t mazeRows = std::max<uint16_t>(1, (kStandbyLifeRows - 1) / 2);
  const size_t mazeCellCount = static_cast<size_t>(mazeColumns) * mazeRows;
  if (mazeVisited_.size() != mazeCellCount || mazeStack_.empty()) {
    if (mazeStack_.empty() && generation_ < 600) {
      ++generation_;
      return;
    }
    seedMaze(millis());
    return;
  }

  constexpr uint8_t kMazeStepsPerFrame = 32;
  for (uint8_t step = 0; step < kMazeStepsPerFrame && !mazeStack_.empty(); ++step) {
    const uint16_t current = mazeStack_.back();
    const uint16_t cx = current % mazeColumns;
    const uint16_t cy = current / mazeColumns;
    uint16_t candidates[4];
    uint8_t candidateCount = 0;

    auto addCandidate = [&](int nx, int ny) {
      if (nx < 0 || ny < 0 || nx >= static_cast<int>(mazeColumns) ||
          ny >= static_cast<int>(mazeRows)) {
        return;
      }
      const uint16_t encoded = static_cast<uint16_t>(ny * mazeColumns + nx);
      if (mazeVisited_[encoded] == 0) {
        candidates[candidateCount++] = encoded;
      }
    };

    addCandidate(static_cast<int>(cx) + 1, cy);
    addCandidate(static_cast<int>(cx) - 1, cy);
    addCandidate(cx, static_cast<int>(cy) + 1);
    addCandidate(cx, static_cast<int>(cy) - 1);

    if (candidateCount == 0) {
      mazeStack_.pop_back();
      continue;
    }

    const uint16_t next = candidates[(advanceStandbyRng(rng_) >> 16) % candidateCount];
    const uint16_t nx = next % mazeColumns;
    const uint16_t ny = next / mazeColumns;
    mazeVisited_[next] = 1;
    mazeStack_.push_back(next);

    const int displayCx = static_cast<int>(cx) * 2 + 1;
    const int displayCy = static_cast<int>(cy) * 2 + 1;
    const int displayNx = static_cast<int>(nx) * 2 + 1;
    const int displayNy = static_cast<int>(ny) * 2 + 1;
    setPackedLifeCellAt(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows, displayNx,
                        displayNy, true);
    setPackedLifeCellAt(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows,
                        (displayCx + displayNx) / 2, (displayCy + displayNy) / 2, true);
  }

  if (mazeStack_.empty()) {
    generation_ = 0;
  } else {
    ++generation_;
  }
}

void StandbyScreensaver::seedVoronoi(uint32_t nowMs) {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  lifeCells_.assign(wordCount, 0);
  lifeNextCells_.assign(wordCount, 0);
  dimCells_.assign(wordCount, 0);
  mazeVisited_.clear();
  mazeStack_.clear();
  generation_ = 0;
  rng_ = nowMs ^ micros() ^ (static_cast<uint32_t>(readerIndex_() + 1) * 3266489917UL) ^
         0x51a7f00dUL;

  constexpr size_t kVoronoiSiteCount = 15;
  voronoiX_.assign(kVoronoiSiteCount, 0);
  voronoiY_.assign(kVoronoiSiteCount, 0);
  voronoiDx_.assign(kVoronoiSiteCount, 0);
  voronoiDy_.assign(kVoronoiSiteCount, 0);
  for (size_t i = 0; i < kVoronoiSiteCount; ++i) {
    voronoiX_[i] = static_cast<int16_t>(
        ((advanceStandbyRng(rng_) >> 8) % kStandbyLifeColumns) * 16);
    voronoiY_[i] = static_cast<int16_t>(
        ((advanceStandbyRng(rng_) >> 8) % kStandbyLifeRows) * 16);

    const int16_t dx = static_cast<int16_t>(4 + ((advanceStandbyRng(rng_) >> 24) % 7));
    const int16_t dy = static_cast<int16_t>(3 + ((advanceStandbyRng(rng_) >> 24) % 6));
    voronoiDx_[i] = (advanceStandbyRng(rng_) & 1U) != 0 ? dx : static_cast<int16_t>(-dx);
    voronoiDy_[i] = (advanceStandbyRng(rng_) & 1U) != 0 ? dy : static_cast<int16_t>(-dy);
  }
  renderVoronoi();
}

void StandbyScreensaver::renderVoronoi() {
  const size_t cellCount =
      static_cast<size_t>(kStandbyLifeColumns) * static_cast<size_t>(kStandbyLifeRows);
  const size_t wordCount = packedLifeWordCount(cellCount);
  lifeCells_.assign(wordCount, 0);
  dimCells_.assign(wordCount, 0);
  if (voronoiX_.empty()) {
    return;
  }

  for (uint16_t y = 0; y < kStandbyLifeRows; ++y) {
    const int32_t cellY = static_cast<int32_t>(y) * 16 + 8;
    for (uint16_t x = 0; x < kStandbyLifeColumns; ++x) {
      const int32_t cellX = static_cast<int32_t>(x) * 16 + 8;
      int32_t nearest = INT32_MAX;
      int32_t secondNearest = INT32_MAX;
      for (size_t i = 0; i < voronoiX_.size(); ++i) {
        const int32_t dx = cellX - voronoiX_[i];
        const int32_t dy = cellY - voronoiY_[i];
        const int32_t distance = dx * dx + dy * dy;
        if (distance < nearest) {
          secondNearest = nearest;
          nearest = distance;
        } else if (distance < secondNearest) {
          secondNearest = distance;
        }
      }

      const size_t index = static_cast<size_t>(y) * kStandbyLifeColumns + x;
      const int32_t gap = secondNearest - nearest;
      if (nearest < 1200 || gap < 190) {
        setPackedLifeCell(lifeCells_, index, true);
      } else if (gap < 580 + nearest / 180) {
        setPackedLifeCell(dimCells_, index, true);
      }
    }
  }
}

void StandbyScreensaver::stepVoronoi() {
  constexpr size_t kVoronoiSiteCount = 15;
  if (voronoiX_.size() != kVoronoiSiteCount || voronoiY_.size() != kVoronoiSiteCount ||
      voronoiDx_.size() != kVoronoiSiteCount || voronoiDy_.size() != kVoronoiSiteCount) {
    seedVoronoi(millis());
    return;
  }

  const int16_t maxX = static_cast<int16_t>((kStandbyLifeColumns - 1) * 16);
  const int16_t maxY = static_cast<int16_t>((kStandbyLifeRows - 1) * 16);
  for (size_t i = 0; i < voronoiX_.size(); ++i) {
    int16_t nextX = static_cast<int16_t>(voronoiX_[i] + voronoiDx_[i]);
    int16_t nextY = static_cast<int16_t>(voronoiY_[i] + voronoiDy_[i]);
    if (nextX < 0 || nextX > maxX) {
      voronoiDx_[i] = static_cast<int16_t>(-voronoiDx_[i]);
      nextX = std::max<int16_t>(0, std::min<int16_t>(maxX, nextX));
    }
    if (nextY < 0 || nextY > maxY) {
      voronoiDy_[i] = static_cast<int16_t>(-voronoiDy_[i]);
      nextY = std::max<int16_t>(0, std::min<int16_t>(maxY, nextY));
    }
    voronoiX_[i] = nextX;
    voronoiY_[i] = nextY;
  }

  ++generation_;
  if (generation_ > 2400) {
    seedVoronoi(millis());
    return;
  }
  renderVoronoi();
}

void StandbyScreensaver::seedScreenOff(uint32_t nowMs) {
  (void)nowMs;
  lifeCells_.clear();
  lifeNextCells_.clear();
  dimCells_.clear();
  mazeVisited_.clear();
  mazeStack_.clear();
  voronoiX_.clear();
  voronoiY_.clear();
  voronoiDx_.clear();
  voronoiDy_.clear();
  generation_ = 0;
  screenOffActive_ = true;
  display_.prepareForSleep();
}

void StandbyScreensaver::update(uint32_t nowMs, bool force) {
  if (mode_ == Mode::ScreenOff) {
    if (!screenOffActive_) {
      seedScreenOff(nowMs);
    }
    lastFrameMs_ = nowMs;
    return;
  }

  if (!force && nowMs - lastFrameMs_ < kStandbyFrameMs) {
    return;
  }

  if (!force) {
    step(nowMs);
  } else if (lifeCells_.empty()) {
    seed(nowMs);
  }

  lastFrameMs_ = nowMs;
  display_.renderLifeScreensaver(lifeCells_, kStandbyLifeColumns, kStandbyLifeRows, generation_,
                                 dimCells_.empty() ? nullptr : &dimCells_);
}
