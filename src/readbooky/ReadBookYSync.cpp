#include "ReadBookYSync.h"

#ifdef ENABLE_READBOOKY

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "ReadBookYAnchors.h"
#include "ReadBookYAuth.h"
#include "ReadBookYClient.h"
#include "ReadBookYConfig.h"
#include "ReadBookYOutbox.h"
#include "ReadBookYPage.h"
#include "ReadBookYStorage.h"

namespace readbooky {

namespace {

int64_t nowEpochMs() {
  const int64_t s = trustedtime::trustedNow();
  return s > 0 ? s * 1000LL : 0LL;
}

bool isNonSequentialReason(const std::string& reason) {
  return reason == "jump" || reason == "toc" || reason == "search" || reason == "bookmark" || reason == "footnote" ||
         reason == "external";
}

ReadBookYAnchor parseAnchorJson(JsonObjectConst o, const std::string& fallbackJourney,
                                const std::string& fallbackBook) {
  ReadBookYAnchor a;
  a.id = o["id"] | "";
  a.journeyId = o["journeyId"] | fallbackJourney;
  a.bookId = o["bookId"] | fallbackBook;
  a.scope = o["scope"] | "public";
  a.ownerUid = o["ownerUid"] | "";
  a.isPrivate = o["isPrivate"] | false;
  for (JsonVariantConst v : o["visibleTo"].as<JsonArrayConst>()) {
    if (v.is<const char*>()) a.visibleTo.emplace_back(v.as<const char*>());
  }
  a.spineIndex = o["spineIndex"] | 0;
  a.chapterHref = o["chapterHref"] | "";
  a.cfiRange = o["cfiRange"] | "";
  a.sourceStartOffset = o["sourceStartOffset"].isNull() ? UINT32_MAX : (o["sourceStartOffset"] | UINT32_MAX);
  a.sourceEndOffset = o["sourceEndOffset"].isNull() ? UINT32_MAX : (o["sourceEndOffset"] | UINT32_MAX);
  a.ordinal = o["ordinal"] | 0U;
  a.anchorText = o["anchorText"] | "";
  a.progressPercent = o["progressPercent"] | 0.0f;
  a.updatedAtMs = o["updatedAtMs"] | 0LL;
  a.lastCommentAtMs = o["lastCommentAtMs"] | 0LL;
  return a;
}

ReadBookYHighlight parseHighlightJson(JsonObjectConst o, const std::string& fallbackJourney,
                                      const std::string& fallbackBook) {
  ReadBookYHighlight h;
  h.id = o["id"] | "";
  h.anchorId = o["anchorId"] | "";
  h.journeyId = o["journeyId"] | fallbackJourney;
  h.bookId = o["bookId"] | fallbackBook;
  h.uid = o["uid"] | "";
  h.spineIndex = o["spineIndex"] | 0;
  h.chapterHref = o["chapterHref"] | "";
  h.cfiRange = o["cfiRange"] | "";
  h.sourceStartOffset = o["sourceStartOffset"].isNull() ? UINT32_MAX : (o["sourceStartOffset"] | UINT32_MAX);
  h.sourceEndOffset = o["sourceEndOffset"].isNull() ? UINT32_MAX : (o["sourceEndOffset"] | UINT32_MAX);
  h.ordinal = o["ordinal"] | 0U;
  h.selectedText = o["selectedText"] | "";
  h.contextBefore = o["contextBefore"] | "";
  h.contextAfter = o["contextAfter"] | "";
  h.color = o["color"] | "#999999";
  h.scope = o["scope"] | "public";
  h.isPrivate = o["isPrivate"] | false;
  for (JsonVariantConst v : o["visibleTo"].as<JsonArrayConst>()) {
    if (v.is<const char*>()) h.visibleTo.emplace_back(v.as<const char*>());
  }
  h.anchorResolution = o["anchorResolution"] | "exact";
  h.createdAtMs = o["createdAtMs"] | 0LL;
  h.updatedAtMs = o["updatedAtMs"] | 0LL;
  return h;
}

ReadBookYComment parseCommentJson(JsonObjectConst o, const std::string& fallbackJourney,
                                  const std::string& fallbackBook) {
  ReadBookYComment c;
  c.id = o["id"] | "";
  c.anchorId = o["anchorId"] | "";
  c.highlightId = o["highlightId"] | "";
  c.journeyId = o["journeyId"] | fallbackJourney;
  c.bookId = o["bookId"] | fallbackBook;
  c.uid = o["uid"] | "";
  c.authorName = o["authorName"] | "Reader";
  c.content = o["content"] | "";
  c.parentCommentId = o["parentCommentId"] | "";
  c.rootCommentId = o["rootCommentId"] | "";
  c.scope = o["scope"] | "public";
  c.isPrivate = o["isPrivate"] | false;
  for (JsonVariantConst v : o["visibleTo"].as<JsonArrayConst>()) {
    if (v.is<const char*>()) c.visibleTo.emplace_back(v.as<const char*>());
  }
  JsonObjectConst rx = o["reactions"].as<JsonObjectConst>();
  if (!rx.isNull()) {
    for (size_t k = 0; k < REACTION_COUNT; ++k) {
      for (JsonVariantConst v : rx[REACTION_KEYS[k]].as<JsonArrayConst>()) {
        if (v.is<const char*>()) c.reactions[k].emplace_back(v.as<const char*>());
      }
    }
  }
  c.createdAtMs = o["createdAtMs"] | 0LL;
  c.updatedAtMs = o["updatedAtMs"] | 0LL;
  return c;
}

template <typename T>
void upsertById(std::vector<T>& list, T&& item) {
  for (auto& existing : list) {
    if (existing.id == item.id) {
      existing = std::move(item);
      return;
    }
  }
  list.push_back(std::move(item));
}

template <typename T>
void removeById(std::vector<T>& list, const std::string& id) {
  list.erase(std::remove_if(list.begin(), list.end(), [&](const T& item) { return item.id == id; }), list.end());
}

struct BgSyncArg {
  std::string priorityJourneyId;
};

void parsePositionFromJson(JsonObjectConst posObj, ReadBookYPosition& pos) {
  if (posObj.isNull()) return;
  const uint32_t srvTrustedOrd = posObj["trustedOrdinal"] | 0U;
  if (srvTrustedOrd > 0) {
    pos.trustedOrdinal = srvTrustedOrd;
    pos.trustedPercent = posObj["trustedPercent"] | pos.trustedPercent;
    const std::string ch = posObj["trustedChapterTitle"] | "";
    if (!ch.empty()) pos.trustedChapterTitle = ch;
    const std::string src = posObj["trustedPositionSource"] | "";
    if (!src.empty()) pos.trustedPositionSource = src;
  }
  const int64_t srvUpd = posObj["updatedAtMs"] | 0LL;
  if (srvUpd > pos.updatedAtMs) {
    pos.spineIndex = posObj["spineIndex"] | pos.spineIndex;
    const std::string href = posObj["chapterHref"] | "";
    if (!href.empty()) pos.chapterHref = href;
    pos.sourceOffset = posObj["sourceOffset"] | pos.sourceOffset;
    pos.progressPercent = posObj["progressPercent"] | pos.progressPercent;
    const std::string cfi = posObj["currentCfi"] | "";
    if (!cfi.empty()) pos.currentCfi = cfi;
    pos.updatedAtMs = srvUpd;
  }
}

void applySnapshotToCache(JsonObjectConst snapObj, ReadBookYBookCache& cache) {
  cache.anchors.clear();
  cache.highlights.clear();
  cache.comments.clear();

  for (JsonObjectConst o : snapObj["anchors"].as<JsonArrayConst>()) {
    auto a = parseAnchorJson(o, cache.journeyId, cache.bookId);
    if (!a.id.empty()) cache.anchors.push_back(std::move(a));
  }
  for (JsonObjectConst o : snapObj["highlights"].as<JsonArrayConst>()) {
    auto h = parseHighlightJson(o, cache.journeyId, cache.bookId);
    if (!h.id.empty()) cache.highlights.push_back(std::move(h));
  }
  for (JsonObjectConst o : snapObj["comments"].as<JsonArrayConst>()) {
    auto c = parseCommentJson(o, cache.journeyId, cache.bookId);
    if (!c.id.empty()) cache.comments.push_back(std::move(c));
  }
  parsePositionFromJson(snapObj["position"].as<JsonObjectConst>(), cache.position);
}

void applyDeltaChangesToCache(JsonArrayConst changesArr, ReadBookYBookCache& cache) {
  if (changesArr.isNull()) return;
  for (JsonObjectConst ch : changesArr) {
    const std::string entityType = ch["entityType"] | "";
    const std::string entityId = ch["entityId"] | "";
    const std::string op = ch["op"] | "";
    if (entityId.empty()) continue;

    if (op == "delete") {
      if (entityType == "highlight") {
        removeById(cache.highlights, entityId);
      } else if (entityType == "comment") {
        removeById(cache.comments, entityId);
      } else if (entityType == "anchor") {
        removeById(cache.anchors, entityId);
      }
      continue;
    }

    JsonObjectConst data = ch["data"].as<JsonObjectConst>();
    if (data.isNull()) continue;

    if (entityType == "highlight") {
      auto h = parseHighlightJson(data, cache.journeyId, cache.bookId);
      if (!h.id.empty()) upsertById(cache.highlights, std::move(h));
    } else if (entityType == "comment") {
      auto c = parseCommentJson(data, cache.journeyId, cache.bookId);
      if (!c.id.empty()) upsertById(cache.comments, std::move(c));
    } else if (entityType == "anchor") {
      auto a = parseAnchorJson(data, cache.journeyId, cache.bookId);
      if (!a.id.empty()) upsertById(cache.anchors, std::move(a));
    }
  }
}

}  // namespace

ReadBookYSync& ReadBookYSync::getInstance() {
  static ReadBookYSync inst;
  return inst;
}

void ReadBookYSync::begin() {
  ReadBookYStorage::ensureDirectories();
  READBOOKY_AUTH.begin();
  READBOOKY_OUTBOX.begin();
  READBOOKY_PAGE.begin();
}

bool ReadBookYSync::isAnchorFurtherForward(const ReadBookYAnchorRef& candidate, const ReadBookYAnchorRef& existing) {
  if (!candidate.valid) return false;
  if (!existing.valid) return true;
  if (candidate.spineIndex != existing.spineIndex) return candidate.spineIndex > existing.spineIndex;
  if (candidate.sourceOffset != existing.sourceOffset) return candidate.sourceOffset > existing.sourceOffset;
  return candidate.percentage > existing.percentage;
}

bool ReadBookYSync::fetchLibrary(std::vector<ReadBookYBookMeta>& outBooks, std::string& errorOut) {
  errorOut.clear();
  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) {
    ReadBookYStorage::loadLibraryCache(outBooks);
    return false;
  }

  // Build map of previously known local paths so downloaded status stays instant
  std::vector<ReadBookYBookMeta> prevBooks;
  ReadBookYStorage::loadLibraryCache(prevBooks);
  std::unordered_map<std::string, std::string> localPathByJourney;
  for (const auto& pb : prevBooks) {
    if (!pb.localPath.empty() && Storage.exists(pb.localPath.c_str())) {
      localPathByJourney[pb.journeyId] = pb.localPath;
    }
  }

  std::vector<ReadBookYBookMeta> fetched;
  std::string cursor;
  for (int page = 0; page < 10; ++page) {
    std::string url = std::string(READBOOKY_API_BASE_URL) + "/library?limit=30";
    if (!cursor.empty()) url += "&cursor=" + cursor;

    const HttpResponse res = ReadBookYClient::getJson(url, idToken);
    if (!res.ok()) {
      ReadBookYAuth::secureScrub(idToken);
      errorOut = res.error.empty() ? "Library fetch failed" : res.error;
      ReadBookYStorage::loadLibraryCache(outBooks);
      return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) {
      ReadBookYAuth::secureScrub(idToken);
      errorOut = "Invalid library response";
      ReadBookYStorage::loadLibraryCache(outBooks);
      return false;
    }

    for (JsonObjectConst o : doc["books"].as<JsonArrayConst>()) {
      ReadBookYBookMeta b;
      b.journeyId = o["journeyId"] | "";
      b.journeyName = o["journeyName"] | "";
      b.bookId = o["bookId"] | "";
      b.editionId = o["editionId"] | "";
      b.workId = o["workId"] | "";
      b.title = o["title"] | "Untitled";
      b.author = o["author"] | "";
      b.language = o["language"] | "en";
      b.coverUrl = o["coverUrl"] | "";
      b.fileSize = o["fileSize"] | 0ULL;
      b.epubSha256 = o["epubSha256"] | "";
      b.pageReady = o["pageReady"] | false;
      b.participantCount = o["participantCount"] | 1;
      b.updatedAtMs = o["updatedAtMs"] | 0LL;
      parsePositionFromJson(o["progress"].as<JsonObjectConst>(), b.progress);

      auto it = localPathByJourney.find(b.journeyId);
      if (it != localPathByJourney.end()) {
        b.localPath = it->second;
      } else {
        const std::string candidate =
            ReadBookYStorage::buildSafeEpubPath(b.title, b.editionId.empty() ? b.bookId : b.editionId);
        if (Storage.exists(candidate.c_str())) {
          b.localPath = candidate;
        }
      }
      b.downloaded = !b.localPath.empty() && Storage.exists(b.localPath.c_str());
      if (!b.journeyId.empty()) fetched.push_back(std::move(b));
    }

    const char* next = doc["nextCursor"].as<const char*>();
    if (!next || next[0] == '\0') break;
    cursor = next;
  }

  ReadBookYAuth::secureScrub(idToken);
  ReadBookYStorage::saveLibraryCache(fetched);
  outBooks = std::move(fetched);
  return true;
}

bool ReadBookYSync::downloadBook(ReadBookYBookMeta& bookInOut, std::string& errorOut,
                                 const DownloadProgressCallback& onProgress) {
  errorOut.clear();
  ReadBookYStorage::ensureDirectories();

  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) return false;

  const std::string targetId = !bookInOut.editionId.empty() ? bookInOut.editionId : bookInOut.bookId;
  const std::string url = std::string(READBOOKY_API_BASE_URL) + "/books/" + targetId + "/download";
  const HttpResponse res = ReadBookYClient::getJson(url, idToken);
  ReadBookYAuth::secureScrub(idToken);

  if (!res.ok()) {
    errorOut = res.error.empty() ? "Download authorization failed" : res.error;
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, res.body)) {
    errorOut = "Invalid download metadata";
    return false;
  }

  const std::string signedUrl = doc["downloadUrl"] | "";
  const std::string sha256 = doc["epubSha256"] | bookInOut.epubSha256;
  const uint64_t fileSize = doc["fileSize"] | bookInOut.fileSize;
  const std::string editionId = doc["editionId"] | bookInOut.editionId;
  const std::string workId = doc["workId"] | bookInOut.workId;

  if (signedUrl.empty()) {
    errorOut = "Missing download URL";
    return false;
  }

  const std::string destPath = ReadBookYStorage::buildSafeEpubPath(
      bookInOut.title, !editionId.empty() ? editionId : bookInOut.bookId);

  if (!ReadBookYClient::downloadFileResumable(signedUrl, destPath, sha256, fileSize, errorOut, onProgress)) {
    return false;
  }

  ReadBookYSidecar sidecar;
  sidecar.journeyId = bookInOut.journeyId;
  sidecar.bookId = bookInOut.bookId;
  sidecar.editionId = editionId;
  sidecar.workId = workId;
  sidecar.title = bookInOut.title;
  sidecar.author = bookInOut.author;
  sidecar.epubSha256 = sha256;
  sidecar.fileSize = fileSize;
  sidecar.downloadedAtMs = nowEpochMs();
  ReadBookYStorage::saveSidecar(destPath, sidecar);

  bookInOut.localPath = destPath;
  bookInOut.editionId = editionId;
  bookInOut.workId = workId;
  bookInOut.epubSha256 = sha256;
  bookInOut.fileSize = fileSize;
  bookInOut.downloaded = true;

  // Update cached library entry + CrossPoint RecentBooksStore
  std::vector<ReadBookYBookMeta> lib;
  if (ReadBookYStorage::loadLibraryCache(lib)) {
    for (auto& b : lib) {
      if (b.journeyId == bookInOut.journeyId) {
        b = bookInOut;
        break;
      }
    }
    ReadBookYStorage::saveLibraryCache(lib);
  }
  RECENT_BOOKS.addBook(destPath, bookInOut.title, bookInOut.author, "");

  // Pull initial social & position snapshot for this book right after download
  std::string syncErr;
  syncJourneyNow(bookInOut.journeyId, bookInOut.bookId, bookInOut.editionId, /*mirrorClippings=*/false, syncErr);
  return true;
}

bool ReadBookYSync::fetchDictionaries(std::vector<ReadBookYDictionaryPack>& outPacks, std::string& errorOut) {
  outPacks.clear();
  errorOut.clear();
  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) return false;

  const std::string url = std::string(READBOOKY_API_BASE_URL) + "/dictionaries";
  const HttpResponse res = ReadBookYClient::getJson(url, idToken);
  ReadBookYAuth::secureScrub(idToken);

  if (!res.ok()) {
    errorOut = res.error.empty() ? "Dictionary list failed" : res.error;
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, res.body)) {
    errorOut = "Invalid dictionary manifest";
    return false;
  }

  if (!(doc["available"] | false)) {
    errorOut = doc["message"] | "No dictionary pack configured on server";
    return true;
  }

  for (JsonObjectConst o : doc["dictionaries"].as<JsonArrayConst>()) {
    ReadBookYDictionaryPack pack;
    pack.id = o["id"] | "";
    pack.name = o["name"] | pack.id;
    pack.language = o["language"] | "en";
    pack.version = o["version"] | "1.0";
    pack.license = o["license"] | "";
    pack.attribution = o["attribution"] | "";
    pack.totalBytes = o["totalBytes"] | 0ULL;
    for (JsonObjectConst fObj : o["files"].as<JsonArrayConst>()) {
      ReadBookYDictionaryFile f;
      f.name = fObj["name"] | "";
      f.size = fObj["size"] | 0ULL;
      f.sha256 = fObj["sha256"] | "";
      f.downloadUrl = fObj["downloadUrl"] | "";
      if (!f.name.empty() && !f.downloadUrl.empty()) pack.files.push_back(std::move(f));
    }
    if (!pack.id.empty() && !pack.files.empty()) {
      outPacks.push_back(std::move(pack));
    }
  }
  return true;
}

bool ReadBookYSync::installDictionaryPack(const ReadBookYDictionaryPack& pack, std::string& errorOut,
                                          const DownloadProgressCallback& onProgress) {
  errorOut.clear();
  if (pack.id.empty() || pack.files.empty()) {
    errorOut = "Empty dictionary pack";
    return false;
  }

  Storage.ensureDirectoryExists(DICTIONARY_DIR);
  const std::string packDir = std::string(DICTIONARY_DIR) + "/" + ReadBookYStorage::sanitizeFileSegment(pack.id, 48);
  Storage.ensureDirectoryExists(packDir.c_str());

  uint64_t cumulativeDone = 0;
  for (const auto& f : pack.files) {
    const std::string destPath = packDir + "/" + f.name;
    auto cb = [&](uint64_t done, uint64_t /*fileTotal*/) {
      if (onProgress) onProgress(cumulativeDone + done, pack.totalBytes);
    };
    if (!ReadBookYClient::downloadFileResumable(f.downloadUrl, destPath, f.sha256, f.size, errorOut, cb)) {
      return false;
    }
    cumulativeDone += f.size;
  }

  // Persist license/attribution notice alongside the StarDict pack
  if (!pack.license.empty() || !pack.attribution.empty()) {
    const std::string licContent = "Dictionary: " + pack.name + " (" + pack.version + ")\nLicense: " + pack.license +
                                   "\nAttribution: " + pack.attribution + "\n";
    ReadBookYStorage::writeAtomicString(packDir + "/ATTRIBUTION.txt", licContent);
  }

  // Select this dictionary in CrossPoint settings if user hasn't picked one yet
  if (SETTINGS.dictionaryName[0] == '\0') {
    snprintf(SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName), "%s",
             ReadBookYStorage::sanitizeFileSegment(pack.id, 48).c_str());
    SETTINGS.saveToFile();
  }
  return true;
}

bool ReadBookYSync::recordReadingProgress(const ReadBookYSidecar& sidecar, const int spineIndex,
                                          const std::string& chapterHref, const uint32_t sourceOffset,
                                          const float percentage, const std::string& chapterTitle,
                                          const std::string& navigationReason, const bool manualOverride) {
  if (sidecar.journeyId.empty()) return false;

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  cache.journeyId = sidecar.journeyId;
  cache.bookId = sidecar.bookId;
  cache.editionId = sidecar.editionId;

  const int64_t ts = nowEpochMs();
  cache.position.spineIndex = spineIndex;
  cache.position.chapterHref = chapterHref;
  cache.position.sourceOffset = sourceOffset;
  cache.position.progressPercent = percentage;
  cache.position.updatedAtMs = ts;

  ReadBookYAnchorRef currentAnchor;
  currentAnchor.spineIndex = spineIndex;
  currentAnchor.chapterHref = chapterHref;
  currentAnchor.sourceOffset = sourceOffset;
  currentAnchor.percentage = percentage;
  currentAnchor.chapterTitle = chapterTitle;
  currentAnchor.valid = true;

  // Enforce PAGE_SPEC.md trusted-position rules locally so offline sequential reading
  // accumulates accurately while jumps/TOC/search/bookmarks/footnotes never raise the spoiler bound!
  if (manualOverride || navigationReason == "manual_override") {
    cache.position.localTrustedAnchor = currentAnchor;
    cache.position.trustedPercent = percentage;
    if (!chapterTitle.empty()) cache.position.trustedChapterTitle = chapterTitle;
    cache.position.trustedPositionSource = "manual_override";
  } else if (!isNonSequentialReason(navigationReason)) {
    if (isAnchorFurtherForward(currentAnchor, cache.position.localTrustedAnchor)) {
      cache.position.localTrustedAnchor = currentAnchor;
      cache.position.trustedPercent = percentage;
      if (!chapterTitle.empty()) cache.position.trustedChapterTitle = chapterTitle;
      cache.position.trustedPositionSource = "sequential";
    }
  }

  ReadBookYStorage::saveBookCache(cache);

  // Enqueue durable coalescing progress op containing BOTH currentAnchor and authoritative localTrustedAnchor
  JsonDocument pDoc;
  pDoc["navigationReason"] = manualOverride ? "manual_override" : navigationReason;
  pDoc["manualOverride"] = manualOverride;
  pDoc["trustedPositionSource"] = cache.position.trustedPositionSource;

  JsonObject cur = pDoc["currentAnchor"].to<JsonObject>();
  cur["spineIndex"] = currentAnchor.spineIndex;
  cur["chapterHref"] = currentAnchor.chapterHref;
  cur["sourceOffset"] = currentAnchor.sourceOffset;
  cur["percentage"] = currentAnchor.percentage;

  if (cache.position.localTrustedAnchor.valid) {
    JsonObject trObj = pDoc["trustedAnchor"].to<JsonObject>();
    trObj["spineIndex"] = cache.position.localTrustedAnchor.spineIndex;
    trObj["chapterHref"] = cache.position.localTrustedAnchor.chapterHref;
    trObj["sourceOffset"] = cache.position.localTrustedAnchor.sourceOffset;
    trObj["percentage"] = cache.position.localTrustedAnchor.percentage;
  }

  std::string payloadJson;
  serializeJson(pDoc, payloadJson);

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = "update_progress";
  op.entityId = sidecar.journeyId;
  op.payloadJson = std::move(payloadJson);
  op.createdAtMs = ts;
  return READBOOKY_OUTBOX.enqueue(op);
}

bool ReadBookYSync::createHighlightLocal(const ReadBookYSidecar& sidecar, const int spineIndex,
                                         const std::string& chapterHref, const uint32_t startOffset,
                                         const uint32_t endOffset, const std::string& selectedText,
                                         const std::string& contextBefore, const std::string& contextAfter,
                                         const std::string& scope, const bool isPrivate,
                                         ReadBookYHighlight& outHighlight) {
  if (sidecar.journeyId.empty() || selectedText.empty()) return false;

  const ReadBookYSession sess = READBOOKY_AUTH.session();
  const int64_t ts = nowEpochMs();
  const std::string hlId = "hl_" + ReadBookYAnchors::generateUuidV4();
  const std::string tempAnchorId = "anc_loc_" + ReadBookYAnchors::generateUuidV4();

  ReadBookYHighlight hl;
  hl.id = hlId;
  hl.anchorId = tempAnchorId;
  hl.journeyId = sidecar.journeyId;
  hl.bookId = sidecar.bookId;
  hl.uid = sess.uid;
  hl.spineIndex = spineIndex;
  hl.chapterHref = chapterHref;
  hl.sourceStartOffset = startOffset;
  hl.sourceEndOffset = endOffset;
  hl.selectedText = selectedText;
  hl.contextBefore = contextBefore;
  hl.contextAfter = contextAfter;
  hl.color = isPrivate ? "#666666" : sess.color;
  hl.scope = isPrivate ? "private" : (scope.empty() ? "public" : scope);
  hl.isPrivate = isPrivate || hl.scope == "private";
  hl.anchorResolution = (startOffset != UINT32_MAX && endOffset > startOffset) ? "exact" : "text_fallback";
  hl.createdAtMs = ts;
  hl.updatedAtMs = ts;

  ReadBookYAnchor anc;
  anc.id = tempAnchorId;
  anc.journeyId = sidecar.journeyId;
  anc.bookId = sidecar.bookId;
  anc.scope = hl.scope;
  anc.ownerUid = hl.isPrivate ? sess.uid : "";
  anc.isPrivate = hl.isPrivate;
  anc.spineIndex = spineIndex;
  anc.chapterHref = chapterHref;
  anc.sourceStartOffset = startOffset;
  anc.sourceEndOffset = endOffset;
  anc.anchorText = selectedText;
  anc.updatedAtMs = ts;

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  cache.journeyId = sidecar.journeyId;
  cache.bookId = sidecar.bookId;
  cache.editionId = sidecar.editionId;
  upsertById(cache.anchors, std::move(anc));
  upsertById(cache.highlights, ReadBookYHighlight(hl));
  ReadBookYStorage::saveBookCache(cache);
  ReadBookYAnchors::mirrorToCrossPointClippings(cache);

  JsonDocument pDoc;
  pDoc["highlightId"] = hl.id;
  pDoc["anchorId"] = hl.anchorId;
  pDoc["spineIndex"] = hl.spineIndex;
  pDoc["chapterHref"] = hl.chapterHref;
  if (hl.sourceStartOffset != UINT32_MAX) pDoc["sourceStartOffset"] = hl.sourceStartOffset;
  if (hl.sourceEndOffset != UINT32_MAX) pDoc["sourceEndOffset"] = hl.sourceEndOffset;
  pDoc["selectedText"] = hl.selectedText;
  pDoc["anchorText"] = hl.selectedText;
  if (!hl.contextBefore.empty()) pDoc["contextBefore"] = hl.contextBefore;
  if (!hl.contextAfter.empty()) pDoc["contextAfter"] = hl.contextAfter;
  pDoc["scope"] = hl.scope;
  pDoc["isPrivate"] = hl.isPrivate;
  pDoc["color"] = hl.color;

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = "create_highlight";
  op.entityId = hl.id;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = ts;

  outHighlight = std::move(hl);
  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::deleteHighlightLocal(const ReadBookYSidecar& sidecar, const std::string& highlightId) {
  if (sidecar.journeyId.empty() || highlightId.empty()) return false;

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  std::string anchorId;
  for (const auto& h : cache.highlights) {
    if (h.id == highlightId) {
      anchorId = h.anchorId;
      break;
    }
  }
  removeById(cache.highlights, highlightId);
  ReadBookYStorage::saveBookCache(cache);
  ReadBookYAnchors::mirrorToCrossPointClippings(cache);

  JsonDocument pDoc;
  pDoc["highlightId"] = highlightId;
  if (!anchorId.empty()) pDoc["anchorId"] = anchorId;

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = "delete_highlight";
  op.entityId = highlightId;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = nowEpochMs();

  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::updateVisibilityLocal(const ReadBookYSidecar& sidecar, const std::string& anchorId,
                                          const std::string& highlightId, const std::string& scope,
                                          const bool isPrivate) {
  if (sidecar.journeyId.empty() || (anchorId.empty() && highlightId.empty())) return false;

  const std::string resolvedScope = isPrivate ? "private" : (scope.empty() ? "public" : scope);
  const bool resolvedPrivate = (resolvedScope == "private");

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  for (auto& a : cache.anchors) {
    if (a.id == anchorId) {
      a.scope = resolvedScope;
      a.isPrivate = resolvedPrivate;
    }
  }
  for (auto& h : cache.highlights) {
    if (h.anchorId == anchorId || (!highlightId.empty() && h.id == highlightId)) {
      h.scope = resolvedScope;
      h.isPrivate = resolvedPrivate;
    }
  }
  for (auto& c : cache.comments) {
    if (c.anchorId == anchorId) {
      c.scope = resolvedScope;
      c.isPrivate = resolvedPrivate;
    }
  }
  ReadBookYStorage::saveBookCache(cache);

  JsonDocument pDoc;
  if (!anchorId.empty()) pDoc["anchorId"] = anchorId;
  if (!highlightId.empty()) pDoc["highlightId"] = highlightId;
  pDoc["scope"] = resolvedScope;
  pDoc["isPrivate"] = resolvedPrivate;

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = "update_highlight_visibility";
  op.entityId = !anchorId.empty() ? anchorId : highlightId;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = nowEpochMs();

  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::createCommentLocal(const ReadBookYSidecar& sidecar, const std::string& anchorId,
                                       const std::string& highlightId, const std::string& content,
                                       const std::string& parentCommentId, const std::string& rootCommentId,
                                       ReadBookYComment& outComment) {
  if (sidecar.journeyId.empty() || anchorId.empty() || content.empty()) return false;

  const ReadBookYSession sess = READBOOKY_AUTH.session();
  const int64_t ts = nowEpochMs();
  const std::string cmId = "cm_" + ReadBookYAnchors::generateUuidV4();

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);

  std::string scope = "public";
  bool isPrivate = false;
  for (const auto& a : cache.anchors) {
    if (a.id == anchorId) {
      scope = a.scope;
      isPrivate = a.isPrivate;
      break;
    }
  }

  ReadBookYComment c;
  c.id = cmId;
  c.anchorId = anchorId;
  c.highlightId = highlightId;
  c.journeyId = sidecar.journeyId;
  c.bookId = sidecar.bookId;
  c.uid = sess.uid;
  c.authorName = sess.displayName.empty() ? "You" : sess.displayName;
  c.content = content;
  c.parentCommentId = parentCommentId;
  c.rootCommentId = parentCommentId.empty() ? "" : (rootCommentId.empty() ? parentCommentId : rootCommentId);
  c.scope = scope;
  c.isPrivate = isPrivate;
  c.createdAtMs = ts;
  c.updatedAtMs = ts;

  cache.comments.push_back(c);
  ReadBookYStorage::saveBookCache(cache);

  JsonDocument pDoc;
  pDoc["commentId"] = c.id;
  pDoc["anchorId"] = c.anchorId;
  if (!c.highlightId.empty()) pDoc["highlightId"] = c.highlightId;
  pDoc["content"] = c.content;
  if (!c.parentCommentId.empty()) pDoc["parentCommentId"] = c.parentCommentId;
  if (!c.rootCommentId.empty()) pDoc["rootCommentId"] = c.rootCommentId;
  pDoc["scope"] = c.scope;
  pDoc["isPrivate"] = c.isPrivate;

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = parentCommentId.empty() ? "create_comment" : "create_reply";
  op.entityId = c.id;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = ts;

  outComment = std::move(c);
  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::editCommentLocal(const ReadBookYSidecar& sidecar, const std::string& commentId,
                                     const std::string& newContent, const bool isReply) {
  if (sidecar.journeyId.empty() || commentId.empty() || newContent.empty()) return false;

  const int64_t ts = nowEpochMs();
  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  for (auto& c : cache.comments) {
    if (c.id == commentId) {
      c.content = newContent;
      c.updatedAtMs = ts;
      break;
    }
  }
  ReadBookYStorage::saveBookCache(cache);

  JsonDocument pDoc;
  pDoc["commentId"] = commentId;
  pDoc["content"] = newContent;

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = isReply ? "edit_reply" : "edit_comment";
  op.entityId = commentId;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = ts;

  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::deleteCommentLocal(const ReadBookYSidecar& sidecar, const std::string& commentId,
                                       const bool isReply) {
  if (sidecar.journeyId.empty() || commentId.empty()) return false;

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  cache.comments.erase(std::remove_if(cache.comments.begin(), cache.comments.end(),
                                      [&](const ReadBookYComment& c) {
                                        return c.id == commentId || c.parentCommentId == commentId;
                                      }),
                       cache.comments.end());
  ReadBookYStorage::saveBookCache(cache);

  JsonDocument pDoc;
  pDoc["commentId"] = commentId;

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = isReply ? "delete_reply" : "delete_comment";
  op.entityId = commentId;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = nowEpochMs();

  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::toggleReactionLocal(const ReadBookYSidecar& sidecar, const std::string& commentId,
                                        const size_t reactionIndex) {
  if (sidecar.journeyId.empty() || commentId.empty() || reactionIndex >= REACTION_COUNT) return false;

  const std::string myUid = READBOOKY_AUTH.session().uid;
  if (myUid.empty()) return false;

  bool adding = true;
  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(sidecar.journeyId, cache);
  for (auto& c : cache.comments) {
    if (c.id == commentId) {
      auto& list = c.reactions[reactionIndex];
      auto it = std::find(list.begin(), list.end(), myUid);
      if (it != list.end()) {
        list.erase(it);
        adding = false;
      } else {
        list.push_back(myUid);
        adding = true;
      }
      break;
    }
  }
  ReadBookYStorage::saveBookCache(cache);

  JsonDocument pDoc;
  pDoc["commentId"] = commentId;
  pDoc["reactionKey"] = REACTION_KEYS[reactionIndex];

  ReadBookYOutboxOp op;
  op.operationId = "op_" + ReadBookYAnchors::generateUuidV4();
  op.journeyId = sidecar.journeyId;
  op.bookId = sidecar.bookId;
  op.editionId = sidecar.editionId;
  op.type = adding ? "add_reaction" : "remove_reaction";
  op.entityId = commentId;
  serializeJson(pDoc, op.payloadJson);
  op.createdAtMs = nowEpochMs();

  const bool ok = READBOOKY_OUTBOX.enqueue(op);
  if (ok) triggerBackgroundSync(sidecar.journeyId);
  return ok;
}

bool ReadBookYSync::syncJourneyNow(const std::string& journeyId, const std::string& bookId,
                                   const std::string& editionId, const bool mirrorClippings, std::string& errorOut) {
  errorOut.clear();
  if (journeyId.empty()) return false;

  std::lock_guard<std::mutex> lock(syncMutex_);
  std::string idToken;
  if (!READBOOKY_AUTH.ensureIdToken(idToken, errorOut)) {
    std::lock_guard<std::mutex> sl(statusMutex_);
    lastError_ = errorOut;
    return false;
  }

  ReadBookYBookCache cache;
  ReadBookYStorage::loadBookCache(journeyId, cache);
  cache.journeyId = journeyId;
  if (!bookId.empty()) cache.bookId = bookId;
  if (!editionId.empty()) cache.editionId = editionId;

  // Gather pending WAL operations for this journey
  std::vector<ReadBookYOutboxOp> allPending;
  READBOOKY_OUTBOX.loadPending(allPending);
  std::vector<ReadBookYOutboxOp> journeyOps;
  for (const auto& op : allPending) {
    if (op.journeyId == journeyId && journeyOps.size() < MAX_OUTBOX_BATCH_OPS) {
      journeyOps.push_back(op);
    }
  }

  bool sentOps = false;
  bool hasMore = true;
  int pageLoops = 0;

  while (hasMore && pageLoops < 10) {
    ++pageLoops;
    JsonDocument reqDoc;
    reqDoc["journeyId"] = journeyId;
    if (!cache.bookId.empty()) reqDoc["bookId"] = cache.bookId;
    if (!cache.editionId.empty()) reqDoc["editionId"] = cache.editionId;
    reqDoc["deviceId"] = READBOOKY_AUTH.deviceId();
    reqDoc["limit"] = 100;

    if (cache.cursor.initialized) {
      JsonObject cObj = reqDoc["cursor"].to<JsonObject>();
      cObj["changedAtMs"] = cache.cursor.changedAtMs;
      cObj["changeId"] = cache.cursor.changeId;
    } else {
      reqDoc["cursor"] = nullptr;
    }

    if (!sentOps && !journeyOps.empty()) {
      JsonArray opsArr = reqDoc["operations"].to<JsonArray>();
      for (const auto& op : journeyOps) {
        JsonObject o = opsArr.add<JsonObject>();
        o["operationId"] = op.operationId;
        o["type"] = op.type;
        o["entityId"] = op.entityId;
        JsonDocument pParsed;
        if (!deserializeJson(pParsed, op.payloadJson)) {
          o["payload"] = pParsed.as<JsonVariantConst>();
        }
      }
      sentOps = true;
    }

    std::string reqBody;
    serializeJson(reqDoc, reqBody);

    const std::string url = std::string(READBOOKY_API_BASE_URL) + "/sync";
    const HttpResponse res = ReadBookYClient::postJson(url, reqBody, idToken);

    if (!res.ok()) {
      ReadBookYAuth::secureScrub(idToken);
      errorOut = res.error.empty() ? "Sync request failed" : res.error;
      std::lock_guard<std::mutex> sl(statusMutex_);
      lastError_ = errorOut;
      return false;
    }

    JsonDocument respDoc;
    if (deserializeJson(respDoc, res.body)) {
      ReadBookYAuth::secureScrub(idToken);
      errorOut = "Invalid sync JSON";
      std::lock_guard<std::mutex> sl(statusMutex_);
      lastError_ = errorOut;
      return false;
    }

    // 1. Acknowledge & atomically compact WAL operations
    if (!journeyOps.empty()) {
      std::unordered_set<std::string> ackedIds;
      std::vector<ReadBookYOutboxOp> rejectedList;
      for (JsonObjectConst opRes : respDoc["operationResults"].as<JsonArrayConst>()) {
        const std::string opId = opRes["operationId"] | "";
        const std::string st = opRes["status"] | "";
        if (opId.empty()) continue;
        if (st == "ok") {
          ackedIds.insert(opId);
          // Reconcile server-assigned canonical anchorId / cfiRange / ordinal on local highlight cache immediately
          const std::string entId = opRes["entityId"] | "";
          const std::string srvAnchorId = opRes["anchorId"] | "";
          const std::string srvCfi = opRes["cfiRange"] | "";
          const uint32_t srvOrd = opRes["ordinal"] | 0U;
          if (!entId.empty() && !srvAnchorId.empty()) {
            for (auto& h : cache.highlights) {
              if (h.id == entId) {
                const std::string oldAnchorId = h.anchorId;
                h.anchorId = srvAnchorId;
                if (!srvCfi.empty()) h.cfiRange = srvCfi;
                if (srvOrd > 0) h.ordinal = srvOrd;
                if (oldAnchorId != srvAnchorId) {
                  for (auto& a : cache.anchors) {
                    if (a.id == oldAnchorId) a.id = srvAnchorId;
                  }
                  for (auto& c : cache.comments) {
                    if (c.anchorId == oldAnchorId) c.anchorId = srvAnchorId;
                  }
                }
                break;
              }
            }
          }
        } else if (st == "rejected") {
          for (auto& srcOp : journeyOps) {
            if (srcOp.operationId == opId) {
              srcOp.lastError = opRes["message"] | (opRes["code"] | "rejected");
              rejectedList.push_back(srcOp);
              break;
            }
          }
        }
      }
      if (!ackedIds.empty() || !rejectedList.empty()) {
        READBOOKY_OUTBOX.acknowledgeAndCompact(ackedIds, rejectedList);
      }
      journeyOps.clear();
    }

    // 2. Apply snapshot (`resetRequired` or initial sync) OR incremental delta changes
    const bool snapshotIncluded = respDoc["snapshotIncluded"] | false;
    if (snapshotIncluded && !respDoc["snapshot"].isNull()) {
      applySnapshotToCache(respDoc["snapshot"].as<JsonObjectConst>(), cache);
    } else {
      applyDeltaChangesToCache(respDoc["changes"].as<JsonArrayConst>(), cache);
      parsePositionFromJson(respDoc["position"].as<JsonObjectConst>(), cache.position);
    }

    // 3. Advance deterministic composite cursor `(changedAtMs, changeId)`
    if (!respDoc["cursor"].isNull()) {
      cache.cursor.changedAtMs = respDoc["cursor"]["changedAtMs"] | cache.cursor.changedAtMs;
      cache.cursor.changeId = respDoc["cursor"]["changeId"] | "";
      cache.cursor.initialized = true;
    }

    hasMore = respDoc["hasMore"] | false;
  }

  ReadBookYAuth::secureScrub(idToken);
  ReadBookYStorage::saveBookCache(cache);
  if (mirrorClippings) {
    ReadBookYAnchors::mirrorToCrossPointClippings(cache);
  }

  // Also flush & poll Page questions for this journey while online
  std::string pageErr;
  READBOOKY_PAGE.flushAndPollJourney(journeyId, pageErr);

  lastSyncMs_.store(nowEpochMs(), std::memory_order_release);
  {
    std::lock_guard<std::mutex> sl(statusMutex_);
    lastError_.clear();
  }
  return true;
}

bool ReadBookYSync::syncAllNow(std::string& errorOut) {
  errorOut.clear();
  if (!READBOOKY_AUTH.isSignedIn()) {
    errorOut = "Not signed in";
    return false;
  }

  // Collect all unique journeyIds that have pending outbox ops or downloaded books
  std::unordered_map<std::string, std::pair<std::string, std::string>> journeys;
  std::vector<ReadBookYOutboxOp> pending;
  READBOOKY_OUTBOX.loadPending(pending);
  for (const auto& op : pending) {
    if (!op.journeyId.empty()) {
      journeys[op.journeyId] = {op.bookId, op.editionId};
    }
  }

  std::vector<ReadBookYBookMeta> lib;
  if (ReadBookYStorage::loadLibraryCache(lib)) {
    for (const auto& b : lib) {
      if (b.downloaded && !b.journeyId.empty()) {
        journeys[b.journeyId] = {b.bookId, b.editionId};
      }
    }
  }

  bool allOk = true;
  for (const auto& kv : journeys) {
    std::string err;
    if (!syncJourneyNow(kv.first, kv.second.first, kv.second.second, /*mirrorClippings=*/false, err)) {
      allOk = false;
      if (errorOut.empty()) errorOut = err;
    }
  }

  // Refresh library metadata when online
  std::vector<ReadBookYBookMeta> refreshed;
  std::string libErr;
  fetchLibrary(refreshed, libErr);
  return allOk;
}

void ReadBookYSync::backgroundSyncTask(void* param) {
  std::unique_ptr<BgSyncArg> arg(static_cast<BgSyncArg*>(param));
  auto& self = ReadBookYSync::getInstance();

  if (WiFi.status() == WL_CONNECTED && READBOOKY_AUTH.isSignedIn() &&
      HalMemory::getInternalHeap().freeBytes >= MIN_SYNC_FREE_HEAP) {
    std::string err;
    if (arg && !arg->priorityJourneyId.empty()) {
      ReadBookYBookCache c;
      ReadBookYStorage::loadBookCache(arg->priorityJourneyId, c);
      self.syncJourneyNow(arg->priorityJourneyId, c.bookId, c.editionId, /*mirrorClippings=*/false, err);
    } else {
      self.syncAllNow(err);
    }
  }

  self.syncRunning_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void ReadBookYSync::triggerBackgroundSync(const std::string& priorityJourneyId) {
  if (!READBOOKY_AUTH.isSignedIn() || WiFi.status() != WL_CONNECTED) return;
  if (HalMemory::getInternalHeap().freeBytes < MIN_SYNC_FREE_HEAP) {
    LOG_DBG("RBYSYNC", "Skipping background sync: low heap (%zu)", HalMemory::getInternalHeap().freeBytes);
    return;
  }
  bool expected = false;
  if (!syncRunning_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;  // Already running
  }

  auto* arg = new (std::nothrow) BgSyncArg{priorityJourneyId};
  if (!arg) {
    syncRunning_.store(false, std::memory_order_release);
    return;
  }

  BaseType_t rc = xTaskCreatePinnedToCore(backgroundSyncTask, "RbySync", 12288, arg, 1, nullptr, 0);
  if (rc != pdPASS) {
    delete arg;
    syncRunning_.store(false, std::memory_order_release);
  }
}

void ReadBookYSync::prepareForSleep() {
  if (!SETTINGS.readbookySyncOnSleep) return;
  if (!READBOOKY_AUTH.isSignedIn() || WiFi.status() != WL_CONNECTED) return;
  if (READBOOKY_OUTBOX.pendingCount() == 0 && READBOOKY_PAGE.totalQueuedOfflineCount() == 0) return;
  if (HalMemory::getInternalHeap().freeBytes < MIN_SYNC_FREE_HEAP) return;

  LOG_INF("RBYSYNC", "Running bounded pre-sleep ReadBookY flush");
  std::string err;
  syncAllNow(err);
}

std::string ReadBookYSync::lastError() const {
  std::lock_guard<std::mutex> lock(statusMutex_);
  return lastError_;
}

std::string ReadBookYSync::statusSummary() const {
  if (!READBOOKY_AUTH.isSignedIn()) return "Not signed in";
  const size_t pending = READBOOKY_OUTBOX.pendingCount();
  const size_t rejected = READBOOKY_OUTBOX.rejectedCount();
  if (isSyncing()) return "Syncing...";
  if (WiFi.status() != WL_CONNECTED) {
    if (pending > 0) return "Offline (" + std::to_string(pending) + " queued)";
    return "Offline";
  }
  if (pending > 0) return std::to_string(pending) + " queued to sync";
  if (rejected > 0) return "Synced (" + std::to_string(rejected) + " failed)";
  const std::string err = lastError();
  if (!err.empty()) return "Sync error: " + err;
  return "Synced";
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
