#pragma once

#include <Arduino.h>
#include <Preferences.h>

class StorageManager;

// Per-book persistence in NVS: saved reading position, word count, "recently
// opened" ordering, and the finished flag, all keyed by a hash of the book path.
// Pure storage layer — no live reader/session state. The live-session
// orchestration (loadBookAtIndex, saveReadingPosition) stays in App and calls
// into this store.
class BookLibraryStore {
 public:
  static constexpr uint32_t kNoSavedWordIndex = 0xFFFFFFFFUL;

  BookLibraryStore(Preferences &preferences, StorageManager &storage);

  static uint32_t hashBookPath(const String &path);

  int findIndexByPath(const String &path) const;

  // Saved reading position. savedWordIndex returns kNoSavedWordIndex if none
  // (optionally migrating the legacy single-book key on first read).
  uint32_t savedWordIndex(const String &path, bool allowLegacyFallback);
  void rememberPosition(const String &path, uint32_t wordIndex);
  void rememberWordCount(const String &path, uint32_t wordCount);

  bool isFinished(const String &path);
  void setFinished(const String &path, bool finished);

  void markRecent(const String &path);
  uint32_t recentSequence(const String &path);

  // Progress for a book that is NOT currently open (reads NVS only). Returns
  // false if no saved position/word-count is available.
  bool savedProgressPercent(const String &path, uint8_t &percent);

 private:
  String positionKey(const String &path) const;
  String wordCountKey(const String &path) const;
  String recentKey(const String &path) const;
  String finishedKey(const String &path) const;
  uint32_t nextRecentSequence();

  Preferences &preferences_;
  StorageManager &storage_;
};
