#pragma once

#include <string>
#include <vector>

#include "ReadBookYModels.h"

namespace readbooky {

// Atomic, crash-safe SD persistence for ReadBookY caches, sidecars, and Page history.
// All file writes use `<path>.tmp` -> write -> flush -> close -> `Storage.replaceFile()`.
class ReadBookYStorage {
 public:
  static bool ensureDirectories();

  // Atomic file helpers via HalStorage
  static bool writeAtomicString(const std::string& path, const std::string& content);
  static bool readStringFile(const std::string& path, size_t maxBytes, std::string& out);

  // Sidecar (`<book.epub>.readbooky.json`) helpers
  static std::string sidecarPathForEpub(const std::string& epubPath);
  static bool isReadBookYBookPath(const std::string& epubPath);
  static bool loadSidecar(const std::string& epubPath, ReadBookYSidecar& out);
  static bool saveSidecar(const std::string& epubPath, const ReadBookYSidecar& sidecar);
  static std::string buildSafeEpubPath(const std::string& title, const std::string& editionId);

  // Cached authorized library list (`/.readbooky/library.json`)
  static bool loadLibraryCache(std::vector<ReadBookYBookMeta>& outBooks);
  static bool saveLibraryCache(const std::vector<ReadBookYBookMeta>& books);

  // Per-journey cache (`/.readbooky/books/<journeyId>.json`)
  static std::string bookCachePath(const std::string& journeyId);
  static bool loadBookCache(const std::string& journeyId, ReadBookYBookCache& out);
  static bool saveBookCache(const ReadBookYBookCache& cache);

  // Per-journey Page history & offline question queue (`/.readbooky/page/<journeyId>.json`)
  static std::string pageCachePath(const std::string& journeyId);
  static bool loadPageItems(const std::string& journeyId, std::vector<ReadBookYPageItem>& outItems);
  static bool savePageItems(const std::string& journeyId, const std::vector<ReadBookYPageItem>& items);

  // Clear user-scoped caches on logout (preserves non-ReadBookY CrossPoint files)
  static void clearUserCaches();

  static std::string sanitizeFileSegment(const std::string& raw, size_t maxLen = 64);
};

}  // namespace readbooky
