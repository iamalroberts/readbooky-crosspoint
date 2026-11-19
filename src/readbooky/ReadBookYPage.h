#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ReadBookYModels.h"

namespace readbooky {

// Manages ReadBookY Page Q&A on XTEINK X4 Pro:
//  - Non-blocking async question submission (`POST /device/v1/page/questions` -> 202 Accepted)
//  - Offline Page question queue preserving the exact frozen local sequential
//    `trustedAnchorAtQuestion` captured when the question was asked (never overwritten by
//    stale server ordinal or raised by later reading!)
//  - Background/manual answer polling (`GET /device/v1/page/questions/{requestId}`)
//  - Offline-readable Page History (`GET /device/v1/page/history`) cached per journey
//  - Static e-ink unread answer badge state ("Page found an answer")
class ReadBookYPage {
 public:
  static ReadBookYPage& getInstance();

  void begin();

  // Persists a question locally immediately (freezing local sequential trustedAnchorAtQuestion)
  // and submits to the server immediately if Wi-Fi is connected.
  // Returns true once durably queued (online or offline); sets `submittedOnlineOut` if accepted by server.
  bool askQuestion(const ReadBookYSidecar& sidecar, const std::string& question, const std::string& selectedText,
                   const ReadBookYAnchorRef& selectedAnchor, bool spoilerOverride, ReadBookYPageItem& outItem,
                   bool& submittedOnlineOut, std::string& errorOut);

  // Submits any `queued_offline` questions and polls `queued`/`working` questions for `journeyId`.
  // Returns true if any item changed state (e.g. a new answer became `ready`).
  bool flushAndPollJourney(const std::string& journeyId, std::string& errorOut);

  // Fetches server Page history for `journeyId` and merges cleanly with local offline queue.
  bool refreshHistory(const std::string& journeyId, std::vector<ReadBookYPageItem>& outItems, std::string& errorOut);

  // Loads offline-readable Page items for `journeyId` directly from SD cache.
  bool loadCachedItems(const std::string& journeyId, std::vector<ReadBookYPageItem>& outItems) const;

  // Marks a ready answer as read locally (and acknowledges on server when online).
  void markRead(const std::string& journeyId, const std::string& requestId);

  // Retries a failed question using a new requestId while preserving its original frozen boundary.
  bool retryFailedQuestion(const std::string& journeyId, const std::string& oldRequestId, ReadBookYPageItem& outItem,
                           bool& submittedOnlineOut, std::string& errorOut);

  // Fast atomic/cached check for reader chrome ("Page found an answer") without SD I/O on every frame.
  bool hasUnreadForJourney(const std::string& journeyId) const;
  size_t totalQueuedOfflineCount() const;
  void refreshUnreadFlagForJourney(const std::string& journeyId);

 private:
  ReadBookYPage() = default;

  bool submitSingleRequestOnline(ReadBookYPageItem& item, std::string& errorOut);
  bool pollSingleRequestOnline(ReadBookYPageItem& item, std::string& errorOut);
  void updateUnreadStateLocked(const std::string& journeyId, const std::vector<ReadBookYPageItem>& items);

  mutable std::mutex mutex_;
  std::string trackedJourneyId_;
  std::atomic<bool> trackedJourneyHasUnread_{false};
};

#define READBOOKY_PAGE readbooky::ReadBookYPage::getInstance()

}  // namespace readbooky
