#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "ReadBookYModels.h"

namespace readbooky {

// Crash-safe durable offline-first WAL outbox (`/.readbooky/outbox/queue.jsonl`).
//
// Simplified state machine:
//  - Operations remain strictly `pending` on disk until the server explicitly acknowledges them.
//  - There is no durable `syncing` transition: if power is lost mid-HTTP-request, the exact
//    same operation with its stable `operationId` and stable entity IDs is safely replayed.
//  - Compaction rewrites remaining pending ops to `queue.jsonl.tmp` -> flush -> close ->
//    atomic `Storage.replaceFile()`.
//  - Permanently rejected ops (e.g. deleted parent comment or permission denied) are moved to
//    `rejected.jsonl` for user visibility/dismissal in ReadBookY Settings.
class ReadBookYOutbox {
 public:
  static ReadBookYOutbox& getInstance();

  void begin();

  // Durably appends an operation to the WAL. Consecutive `update_progress` operations for the
  // same `journeyId` are coalesced safely; all social mutations remain distinct ordered records.
  bool enqueue(const ReadBookYOutboxOp& op);

  // Loads all pending operations in FIFO order.
  bool loadPending(std::vector<ReadBookYOutboxOp>& outOps) const;

  // Atomically compacts the WAL by removing acknowledged operationIds and moving permanently
  // rejected operations to `rejected.jsonl`.
  bool acknowledgeAndCompact(const std::unordered_set<std::string>& ackedOpIds,
                             const std::vector<ReadBookYOutboxOp>& rejectedOps);

  size_t pendingCount() const;
  size_t rejectedCount() const;
  bool loadRejected(std::vector<ReadBookYOutboxOp>& outOps) const;
  bool clearRejected();

 private:
  ReadBookYOutbox() = default;

  static std::string serializeOpLine(const ReadBookYOutboxOp& op);
  static bool parseOpLine(const std::string& line, ReadBookYOutboxOp& out);
  bool loadFileLocked(const char* path, std::vector<ReadBookYOutboxOp>& outOps) const;
  bool writeAllAtomicLocked(const char* path, const std::vector<ReadBookYOutboxOp>& ops) const;

  mutable std::mutex mutex_;
  mutable size_t cachedPendingCount_ = 0;
  mutable size_t cachedRejectedCount_ = 0;
  mutable bool countsLoaded_ = false;
};

#define READBOOKY_OUTBOX readbooky::ReadBookYOutbox::getInstance()

}  // namespace readbooky
