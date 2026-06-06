#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

#include "app/AppState.h"

class ReadingLoop;

// Computes "time remaining" for the current book. The base estimate is words /
// WPM; the accurate mode adds the per-word pacing bonus (long/complex/punctuation
// delays) summed in the background, block by block, so a big book does not stall
// the UI. Holds the prefix-sum cache and the in-progress build state.
//
// Book identity (path/title) is passed per call; state-machine queries and
// re-render side effects are injected so the engine never sees App.
class TimeEstimateEngine {
 public:
  using StateProvider = std::function<AppState()>;
  using RenderReaderFn = std::function<void(uint32_t nowMs)>;
  using RenderMenuFn = std::function<void()>;
  using RenderStatusFn =
      std::function<void(const char *title, const char *line1, const char *line2, int percent)>;

  TimeEstimateEngine(ReadingLoop &reader, StateProvider currentState,
                     RenderReaderFn renderActiveReader, RenderMenuFn renderMenu,
                     RenderStatusFn renderStatus);

  uint32_t estimatedReadingTimeRemainingMs(size_t startIndex, size_t endIndex) const;

  void rebuild(const String &bookPath, const String &bookTitle);
  void invalidate();
  void markPendingRebuild() { pendingRebuild_ = true; }
  void flushPendingRebuild(const String &bookPath, const String &bookTitle);
  void update(uint32_t nowMs, const String &bookPath);  // advance the background build

  bool buildMatchesCurrentBook(const String &bookPath) const;
  bool buildInProgress() const { return buildInProgress_; }
  int buildProgressPercent() const;
  bool cacheValid() const { return cacheValid_; }

  bool accurateEstimate() const { return accurateEstimate_; }
  void setAccurateEstimate(bool enabled) { accurateEstimate_ = enabled; }

  static String formatReadingTimeRemaining(uint32_t remainingMs);

 private:
  uint32_t estimatedPacingBonusMs(size_t startIndex, size_t endIndex) const;
  void cancelBuild();

  ReadingLoop &reader_;
  StateProvider currentState_;
  RenderReaderFn renderActiveReader_;
  RenderMenuFn renderMenu_;
  RenderStatusFn renderStatus_;

  std::vector<uint32_t> wordBonusBlockPrefixSumMs_;
  String buildBookPath_;
  size_t buildWordCount_ = 0;
  size_t buildBlockCount_ = 0;
  size_t buildNextBlock_ = 0;
  uint32_t buildRunningMs_ = 0;
  uint32_t buildStartedMs_ = 0;
  uint32_t buildLastLogMs_ = 0;
  bool cacheValid_ = false;
  bool buildInProgress_ = false;
  bool accurateEstimate_ = true;
  bool pendingRebuild_ = false;
};
