#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ReadBookYClient.h"
#include "ReadBookYModels.h"

namespace readbooky {

// Local-first synchronization engine for ReadBookY on XTEINK X4 Pro.
// Owns:
//  - Library listing (`GET /device/v1/library`) and Range-resumable EPUB downloads with SHA-256 verification
//  - Optional StarDict dictionary pack discovery & installation (`GET /device/v1/dictionaries`)
//  - Local-first mutations for progress, highlights, visibility, comments, replies, and 6 reactions
//  - Two-way idempotent delta sync (`POST /device/v1/sync`) with deterministic `(changedAt, changeId)`
//    cursor pagination and `resetRequired` snapshot recovery
//  - Bounded background sync task & optional sleep-time flush
class ReadBookYSync {
 public:
  static ReadBookYSync& getInstance();

  void begin();

  // Library & downloads
  bool fetchLibrary(std::vector<ReadBookYBookMeta>& outBooks, std::string& errorOut);
  bool downloadBook(ReadBookYBookMeta& bookInOut, std::string& errorOut,
                    const DownloadProgressCallback& onProgress = nullptr);

  // Offline StarDict dictionary packs
  bool fetchDictionaries(std::vector<ReadBookYDictionaryPack>& outPacks, std::string& errorOut);
  bool installDictionaryPack(const ReadBookYDictionaryPack& pack, std::string& errorOut,
                             const DownloadProgressCallback& onProgress = nullptr);

  // Local-first reading position & PAGE_SPEC.md trusted-position tracking
  // navigationReason: "sequential" | "jump" | "toc" | "search" | "bookmark" | "footnote" | "external" | "manual_override"
  bool recordReadingProgress(const ReadBookYSidecar& sidecar, int spineIndex, const std::string& chapterHref,
                             uint32_t sourceOffset, float percentage, const std::string& chapterTitle,
                             const std::string& navigationReason, bool manualOverride = false);

  // Local-first highlight & social mutations (always succeed offline via durable WAL + local cache)
  bool createHighlightLocal(const ReadBookYSidecar& sidecar, int spineIndex, const std::string& chapterHref,
                            uint32_t startOffset, uint32_t endOffset, const std::string& selectedText,
                            const std::string& contextBefore, const std::string& contextAfter,
                            const std::string& scope, bool isPrivate, ReadBookYHighlight& outHighlight);
  bool deleteHighlightLocal(const ReadBookYSidecar& sidecar, const std::string& highlightId);
  bool updateVisibilityLocal(const ReadBookYSidecar& sidecar, const std::string& anchorId,
                             const std::string& highlightId, const std::string& scope, bool isPrivate);
  bool createCommentLocal(const ReadBookYSidecar& sidecar, const std::string& anchorId,
                          const std::string& highlightId, const std::string& content,
                          const std::string& parentCommentId, const std::string& rootCommentId,
                          ReadBookYComment& outComment);
  bool editCommentLocal(const ReadBookYSidecar& sidecar, const std::string& commentId,
                        const std::string& newContent, bool isReply);
  bool deleteCommentLocal(const ReadBookYSidecar& sidecar, const std::string& commentId, bool isReply);
  bool toggleReactionLocal(const ReadBookYSidecar& sidecar, const std::string& commentId, size_t reactionIndex);

  // Two-way delta sync (pushes pending WAL ops + pulls remote changes or full snapshot on resetRequired)
  bool syncJourneyNow(const std::string& journeyId, const std::string& bookId, const std::string& editionId,
                      bool mirrorClippings, std::string& errorOut);
  bool syncAllNow(std::string& errorOut);

  // Non-blocking background sync trigger (bounded heap/Wi-Fi/concurrency gate)
  void triggerBackgroundSync(const std::string& priorityJourneyId = "");
  bool isSyncing() const { return syncRunning_.load(std::memory_order_acquire); }

  // Called during sleep entry when `SETTINGS.readbookySyncOnSleep` is enabled
  void prepareForSleep();

  // Status summary for static e-ink banners
  std::string statusSummary() const;
  int64_t lastSyncEpochMs() const { return lastSyncMs_.load(std::memory_order_acquire); }
  std::string lastError() const;

 private:
  ReadBookYSync() = default;

  static void backgroundSyncTask(void* param);
  static bool isAnchorFurtherForward(const ReadBookYAnchorRef& candidate, const ReadBookYAnchorRef& existing);

  mutable std::mutex syncMutex_;
  mutable std::mutex statusMutex_;
  std::atomic<bool> syncRunning_{false};
  std::atomic<int64_t> lastSyncMs_{0};
  std::string lastError_;
  std::string activeJourneyId_;
};

#define READBOOKY_SYNC readbooky::ReadBookYSync::getInstance()

}  // namespace readbooky
