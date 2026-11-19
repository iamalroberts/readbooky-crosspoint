#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ReadBookYConfig.h"

namespace readbooky {

struct ReadBookYSession {
  bool signedIn = false;
  std::string uid;
  std::string email;
  std::string displayName;
  std::string color = "#999999";
  std::string deviceId;
};

// Frozen anchor reference using CrossPoint visible-codepoint offsets.
// Authoritative for offline Page questions and cross-device position sync.
struct ReadBookYAnchorRef {
  int spineIndex = 0;
  std::string chapterHref;
  uint32_t sourceOffset = 0;
  float percentage = 0.0f;
  std::string chapterTitle;
  bool valid = false;
};

struct ReadBookYPosition {
  int spineIndex = 0;
  std::string chapterHref;
  uint32_t sourceOffset = 0;
  float progressPercent = 0.0f;
  std::string currentCfi;

  // Authoritative local sequential-reading boundary (PAGE_SPEC.md)
  ReadBookYAnchorRef localTrustedAnchor;

  // Server-reported canonical trusted boundary
  uint32_t trustedOrdinal = 0;
  float trustedPercent = 0.0f;
  std::string trustedChapterTitle;
  std::string trustedPositionSource = "sequential";
  int64_t updatedAtMs = 0;
};

struct ReadBookYBookMeta {
  std::string journeyId;
  std::string journeyName;
  std::string bookId;
  std::string editionId;
  std::string workId;
  std::string title;
  std::string author;
  std::string language = "en";
  std::string coverUrl;
  std::string localPath;  // /Books/ReadBookY/<SafeTitle>.epub when downloaded
  uint64_t fileSize = 0;
  std::string epubSha256;
  bool pageReady = false;
  bool downloaded = false;
  int participantCount = 1;
  int64_t updatedAtMs = 0;
  ReadBookYPosition progress;
};

struct ReadBookYSidecar {
  std::string journeyId;
  std::string bookId;
  std::string editionId;
  std::string workId;
  std::string title;
  std::string author;
  std::string epubSha256;
  uint64_t fileSize = 0;
  int64_t downloadedAtMs = 0;
};

struct ReadBookYHighlight {
  std::string id;
  std::string anchorId;
  std::string journeyId;
  std::string bookId;
  std::string uid;
  int spineIndex = 0;
  std::string chapterHref;
  std::string cfiRange;
  uint32_t sourceStartOffset = UINT32_MAX;
  uint32_t sourceEndOffset = UINT32_MAX;
  uint32_t ordinal = 0;
  std::string selectedText;
  std::string contextBefore;
  std::string contextAfter;
  std::string color = "#999999";
  std::string scope = "public";  // "public" | "private" | "restricted"
  bool isPrivate = false;
  std::vector<std::string> visibleTo;
  std::string anchorResolution = "exact";
  int64_t createdAtMs = 0;
  int64_t updatedAtMs = 0;
};

struct ReadBookYAnchor {
  std::string id;
  std::string journeyId;
  std::string bookId;
  std::string scope = "public";
  std::string ownerUid;
  bool isPrivate = false;
  std::vector<std::string> visibleTo;
  int spineIndex = 0;
  std::string chapterHref;
  std::string cfiRange;
  uint32_t sourceStartOffset = UINT32_MAX;
  uint32_t sourceEndOffset = UINT32_MAX;
  uint32_t ordinal = 0;
  std::string anchorText;
  float progressPercent = 0.0f;
  int64_t updatedAtMs = 0;
  int64_t lastCommentAtMs = 0;
};

struct ReadBookYComment {
  std::string id;
  std::string anchorId;
  std::string highlightId;
  std::string journeyId;
  std::string bookId;
  std::string uid;
  std::string authorName;
  std::string content;
  std::string parentCommentId;
  std::string rootCommentId;
  std::string scope = "public";
  bool isPrivate = false;
  std::vector<std::string> visibleTo;
  // Parallel to REACTION_KEYS[0..5]
  std::vector<std::string> reactions[REACTION_COUNT];
  int64_t createdAtMs = 0;
  int64_t updatedAtMs = 0;
};

struct ReadBookYSyncCursor {
  int64_t changedAtMs = 0;
  std::string changeId;
  bool initialized = false;
};

struct ReadBookYBookCache {
  std::string journeyId;
  std::string bookId;
  std::string editionId;
  ReadBookYSyncCursor cursor;
  ReadBookYPosition position;
  std::vector<ReadBookYAnchor> anchors;
  std::vector<ReadBookYHighlight> highlights;
  std::vector<ReadBookYComment> comments;
};

// Simplified durable outbox op: strictly pending on disk until server ack.
struct ReadBookYOutboxOp {
  std::string operationId;
  std::string journeyId;
  std::string bookId;
  std::string editionId;
  std::string type;
  std::string entityId;
  std::string payloadJson;
  int64_t createdAtMs = 0;
  uint16_t retryCount = 0;
  std::string lastError;
};

struct ReadBookYPageCitation {
  std::string anchorId;
  std::string chapterTitle;
  std::string snippet;
  uint32_t ordinal = 0;
  bool isPriorBook = false;
};

struct ReadBookYPageItem {
  std::string requestId;
  std::string threadId;
  std::string journeyId;
  std::string bookId;
  // "queued_offline" | "queued" | "working" | "ready" | "failed"
  std::string status = "queued_offline";
  std::string question;
  std::string selectedText;
  ReadBookYAnchorRef selectedAnchor;
  // Authoritative local sequential boundary captured when the question was asked
  ReadBookYAnchorRef trustedAnchorAtQuestion;
  // Fallback-only last known server ordinal at question time
  uint32_t trustedOrdinalAtQuestion = 0;
  uint32_t trustedOrdinalUsed = 0;
  std::string boundaryLabel;
  bool spoilerOverride = false;
  std::string answer;
  bool cautious = false;
  std::string answerConfidence;
  std::vector<ReadBookYPageCitation> citations;
  bool unread = false;
  bool retryable = false;
  std::string errorMessage;
  int64_t queuedAtMs = 0;
  int64_t completedAtMs = 0;
};

struct ReadBookYDictionaryFile {
  std::string name;
  uint64_t size = 0;
  std::string sha256;
  std::string downloadUrl;
};

struct ReadBookYDictionaryPack {
  std::string id;
  std::string name;
  std::string language;
  std::string version;
  std::string license;
  std::string attribution;
  uint64_t totalBytes = 0;
  std::vector<ReadBookYDictionaryFile> files;
};

}  // namespace readbooky
