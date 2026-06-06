#include "book/BookLibraryStore.h"

#include <algorithm>
#include <cstdio>

#include "storage/StorageManager.h"

namespace {

constexpr const char *kPrefLegacyWordIndex = "word";
constexpr const char *kPrefRecentSeq = "seq";

}  // namespace

BookLibraryStore::BookLibraryStore(Preferences &preferences, StorageManager &storage)
    : preferences_(preferences), storage_(storage) {}

uint32_t BookLibraryStore::hashBookPath(const String &path) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < path.length(); ++i) {
    hash ^= static_cast<uint8_t>(path[i]);
    hash *= 16777619UL;
  }
  return hash;
}

String BookLibraryStore::positionKey(const String &path) const {
  char key[10];
  std::snprintf(key, sizeof(key), "p%08lx", static_cast<unsigned long>(hashBookPath(path)));
  return String(key);
}

String BookLibraryStore::wordCountKey(const String &path) const {
  char key[10];
  std::snprintf(key, sizeof(key), "c%08lx", static_cast<unsigned long>(hashBookPath(path)));
  return String(key);
}

String BookLibraryStore::recentKey(const String &path) const {
  char key[10];
  std::snprintf(key, sizeof(key), "r%08lx", static_cast<unsigned long>(hashBookPath(path)));
  return String(key);
}

String BookLibraryStore::finishedKey(const String &path) const {
  char key[10];
  std::snprintf(key, sizeof(key), "f%08lx", static_cast<unsigned long>(hashBookPath(path)));
  return String(key);
}

int BookLibraryStore::findIndexByPath(const String &path) const {
  for (size_t i = 0; i < storage_.bookCount(); ++i) {
    if (storage_.bookPath(i) == path) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

uint32_t BookLibraryStore::savedWordIndex(const String &path, bool allowLegacyFallback) {
  const String key = positionKey(path);
  if (preferences_.isKey(key.c_str())) {
    return preferences_.getUInt(key.c_str(), 0);
  }

  if (allowLegacyFallback && preferences_.isKey(kPrefLegacyWordIndex)) {
    const uint32_t legacyWordIndex = preferences_.getUInt(kPrefLegacyWordIndex, 0);
    preferences_.putUInt(key.c_str(), legacyWordIndex);
    Serial.printf("[app] migrated legacy position word=%u to key=%s\n",
                  static_cast<unsigned int>(legacyWordIndex), key.c_str());
    return legacyWordIndex;
  }

  return kNoSavedWordIndex;
}

void BookLibraryStore::rememberPosition(const String &path, uint32_t wordIndex) {
  preferences_.putUInt(positionKey(path).c_str(), wordIndex);
  preferences_.putUInt(kPrefLegacyWordIndex, wordIndex);
}

void BookLibraryStore::rememberWordCount(const String &path, uint32_t wordCount) {
  preferences_.putUInt(wordCountKey(path).c_str(), wordCount);
}

bool BookLibraryStore::isFinished(const String &path) {
  if (path.isEmpty()) {
    return false;
  }
  return preferences_.getBool(finishedKey(path).c_str(), false);
}

void BookLibraryStore::setFinished(const String &path, bool finished) {
  if (path.isEmpty()) {
    return;
  }
  const String key = finishedKey(path);
  if (finished) {
    preferences_.putBool(key.c_str(), true);
  } else if (preferences_.isKey(key.c_str())) {
    preferences_.remove(key.c_str());
  }
}

uint32_t BookLibraryStore::nextRecentSequence() {
  uint32_t sequence = preferences_.getUInt(kPrefRecentSeq, 0);
  if (sequence == 0xFFFFFFFEUL) {
    sequence = 0;
  }
  ++sequence;
  preferences_.putUInt(kPrefRecentSeq, sequence);
  return sequence;
}

uint32_t BookLibraryStore::recentSequence(const String &path) {
  return preferences_.getUInt(recentKey(path).c_str(), 0);
}

void BookLibraryStore::markRecent(const String &path) {
  if (path.isEmpty()) {
    return;
  }
  preferences_.putUInt(recentKey(path).c_str(), nextRecentSequence());
}

bool BookLibraryStore::savedProgressPercent(const String &path, uint8_t &percent) {
  const String posKey = positionKey(path);
  const String countKey = wordCountKey(path);
  if (!preferences_.isKey(posKey.c_str()) || !preferences_.isKey(countKey.c_str())) {
    return false;
  }

  size_t wordIndex = preferences_.getUInt(posKey.c_str(), 0);
  const size_t wordCount = preferences_.getUInt(countKey.c_str(), 0);
  if (wordCount <= 1) {
    return false;
  }

  wordIndex = std::min(wordIndex, wordCount - 1);
  const size_t progress = (wordIndex * static_cast<size_t>(100)) / (wordCount - 1);
  percent = static_cast<uint8_t>(std::min(static_cast<size_t>(100), progress));
  return true;
}
