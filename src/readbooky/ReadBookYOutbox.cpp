#include "ReadBookYOutbox.h"

#ifdef ENABLE_READBOOKY

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "ReadBookYConfig.h"
#include "ReadBookYStorage.h"

namespace readbooky {

ReadBookYOutbox& ReadBookYOutbox::getInstance() {
  static ReadBookYOutbox inst;
  return inst;
}

void ReadBookYOutbox::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReadBookYStorage::ensureDirectories();
  std::vector<ReadBookYOutboxOp> pending;
  std::vector<ReadBookYOutboxOp> rejected;
  loadFileLocked(OUTBOX_WAL_PATH, pending);
  loadFileLocked(OUTBOX_REJECTED_PATH, rejected);
  cachedPendingCount_ = pending.size();
  cachedRejectedCount_ = rejected.size();
  countsLoaded_ = true;
}

std::string ReadBookYOutbox::serializeOpLine(const ReadBookYOutboxOp& op) {
  JsonDocument doc;
  doc["opId"] = op.operationId;
  doc["journeyId"] = op.journeyId;
  doc["bookId"] = op.bookId;
  doc["editionId"] = op.editionId;
  doc["type"] = op.type;
  doc["entityId"] = op.entityId;
  doc["payloadJson"] = op.payloadJson;
  doc["ts"] = op.createdAtMs;
  doc["retries"] = op.retryCount;
  if (!op.lastError.empty()) doc["err"] = op.lastError;

  std::string line;
  serializeJson(doc, line);
  line.push_back('\n');
  return line;
}

bool ReadBookYOutbox::parseOpLine(const std::string& line, ReadBookYOutboxOp& out) {
  if (line.empty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, line)) return false;

  out = {};
  out.operationId = doc["opId"] | "";
  out.journeyId = doc["journeyId"] | "";
  out.bookId = doc["bookId"] | "";
  out.editionId = doc["editionId"] | "";
  out.type = doc["type"] | "";
  out.entityId = doc["entityId"] | "";
  out.payloadJson = doc["payloadJson"] | "{}";
  out.createdAtMs = doc["ts"] | 0LL;
  out.retryCount = doc["retries"] | 0;
  out.lastError = doc["err"] | "";
  return !out.operationId.empty() && !out.type.empty();
}

bool ReadBookYOutbox::loadFileLocked(const char* path, std::vector<ReadBookYOutboxOp>& outOps) const {
  outOps.clear();
  if (!Storage.exists(path)) return true;

  std::string raw;
  if (!ReadBookYStorage::readStringFile(path, 256U * 1024U, raw)) return false;

  size_t pos = 0;
  while (pos < raw.size() && outOps.size() < MAX_OUTBOX_LINES) {
    size_t nl = raw.find('\n', pos);
    const bool hasNewline = (nl != std::string::npos);
    std::string line = hasNewline ? raw.substr(pos, nl - pos) : raw.substr(pos);
    pos = hasNewline ? (nl + 1) : raw.size();

    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    ReadBookYOutboxOp op;
    if (parseOpLine(line, op)) {
      outOps.push_back(std::move(op));
    } else {
      // Tolerate a truncated trailing line from mid-write power loss without losing prior ops
      LOG_ERR("RBYWAL", "Skipping malformed WAL line in %s", path);
    }
  }
  return true;
}

bool ReadBookYOutbox::writeAllAtomicLocked(const char* path, const std::vector<ReadBookYOutboxOp>& ops) const {
  ReadBookYStorage::ensureDirectories();
  std::string combined;
  combined.reserve(ops.size() * 256);
  for (const auto& op : ops) {
    combined.append(serializeOpLine(op));
  }
  return ReadBookYStorage::writeAtomicString(path, combined);
}

bool ReadBookYOutbox::enqueue(const ReadBookYOutboxOp& op) {
  if (op.operationId.empty() || op.type.empty()) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  ReadBookYStorage::ensureDirectories();

  // Coalesce `update_progress` per journeyId (safe because latest payload carries both
  // currentAnchor and furthest monotonic localTrustedAnchor).
  if (op.type == "update_progress") {
    std::vector<ReadBookYOutboxOp> pending;
    loadFileLocked(OUTBOX_WAL_PATH, pending);

    bool replaced = false;
    for (auto& existing : pending) {
      if (existing.type == "update_progress" && existing.journeyId == op.journeyId) {
        existing = op;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      pending.push_back(op);
    }
    if (!writeAllAtomicLocked(OUTBOX_WAL_PATH, pending)) return false;
    cachedPendingCount_ = pending.size();
    countsLoaded_ = true;
    return true;
  }

  // For all social mutations: durable crash-safe append to queue.jsonl (or atomic rewrite if new)
  const std::string line = serializeOpLine(op);
  if (!Storage.exists(OUTBOX_WAL_PATH)) {
    if (!ReadBookYStorage::writeAtomicString(OUTBOX_WAL_PATH, line)) return false;
    cachedPendingCount_ = 1;
    countsLoaded_ = true;
    return true;
  }

  HalFile f = Storage.open(OUTBOX_WAL_PATH, O_RDWR | O_CREAT | O_APPEND);
  if (!f) {
    LOG_ERR("RBYWAL", "Failed opening WAL for append");
    return false;
  }
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  f.flush();
  f.close();
  if (written != line.size()) {
    LOG_ERR("RBYWAL", "Short append on WAL");
    return false;
  }

  if (countsLoaded_) {
    cachedPendingCount_ += 1;
  } else {
    std::vector<ReadBookYOutboxOp> pending;
    loadFileLocked(OUTBOX_WAL_PATH, pending);
    cachedPendingCount_ = pending.size();
    countsLoaded_ = true;
  }
  return true;
}

bool ReadBookYOutbox::loadPending(std::vector<ReadBookYOutboxOp>& outOps) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool ok = loadFileLocked(OUTBOX_WAL_PATH, outOps);
  if (ok) {
    cachedPendingCount_ = outOps.size();
    countsLoaded_ = true;
  }
  return ok;
}

bool ReadBookYOutbox::acknowledgeAndCompact(const std::unordered_set<std::string>& ackedOpIds,
                                            const std::vector<ReadBookYOutboxOp>& rejectedOps) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::unordered_set<std::string> removeIds = ackedOpIds;
  for (const auto& r : rejectedOps) {
    removeIds.insert(r.operationId);
  }

  std::vector<ReadBookYOutboxOp> pending;
  loadFileLocked(OUTBOX_WAL_PATH, pending);

  std::vector<ReadBookYOutboxOp> remaining;
  remaining.reserve(pending.size());
  for (auto& op : pending) {
    if (removeIds.find(op.operationId) == removeIds.end()) {
      remaining.push_back(std::move(op));
    }
  }

  if (!writeAllAtomicLocked(OUTBOX_WAL_PATH, remaining)) {
    return false;
  }
  cachedPendingCount_ = remaining.size();

  if (!rejectedOps.empty()) {
    std::vector<ReadBookYOutboxOp> existingRejected;
    loadFileLocked(OUTBOX_REJECTED_PATH, existingRejected);
    for (const auto& r : rejectedOps) {
      existingRejected.push_back(r);
    }
    // Bound rejected log to most recent 50 entries
    if (existingRejected.size() > 50) {
      existingRejected.erase(existingRejected.begin(),
                             existingRejected.begin() + static_cast<ptrdiff_t>(existingRejected.size() - 50));
    }
    writeAllAtomicLocked(OUTBOX_REJECTED_PATH, existingRejected);
    cachedRejectedCount_ = existingRejected.size();
  }

  countsLoaded_ = true;
  return true;
}

size_t ReadBookYOutbox::pendingCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!countsLoaded_) {
    std::vector<ReadBookYOutboxOp> p;
    loadFileLocked(OUTBOX_WAL_PATH, p);
    cachedPendingCount_ = p.size();
    std::vector<ReadBookYOutboxOp> r;
    loadFileLocked(OUTBOX_REJECTED_PATH, r);
    cachedRejectedCount_ = r.size();
    countsLoaded_ = true;
  }
  return cachedPendingCount_;
}

size_t ReadBookYOutbox::rejectedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!countsLoaded_) {
    std::vector<ReadBookYOutboxOp> r;
    loadFileLocked(OUTBOX_REJECTED_PATH, r);
    cachedRejectedCount_ = r.size();
  }
  return cachedRejectedCount_;
}

bool ReadBookYOutbox::loadRejected(std::vector<ReadBookYOutboxOp>& outOps) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool ok = loadFileLocked(OUTBOX_REJECTED_PATH, outOps);
  if (ok) cachedRejectedCount_ = outOps.size();
  return ok;
}

bool ReadBookYOutbox::clearRejected() {
  std::lock_guard<std::mutex> lock(mutex_);
  Storage.remove(OUTBOX_REJECTED_PATH);
  cachedRejectedCount_ = 0;
  return true;
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
