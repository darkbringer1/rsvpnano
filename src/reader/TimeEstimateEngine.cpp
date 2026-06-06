#include "reader/TimeEstimateEngine.h"

#include <algorithm>
#include <utility>

#include "reader/ReadingLoop.h"

namespace {

constexpr size_t kTimeEstimateBlockWords = 256;
constexpr size_t kTimeEstimateBlocksPerUpdate = 1;
constexpr uint32_t kTimeEstimateProgressLogMs = 5000;

}  // namespace

TimeEstimateEngine::TimeEstimateEngine(ReadingLoop &reader, StateProvider currentState,
                                       RenderReaderFn renderActiveReader, RenderMenuFn renderMenu,
                                       RenderStatusFn renderStatus)
    : reader_(reader),
      currentState_(std::move(currentState)),
      renderActiveReader_(std::move(renderActiveReader)),
      renderMenu_(std::move(renderMenu)),
      renderStatus_(std::move(renderStatus)) {}

uint32_t TimeEstimateEngine::estimatedReadingTimeRemainingMs(size_t startIndex,
                                                             size_t endIndex) const {
  const size_t wordCount = reader_.wordCount();
  if (wordCount == 0 || reader_.wpm() == 0) {
    return 0;
  }

  startIndex = std::min(startIndex, wordCount);
  endIndex = std::min(endIndex, wordCount);
  if (endIndex <= startIndex) {
    return 0;
  }

  const uint32_t baseMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(endIndex - startIndex) * 60000ULL) /
      static_cast<uint64_t>(reader_.wpm()));

  if (!accurateEstimate_ || !cacheValid_) {
    return baseMs;
  }

  return baseMs + estimatedPacingBonusMs(startIndex, endIndex);
}

uint32_t TimeEstimateEngine::estimatedPacingBonusMs(size_t startIndex, size_t endIndex) const {
  if (!cacheValid_ || wordBonusBlockPrefixSumMs_.empty() || endIndex <= startIndex) {
    return 0;
  }

  const size_t wordCount = reader_.wordCount();
  startIndex = std::min(startIndex, wordCount);
  endIndex = std::min(endIndex, wordCount);
  if (endIndex <= startIndex) {
    return 0;
  }

  const size_t firstFullBlock = (startIndex + kTimeEstimateBlockWords - 1) /
                                kTimeEstimateBlockWords;
  const size_t lastFullBlockEnd = endIndex / kTimeEstimateBlockWords;
  uint32_t bonusMs = 0;

  if (firstFullBlock < lastFullBlockEnd &&
      lastFullBlockEnd < wordBonusBlockPrefixSumMs_.size()) {
    const size_t startPartialEnd =
        std::min(endIndex, firstFullBlock * kTimeEstimateBlockWords);
    for (size_t i = startIndex; i < startPartialEnd; ++i) {
      bonusMs += reader_.wordPacingBonusMsAt(i);
    }

    bonusMs += wordBonusBlockPrefixSumMs_[lastFullBlockEnd] -
               wordBonusBlockPrefixSumMs_[firstFullBlock];

    const size_t endPartialStart = lastFullBlockEnd * kTimeEstimateBlockWords;
    for (size_t i = endPartialStart; i < endIndex; ++i) {
      bonusMs += reader_.wordPacingBonusMsAt(i);
    }
    return bonusMs;
  }

  for (size_t i = startIndex; i < endIndex; ++i) {
    bonusMs += reader_.wordPacingBonusMsAt(i);
  }
  return bonusMs;
}

void TimeEstimateEngine::invalidate() {
  cancelBuild();
  cacheValid_ = false;
  std::vector<uint32_t>().swap(wordBonusBlockPrefixSumMs_);
}

void TimeEstimateEngine::rebuild(const String &bookPath, const String &bookTitle) {
  invalidate();
  pendingRebuild_ = false;
  if (!accurateEstimate_) {
    if (!bookTitle.isEmpty()) {
      renderStatus_("Reading time", bookTitle.c_str(), "Fast estimate enabled", 100);
    }
    return;
  }

  const size_t n = reader_.wordCount();
  if (n == 0) {
    return;
  }

  const String label = bookTitle.isEmpty() ? String("Current book") : bookTitle;
  buildWordCount_ = n;
  buildBlockCount_ = (buildWordCount_ + kTimeEstimateBlockWords - 1) / kTimeEstimateBlockWords;
  if (buildBlockCount_ == 0) {
    return;
  }

  wordBonusBlockPrefixSumMs_.assign(buildBlockCount_ + 1, 0);
  buildBookPath_ = bookPath;
  buildNextBlock_ = 0;
  buildRunningMs_ = 0;
  buildStartedMs_ = millis();
  buildLastLogMs_ = buildStartedMs_;
  buildInProgress_ = true;

  const String detail = String(static_cast<unsigned int>(n)) + " words in background";
  renderStatus_("Reading time", label.c_str(), detail.c_str(), 0);
  Serial.printf("[time-est] background build started words=%u blocks=%u book=%s\n",
                static_cast<unsigned int>(buildWordCount_),
                static_cast<unsigned int>(buildBlockCount_), bookPath.c_str());
}

void TimeEstimateEngine::flushPendingRebuild(const String &bookPath, const String &bookTitle) {
  if (!pendingRebuild_) {
    return;
  }
  rebuild(bookPath, bookTitle);
}

void TimeEstimateEngine::cancelBuild() {
  buildInProgress_ = false;
  buildBookPath_ = "";
  buildWordCount_ = 0;
  buildBlockCount_ = 0;
  buildNextBlock_ = 0;
  buildRunningMs_ = 0;
  buildStartedMs_ = 0;
  buildLastLogMs_ = 0;
}

int TimeEstimateEngine::buildProgressPercent() const {
  return static_cast<int>((buildNextBlock_ * 100UL) / std::max<size_t>(1, buildBlockCount_));
}

bool TimeEstimateEngine::buildMatchesCurrentBook(const String &bookPath) const {
  return buildInProgress_ && buildBookPath_ == bookPath && buildWordCount_ == reader_.wordCount();
}

void TimeEstimateEngine::update(uint32_t nowMs, const String &bookPath) {
  if (!buildInProgress_) {
    return;
  }

  if (!accurateEstimate_ || !buildMatchesCurrentBook(bookPath)) {
    Serial.println("[time-est] background build cancelled");
    invalidate();
    return;
  }

  const AppState state = currentState_();
  if (state == AppState::Playing || state == AppState::CompanionSync ||
      state == AppState::UsbTransfer || state == AppState::Standby ||
      state == AppState::Sleeping) {
    return;
  }

  size_t processedBlocks = 0;
  while (buildNextBlock_ < buildBlockCount_ && processedBlocks < kTimeEstimateBlocksPerUpdate) {
    const size_t block = buildNextBlock_;
    wordBonusBlockPrefixSumMs_[block] = buildRunningMs_;
    const size_t blockStart = block * kTimeEstimateBlockWords;
    const size_t blockEnd = std::min(buildWordCount_, blockStart + kTimeEstimateBlockWords);
    for (size_t i = blockStart; i < blockEnd; ++i) {
      buildRunningMs_ += reader_.wordPacingBonusMsAt(i);
    }
    ++buildNextBlock_;
    ++processedBlocks;
    delay(0);
  }

  if (buildNextBlock_ >= buildBlockCount_) {
    wordBonusBlockPrefixSumMs_[buildBlockCount_] = buildRunningMs_;
    cacheValid_ = true;
    const uint32_t elapsedMs = millis() - buildStartedMs_;
    Serial.printf("[time-est] background cached %u words in %u blocks bonus=%lums took=%lums\n",
                  static_cast<unsigned int>(buildWordCount_),
                  static_cast<unsigned int>(buildBlockCount_),
                  static_cast<unsigned long>(buildRunningMs_),
                  static_cast<unsigned long>(elapsedMs));
    cancelBuild();
    const AppState now = currentState_();
    if (now == AppState::Paused || now == AppState::Playing) {
      renderActiveReader_(nowMs);
    } else if (now == AppState::Menu) {
      renderMenu_();
    }
    return;
  }

  if (nowMs - buildLastLogMs_ >= kTimeEstimateProgressLogMs) {
    const int progress = static_cast<int>((buildNextBlock_ * 100UL) /
                                          std::max<size_t>(1, buildBlockCount_));
    Serial.printf("[time-est] background progress %u/%u blocks (%d%%)\n",
                  static_cast<unsigned int>(buildNextBlock_),
                  static_cast<unsigned int>(buildBlockCount_), progress);
    buildLastLogMs_ = nowMs;
    if (currentState_() == AppState::Paused) {
      renderActiveReader_(nowMs);
    }
  }
}

String TimeEstimateEngine::formatReadingTimeRemaining(uint32_t remainingMs) {
  const uint32_t totalSeconds = remainingMs / 1000UL;
  if (totalSeconds < 60UL) {
    return "0m";
  }

  const uint32_t totalMinutes = totalSeconds / 60UL;
  if (totalMinutes < 60UL) {
    return String(totalMinutes) + "m";
  }

  const uint32_t totalHours = totalMinutes / 60UL;
  const uint32_t minutes = totalMinutes % 60UL;
  if (totalHours < 24UL) {
    if (minutes == 0) {
      return String(totalHours) + "h";
    }
    return String(totalHours) + "h" + String(minutes) + "m";
  }

  const uint32_t days = totalHours / 24UL;
  const uint32_t hours = totalHours % 24UL;
  if (hours == 0) {
    return String(days) + "d";
  }
  return String(days) + "d" + String(hours) + "h";
}
