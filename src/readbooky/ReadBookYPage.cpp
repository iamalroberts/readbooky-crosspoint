#include "ReadBookYPage.h"

#ifdef ENABLE_READBOOKY

#include <ArduinoJson.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <algorithm>

#include "ReadBookYAnchors.h"
#include "ReadBookYAuth.h"
#include "ReadBookYClient.h"
#include "ReadBookYConfig.h"
#include "ReadBookYStorage.h"

namespace readbooky {

namespace {

int64_t currentEpochMs() {
  const int64_t sec = trustedtime::trustedNow();
  return sec > 0 ? sec * 1000LL : 0LL;
}

void parseCitations(JsonArrayConst arr, std::vector<ReadBookYPageCitation>& out) {
  out.clear();
  if (arr.isNull()) return;
  for (JsonObjectConst o : arr) {
    ReadBookYPageCitation c;
    c.anchorId = o["anchorId"] | "";
    c.chapterTitle = o["chapterTitle"] | "";
    c.snippet = o["snippet"] | "";
    c.ordinal = o["ordinal"] | 0U;
    c.isPriorBook = o["isPriorBook"] | false;
    out.push_back(std::move(c));
  }
}

void applyServerItemFields(JsonObjectConst obj, ReadBookYPageItem& dst) {
  if (obj.isNull()) return;
  const std::string reqId = obj["requestId"] | "";
  if (!reqId.empty()) dst.requestId = reqId;
  const std::string thrId = obj["threadId"] | "";
  if (!thrId.empty()) dst.threadId = thrId;
  const std::string st = obj["status"] | "";
  if (!st.empty()) dst.status = st;
  const std::string ans = obj["answer"] | "";
  if (!ans.empty()) dst.answer = ans;
  dst.cautious = obj["cautious"] | dst.cautious;
  const std::string conf = obj["answerConfidence"] | "";
  if (!conf.empty()) dst.answerConfidence = conf;
  const uint32_t ord = obj["trustedOrdinalUsed"] | 0U;
  if (ord > 0) dst.trustedOrdinalUsed = ord;
  const std::string bLabel = obj["boundaryLabel"] | "";
  if (!bLabel.empty()) dst.boundaryLabel = bLabel;
  dst.retryable = obj["retryable"] | false;
  dst.errorMessage = obj["errorMessage"] | "";
  const int64_t compMs = obj["completedAtMs"] | 0LL;
  if (compMs > 0) dst.completedAtMs = compMs;
  if (!obj["citations"].isNull()) {
    parseCitations(obj["citations"].as<JsonArrayConst>(), dst.citations);
  }
}

}  // namespace

ReadBookYPage& ReadBookYPage::getInstance() {
  static ReadBookYPage inst;
  return inst;
}

void ReadBookYPage::begin() { ReadBookYStorage::ensureDirectories(); }

void ReadBookYPage::updateUnreadStateLocked(const std::string& journeyId,
                                            const std::vector<ReadBookYPageItem>& items) {
  bool anyUnread = false;
  for (const auto& it : items) {
    if (it.status == "ready" && it.unread) {
      anyUnread = true;
      break;
    }
  }
  trackedJourneyId_ = journeyId;
  trackedJourneyHasUnread_.store(anyUnread, std::memory_order_release);
}

void ReadBookYPage::refreshUnreadFlagForJourney(const std::string& journeyId) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ReadBookYPageItem> items;
  ReadBookYStorage::loadPageItems(journeyId, items);
  updateUnreadStateLocked(journeyId, items);
}

bool ReadBookYPage::hasUnreadForJourney(const std::string& journeyId) const {
  if (journeyId.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (trackedJourneyId_ == journeyId) {
    return trackedJourneyHasUnread_.load(std::memory_order_acquire);
  }
  return false;
}

size_t ReadBookYPage::totalQueuedOfflineCount() const {
  std::vector<ReadBookYBookMeta> books;
  if (!ReadBookYStorage::loadLibraryCache(books)) return 0;
  size_t count = 0;
  for (const auto& b : books) {
    std::vector<ReadBookYPageItem> items;
    if (!ReadBookYStorage::loadPageItems(b.journeyId, items)) continue;
    for (const auto& it : items) {
      if (it.status == "queued_offline" || it.status == "queued" || it.status == "working") {
        ++count;
      }
    }
  }
  return count;
}

bool ReadBookYPage::loadCachedItems(const std::string& journeyId, std::vector<ReadBookYPageItem>& outItems) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadBookYStorage::loadPageItems(journeyId, outItems);
}

bool ReadBookYPage::askQuestion(const ReadBookYSidecar& sidecar, const std::string& question,
                                const std::string& selectedText, const ReadBookYAnchorRef& selectedAnchor,
                                const bool spoilerOverride, ReadBookYPageItem& outItem, bool& submittedOnlineOut,
                                std::string& errorOut) {
  submittedOnlineOut = false;
  errorOut.clear();
  if (sidecar.journeyId.empty() || question.empty()) {
    errorOut = "Journey and question are required";
    return false;
  }

  // Capture authoritative frozen local sequential reading boundary from book cache AT QUESTION TIME.
  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);

  ReadBookYPageItem item;
  item.requestId = "pgr_" + ReadBookYAnchors::generateUuidV4();
  item.journeyId = sidecar.journeyId;
  item.bookId = sidecar.bookId;
  item.status = "queued_offline";
  item.question = question;
  item.selectedText = selectedText;
  item.selectedAnchor = selectedAnchor;
  item.spoilerOverride = spoilerOverride;
  item.queuedAtMs = currentEpochMs();

  // Authoritative frozen boundary: local sequential trusted anchor (or current position if sequential not yet set)
  if (cache.position.localTrustedAnchor.valid) {
    item.trustedAnchorAtQuestion = cache.position.localTrustedAnchor;
  } else {
    item.trustedAnchorAtQuestion.spineIndex = cache.position.spineIndex;
    item.trustedAnchorAtQuestion.chapterHref = cache.position.chapterHref;
    item.trustedAnchorAtQuestion.sourceOffset = cache.position.sourceOffset;
    item.trustedAnchorAtQuestion.percentage = cache.position.progressPercent;
    item.trustedAnchorAtQuestion.chapterTitle = cache.position.trustedChapterTitle;
    item.trustedAnchorAtQuestion.valid = true;
  }
  // Fallback-only last known server ordinal (backend will NOT let this overwrite newer local anchor)
  item.trustedOrdinalAtQuestion = cache.position.trustedOrdinal;

  if (!item.trustedAnchorAtQuestion.chapterTitle.empty()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (%.0f%%)", item.trustedAnchorAtQuestion.chapterTitle.c_str(),
             item.trustedAnchorAtQuestion.percentage);
    item.boundaryLabel = buf;
  } else if (!cache.position.trustedChapterTitle.empty()) {
    item.boundaryLabel = cache.position.trustedChapterTitle;
  }

  // Persist durably FIRST so reboot/power loss never loses an offline question or its frozen boundary
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReadBookYPageItem> items;
    ReadBookYStorage::loadPageItems(sidecar.journeyId, items);
    items.insert(items.begin(), item);
    if (!ReadBookYStorage::savePageItems(sidecar.journeyId, items)) {
      errorOut = "Failed saving Page question to SD";
      return false;
    }
  }

  // Attempt immediate non-blocking online submission if Wi-Fi is connected
  if (WiFi.status() == WL_CONNECTED && READBOOKY_AUTH.isSignedIn()) {
    std::string subErr;
    if (submitSingleRequestOnline(item, subErr)) {
      submittedOnlineOut = true;
      std::lock_guard<std::mutex> lock(mutex_);
      std::vector<ReadBookYPageItem> items;
      ReadBookYStorage::loadPageItems(sidecar.journeyId, items);
      for (auto& existing : items) {
        if (existing.requestId == item.requestId) {
          existing = item;
          break;
        }
      }
      ReadBookYStorage::savePageItems(sidecar.journeyId, items);
      updateUnreadStateLocked(sidecar.journeyId, items);
    } else {
      LOG_INF("RBYPAGE", "Queued Page question offline (%s): %s", item.requestId.c_str(), subErr.c_str());
    }
  }

  outItem = item;
  return true;
}

bool ReadBookYPage::submitSingleRequestOnline(ReadBookYPageItem& item, std::string& errorOut) {
  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) return false;

  JsonDocument reqDoc;
  reqDoc["requestId"] = item.requestId;
  reqDoc["journeyId"] = item.journeyId;
  reqDoc["bookId"] = item.bookId;
  reqDoc["deviceId"] = READBOOKY_AUTH.deviceId();
  reqDoc["question"] = item.question;
  if (!item.selectedText.empty()) reqDoc["selectedText"] = item.selectedText;
  if (!item.threadId.empty()) reqDoc["threadId"] = item.threadId;
  if (item.spoilerOverride) reqDoc["spoilerOverride"] = true;
  if (item.queuedAtMs > 0) reqDoc["queuedAtMs"] = item.queuedAtMs;

  if (item.selectedAnchor.valid) {
    JsonObject sa = reqDoc["selectedAnchor"].to<JsonObject>();
    sa["spineIndex"] = item.selectedAnchor.spineIndex;
    sa["chapterHref"] = item.selectedAnchor.chapterHref;
    sa["sourceOffset"] = item.selectedAnchor.sourceOffset;
    sa["percentage"] = item.selectedAnchor.percentage;
  }

  // Send authoritative frozen local anchor at question time
  if (item.trustedAnchorAtQuestion.valid) {
    JsonObject ta = reqDoc["trustedAnchorAtQuestion"].to<JsonObject>();
    ta["spineIndex"] = item.trustedAnchorAtQuestion.spineIndex;
    ta["chapterHref"] = item.trustedAnchorAtQuestion.chapterHref;
    ta["sourceOffset"] = item.trustedAnchorAtQuestion.sourceOffset;
    ta["percentage"] = item.trustedAnchorAtQuestion.percentage;
    if (!item.trustedAnchorAtQuestion.chapterTitle.empty()) {
      ta["chapterTitle"] = item.trustedAnchorAtQuestion.chapterTitle;
    }
  }
  if (item.trustedOrdinalAtQuestion > 0) {
    reqDoc["trustedOrdinalAtQuestion"] = item.trustedOrdinalAtQuestion;
  }

  std::string body;
  serializeJson(reqDoc, body);

  const std::string url = std::string(READBOOKY_API_BASE_URL) + "/page/questions";
  const HttpResponse res = ReadBookYClient::postJson(url, body, idToken);
  ReadBookYAuth::secureScrub(idToken);

  if (!res.ok()) {
    errorOut = res.error.empty() ? "Page submit failed" : res.error;
    return false;
  }

  JsonDocument respDoc;
  if (!deserializeJson(respDoc, res.body)) {
    const bool wasReady = (item.status == "ready");
    applyServerItemFields(respDoc.as<JsonObjectConst>(), item);
    if (item.status == "ready" && !wasReady) {
      item.unread = true;
    }
  } else {
    item.status = "queued";
  }
  return true;
}

bool ReadBookYPage::pollSingleRequestOnline(ReadBookYPageItem& item, std::string& errorOut) {
  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) return false;

  const std::string url = std::string(READBOOKY_API_BASE_URL) + "/page/questions/" + item.requestId;
  const HttpResponse res = ReadBookYClient::getJson(url, idToken);
  ReadBookYAuth::secureScrub(idToken);

  if (!res.ok()) {
    errorOut = res.error.empty() ? "Page poll failed" : res.error;
    return false;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, res.body)) {
    errorOut = "Invalid Page poll response";
    return false;
  }

  const bool wasReady = (item.status == "ready");
  applyServerItemFields(respDoc.as<JsonObjectConst>(), item);
  if (item.status == "ready" && !wasReady) {
    item.unread = true;
  }
  return true;
}

bool ReadBookYPage::flushAndPollJourney(const std::string& journeyId, std::string& errorOut) {
  errorOut.clear();
  if (journeyId.empty() || WiFi.status() != WL_CONNECTED || !READBOOKY_AUTH.isSignedIn()) return false;

  std::vector<ReadBookYPageItem> items;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ReadBookYStorage::loadPageItems(journeyId, items);
  }

  bool anyChanged = false;
  for (auto& item : items) {
    if (item.status == "queued_offline") {
      std::string err;
      if (submitSingleRequestOnline(item, err)) {
        anyChanged = true;
      } else {
        errorOut = err;
      }
    } else if (item.status == "queued" || item.status == "working") {
      const std::string prevStatus = item.status;
      std::string err;
      if (pollSingleRequestOnline(item, err)) {
        if (item.status != prevStatus) anyChanged = true;
      } else {
        errorOut = err;
      }
    }
  }

  if (anyChanged) {
    std::lock_guard<std::mutex> lock(mutex_);
    ReadBookYStorage::savePageItems(journeyId, items);
    updateUnreadStateLocked(journeyId, items);
  }
  return anyChanged;
}

bool ReadBookYPage::refreshHistory(const std::string& journeyId, std::vector<ReadBookYPageItem>& outItems,
                                   std::string& errorOut) {
  errorOut.clear();
  if (journeyId.empty()) return false;

  // Flush offline queue & poll pending first
  std::string flushErr;
  flushAndPollJourney(journeyId, flushErr);

  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) {
    loadCachedItems(journeyId, outItems);
    return false;
  }

  const std::string url = std::string(READBOOKY_API_BASE_URL) + "/page/history?journeyId=" + journeyId + "&limit=30";
  const HttpResponse res = ReadBookYClient::getJson(url, idToken);
  ReadBookYAuth::secureScrub(idToken);

  if (!res.ok()) {
    errorOut = res.error.empty() ? "Failed fetching Page history" : res.error;
    loadCachedItems(journeyId, outItems);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, res.body)) {
    errorOut = "Invalid Page history JSON";
    loadCachedItems(journeyId, outItems);
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ReadBookYPageItem> existing;
  ReadBookYStorage::loadPageItems(journeyId, existing);

  std::vector<ReadBookYPageItem> merged;
  // Preserve any still-unsent local offline questions first
  for (const auto& loc : existing) {
    if (loc.status == "queued_offline") {
      merged.push_back(loc);
    }
  }

  auto findExistingUnread = [&](const std::string& reqId) -> bool {
    for (const auto& e : existing) {
      if (!reqId.empty() && e.requestId == reqId) return e.unread;
    }
    return false;
  };

  auto addServerItem = [&](JsonObjectConst o) {
    ReadBookYPageItem it;
    it.requestId = o["requestId"] | (o["id"] | "");
    it.threadId = o["threadId"] | "";
    it.journeyId = o["journeyId"] | journeyId;
    it.bookId = o["bookId"] | "";
    it.status = o["status"] | "ready";
    it.question = o["question"] | "";
    it.selectedText = o["selectedText"] | "";
    it.answer = o["answer"] | "";
    it.cautious = o["cautious"] | false;
    it.answerConfidence = o["answerConfidence"] | "";
    it.trustedOrdinalUsed = o["trustedOrdinalUsed"] | 0U;
    it.boundaryLabel = o["boundaryLabel"] | "";
    it.retryable = o["retryable"] | false;
    it.errorMessage = o["errorMessage"] | "";
    it.queuedAtMs = o["queuedAtMs"] | (o["createdAtMs"] | 0LL);
    it.completedAtMs = o["completedAtMs"] | 0LL;
    it.unread = (o["unread"] | false) || findExistingUnread(it.requestId);
    parseCitations(o["citations"].as<JsonArrayConst>(), it.citations);

    for (const auto& m : merged) {
      if (!it.requestId.empty() && m.requestId == it.requestId) return;
    }
    merged.push_back(std::move(it));
  };

  for (JsonObjectConst o : doc["pendingRequests"].as<JsonArrayConst>()) {
    addServerItem(o);
  }
  for (JsonObjectConst o : doc["items"].as<JsonArrayConst>()) {
    addServerItem(o);
  }

  ReadBookYStorage::savePageItems(journeyId, merged);
  updateUnreadStateLocked(journeyId, merged);
  outItems = std::move(merged);
  return true;
}

void ReadBookYPage::markRead(const std::string& journeyId, const std::string& requestId) {
  if (journeyId.empty() || requestId.empty()) return;

  bool shouldAckOnline = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReadBookYPageItem> items;
    if (ReadBookYStorage::loadPageItems(journeyId, items)) {
      bool changed = false;
      for (auto& it : items) {
        if (it.requestId == requestId && it.unread) {
          it.unread = false;
          changed = true;
          shouldAckOnline = true;
        }
      }
      if (changed) {
        ReadBookYStorage::savePageItems(journeyId, items);
      }
      updateUnreadStateLocked(journeyId, items);
    }
  }

  if (shouldAckOnline && WiFi.status() == WL_CONNECTED && READBOOKY_AUTH.isSignedIn()) {
    std::string idToken, err;
    if (READBOOKY_AUTH.ensureIdToken(idToken, err)) {
      const std::string url = std::string(READBOOKY_API_BASE_URL) + "/page/questions/" + requestId + "?ack=1";
      ReadBookYClient::getJson(url, idToken);
      ReadBookYAuth::secureScrub(idToken);
    }
  }
}

bool ReadBookYPage::retryFailedQuestion(const std::string& journeyId, const std::string& oldRequestId,
                                        ReadBookYPageItem& outItem, bool& submittedOnlineOut, std::string& errorOut) {
  submittedOnlineOut = false;
  errorOut.clear();

  ReadBookYPageItem cloned;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ReadBookYPageItem> items;
    ReadBookYStorage::loadPageItems(journeyId, items);
    for (auto& it : items) {
      if (it.requestId == oldRequestId) {
        cloned = it;
        cloned.requestId = "pgr_" + ReadBookYAnchors::generateUuidV4();
        cloned.status = "queued_offline";
        cloned.errorMessage.clear();
        cloned.retryable = false;
        it = cloned;
        found = true;
        break;
      }
    }
    if (found) {
      ReadBookYStorage::savePageItems(journeyId, items);
    }
  }

  if (!found) {
    errorOut = "Question not found";
    return false;
  }

  if (WiFi.status() == WL_CONNECTED && READBOOKY_AUTH.isSignedIn()) {
    if (submitSingleRequestOnline(cloned, errorOut)) {
      submittedOnlineOut = true;
      std::lock_guard<std::mutex> lock(mutex_);
      std::vector<ReadBookYPageItem> items;
      ReadBookYStorage::loadPageItems(journeyId, items);
      for (auto& it : items) {
        if (it.requestId == cloned.requestId) {
          it = cloned;
          break;
        }
      }
      ReadBookYStorage::savePageItems(journeyId, items);
    }
  }

  outItem = cloned;
  return true;
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
