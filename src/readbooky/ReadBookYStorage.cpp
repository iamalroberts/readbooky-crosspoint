#include "ReadBookYStorage.h"

#ifdef ENABLE_READBOOKY

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace readbooky {

namespace {

void writeAnchorRef(JsonObject obj, const ReadBookYAnchorRef& ref) {
  obj["spineIndex"] = ref.spineIndex;
  obj["chapterHref"] = ref.chapterHref;
  obj["sourceOffset"] = ref.sourceOffset;
  obj["percentage"] = ref.percentage;
  obj["chapterTitle"] = ref.chapterTitle;
  obj["valid"] = ref.valid;
}

void readAnchorRef(JsonObjectConst obj, ReadBookYAnchorRef& ref) {
  if (obj.isNull()) {
    ref = {};
    return;
  }
  ref.spineIndex = obj["spineIndex"] | 0;
  ref.chapterHref = obj["chapterHref"] | "";
  ref.sourceOffset = obj["sourceOffset"] | 0U;
  ref.percentage = obj["percentage"] | 0.0f;
  ref.chapterTitle = obj["chapterTitle"] | "";
  ref.valid = obj["valid"] | false;
}

void writePosition(JsonObject obj, const ReadBookYPosition& pos) {
  obj["spineIndex"] = pos.spineIndex;
  obj["chapterHref"] = pos.chapterHref;
  obj["sourceOffset"] = pos.sourceOffset;
  obj["progressPercent"] = pos.progressPercent;
  obj["currentCfi"] = pos.currentCfi;
  obj["trustedOrdinal"] = pos.trustedOrdinal;
  obj["trustedPercent"] = pos.trustedPercent;
  obj["trustedChapterTitle"] = pos.trustedChapterTitle;
  obj["trustedPositionSource"] = pos.trustedPositionSource;
  obj["updatedAtMs"] = pos.updatedAtMs;
  if (pos.localTrustedAnchor.valid) {
    writeAnchorRef(obj["localTrustedAnchor"].to<JsonObject>(), pos.localTrustedAnchor);
  }
}

void readPosition(JsonObjectConst obj, ReadBookYPosition& pos) {
  if (obj.isNull()) {
    pos = {};
    return;
  }
  pos.spineIndex = obj["spineIndex"] | 0;
  pos.chapterHref = obj["chapterHref"] | "";
  pos.sourceOffset = obj["sourceOffset"] | 0U;
  pos.progressPercent = obj["progressPercent"] | 0.0f;
  pos.currentCfi = obj["currentCfi"] | "";
  pos.trustedOrdinal = obj["trustedOrdinal"] | 0U;
  pos.trustedPercent = obj["trustedPercent"] | 0.0f;
  pos.trustedChapterTitle = obj["trustedChapterTitle"] | "";
  pos.trustedPositionSource = obj["trustedPositionSource"] | "sequential";
  pos.updatedAtMs = obj["updatedAtMs"] | 0LL;
  readAnchorRef(obj["localTrustedAnchor"].as<JsonObjectConst>(), pos.localTrustedAnchor);
}

}  // namespace

bool ReadBookYStorage::ensureDirectories() {
  bool ok = true;
  ok &= Storage.ensureDirectoryExists(ROOT_DIR);
  ok &= Storage.ensureDirectoryExists(OUTBOX_DIR);
  ok &= Storage.ensureDirectoryExists(BOOKS_CACHE_DIR);
  ok &= Storage.ensureDirectoryExists(PAGE_CACHE_DIR);
  ok &= Storage.ensureDirectoryExists("/Books");
  ok &= Storage.ensureDirectoryExists(EPUB_DOWNLOAD_DIR);
  return ok;
}

bool ReadBookYStorage::writeAtomicString(const std::string& path, const std::string& content) {
  const std::string tmpPath = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("RBY", tmpPath, file)) {
    LOG_ERR("RBY", "Failed opening temp file for write: %s", tmpPath.c_str());
    return false;
  }

  size_t written = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(content.data());
  const size_t total = content.size();
  while (written < total) {
    const size_t chunk = std::min<size_t>(1024, total - written);
    const size_t n = file.write(bytes + written, chunk);
    if (n == 0) {
      file.close();
      Storage.remove(tmpPath.c_str());
      LOG_ERR("RBY", "Short write on %s", tmpPath.c_str());
      return false;
    }
    written += n;
  }

  file.flush();
  file.close();

  if (!Storage.replaceFile(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("RBY", "Atomic replace failed: %s -> %s", tmpPath.c_str(), path.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool ReadBookYStorage::readStringFile(const std::string& path, const size_t maxBytes, std::string& out) {
  out.clear();
  if (!Storage.exists(path.c_str())) return false;
  return Storage.readFileToString("RBY", path, maxBytes, out);
}

std::string ReadBookYStorage::sanitizeFileSegment(const std::string& raw, const size_t maxLen) {
  std::string out;
  out.reserve(std::min(raw.size(), maxLen));
  for (char c : raw) {
    if (out.size() >= maxLen) break;
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '-' || c == '_' || c == ' ') {
      out.push_back(c);
    } else if (c == '.' && !out.empty() && out.back() != '.') {
      out.push_back('_');
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.empty()) out = "book";
  return out;
}

std::string ReadBookYStorage::sidecarPathForEpub(const std::string& epubPath) {
  return epubPath + SIDECAR_SUFFIX;
}

bool ReadBookYStorage::isReadBookYBookPath(const std::string& epubPath) {
  if (epubPath.empty()) return false;
  return Storage.exists(sidecarPathForEpub(epubPath).c_str());
}

bool ReadBookYStorage::loadSidecar(const std::string& epubPath, ReadBookYSidecar& out) {
  out = {};
  std::string raw;
  if (!readStringFile(sidecarPathForEpub(epubPath), 8192, raw)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  out.journeyId = doc["journeyId"] | "";
  out.bookId = doc["bookId"] | "";
  out.editionId = doc["editionId"] | "";
  out.workId = doc["workId"] | "";
  out.title = doc["title"] | "";
  out.author = doc["author"] | "";
  out.epubSha256 = doc["epubSha256"] | "";
  out.fileSize = doc["fileSize"] | 0ULL;
  out.downloadedAtMs = doc["downloadedAtMs"] | 0LL;
  return !out.journeyId.empty() && !out.bookId.empty();
}

bool ReadBookYStorage::saveSidecar(const std::string& epubPath, const ReadBookYSidecar& sidecar) {
  JsonDocument doc;
  doc["journeyId"] = sidecar.journeyId;
  doc["bookId"] = sidecar.bookId;
  doc["editionId"] = sidecar.editionId;
  doc["workId"] = sidecar.workId;
  doc["title"] = sidecar.title;
  doc["author"] = sidecar.author;
  doc["epubSha256"] = sidecar.epubSha256;
  doc["fileSize"] = sidecar.fileSize;
  doc["downloadedAtMs"] = sidecar.downloadedAtMs;

  std::string serialized;
  serializeJson(doc, serialized);
  return writeAtomicString(sidecarPathForEpub(epubPath), serialized);
}

std::string ReadBookYStorage::buildSafeEpubPath(const std::string& title, const std::string& editionId) {
  const std::string safeTitle = sanitizeFileSegment(title, 48);
  std::string suffix = sanitizeFileSegment(editionId, 12);
  if (suffix.empty()) suffix = "rby";
  return std::string(EPUB_DOWNLOAD_DIR) + "/" + safeTitle + "_" + suffix + ".epub";
}

bool ReadBookYStorage::loadLibraryCache(std::vector<ReadBookYBookMeta>& outBooks) {
  outBooks.clear();
  std::string raw;
  if (!readStringFile(LIBRARY_CACHE_PATH, 128U * 1024U, raw)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  if (arr.isNull()) return false;

  outBooks.reserve(arr.size());
  for (JsonObjectConst o : arr) {
    ReadBookYBookMeta b;
    b.journeyId = o["journeyId"] | "";
    b.journeyName = o["journeyName"] | "";
    b.bookId = o["bookId"] | "";
    b.editionId = o["editionId"] | "";
    b.workId = o["workId"] | "";
    b.title = o["title"] | "";
    b.author = o["author"] | "";
    b.language = o["language"] | "en";
    b.coverUrl = o["coverUrl"] | "";
    b.localPath = o["localPath"] | "";
    b.fileSize = o["fileSize"] | 0ULL;
    b.epubSha256 = o["epubSha256"] | "";
    b.pageReady = o["pageReady"] | false;
    b.participantCount = o["participantCount"] | 1;
    b.updatedAtMs = o["updatedAtMs"] | 0LL;
    readPosition(o["progress"].as<JsonObjectConst>(), b.progress);
    if (b.localPath.empty() && !b.title.empty()) {
      const std::string candidate = buildSafeEpubPath(b.title, b.editionId.empty() ? b.bookId : b.editionId);
      if (Storage.exists(candidate.c_str())) {
        b.localPath = candidate;
      }
    }
    b.downloaded = !b.localPath.empty() && Storage.exists(b.localPath.c_str());
    if (!b.journeyId.empty()) outBooks.push_back(std::move(b));
  }
  return true;
}

bool ReadBookYStorage::saveLibraryCache(const std::vector<ReadBookYBookMeta>& books) {
  ensureDirectories();
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& b : books) {
    JsonObject o = arr.add<JsonObject>();
    o["journeyId"] = b.journeyId;
    o["journeyName"] = b.journeyName;
    o["bookId"] = b.bookId;
    o["editionId"] = b.editionId;
    o["workId"] = b.workId;
    o["title"] = b.title;
    o["author"] = b.author;
    o["language"] = b.language;
    o["coverUrl"] = b.coverUrl;
    o["localPath"] = b.localPath;
    o["fileSize"] = b.fileSize;
    o["epubSha256"] = b.epubSha256;
    o["pageReady"] = b.pageReady;
    o["participantCount"] = b.participantCount;
    o["updatedAtMs"] = b.updatedAtMs;
    writePosition(o["progress"].to<JsonObject>(), b.progress);
  }
  std::string serialized;
  serializeJson(doc, serialized);
  return writeAtomicString(LIBRARY_CACHE_PATH, serialized);
}

std::string ReadBookYStorage::bookCachePath(const std::string& journeyId) {
  return std::string(BOOKS_CACHE_DIR) + "/" + sanitizeFileSegment(journeyId, 64) + ".json";
}

bool ReadBookYStorage::loadBookCache(const std::string& journeyId, ReadBookYBookCache& out) {
  out = {};
  out.journeyId = journeyId;
  std::string raw;
  if (!readStringFile(bookCachePath(journeyId), 384U * 1024U, raw)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  out.journeyId = doc["journeyId"] | journeyId;
  out.bookId = doc["bookId"] | "";
  out.editionId = doc["editionId"] | "";
  out.cursor.changedAtMs = doc["cursor"]["changedAtMs"] | 0LL;
  out.cursor.changeId = doc["cursor"]["changeId"] | "";
  out.cursor.initialized = doc["cursor"]["initialized"] | false;
  readPosition(doc["position"].as<JsonObjectConst>(), out.position);

  for (JsonObjectConst o : doc["anchors"].as<JsonArrayConst>()) {
    ReadBookYAnchor a;
    a.id = o["id"] | "";
    a.journeyId = o["journeyId"] | journeyId;
    a.bookId = o["bookId"] | out.bookId;
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
    if (!a.id.empty()) out.anchors.push_back(std::move(a));
  }

  for (JsonObjectConst o : doc["highlights"].as<JsonArrayConst>()) {
    ReadBookYHighlight h;
    h.id = o["id"] | "";
    h.anchorId = o["anchorId"] | "";
    h.journeyId = o["journeyId"] | journeyId;
    h.bookId = o["bookId"] | out.bookId;
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
    if (!h.id.empty()) out.highlights.push_back(std::move(h));
  }

  for (JsonObjectConst o : doc["comments"].as<JsonArrayConst>()) {
    ReadBookYComment c;
    c.id = o["id"] | "";
    c.anchorId = o["anchorId"] | "";
    c.highlightId = o["highlightId"] | "";
    c.journeyId = o["journeyId"] | journeyId;
    c.bookId = o["bookId"] | out.bookId;
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
    if (!c.id.empty()) out.comments.push_back(std::move(c));
  }

  return true;
}

bool ReadBookYStorage::saveBookCache(const ReadBookYBookCache& cache) {
  if (cache.journeyId.empty()) return false;
  ensureDirectories();

  JsonDocument doc;
  doc["journeyId"] = cache.journeyId;
  doc["bookId"] = cache.bookId;
  doc["editionId"] = cache.editionId;
  doc["cursor"]["changedAtMs"] = cache.cursor.changedAtMs;
  doc["cursor"]["changeId"] = cache.cursor.changeId;
  doc["cursor"]["initialized"] = cache.cursor.initialized;
  writePosition(doc["position"].to<JsonObject>(), cache.position);

  JsonArray ancArr = doc["anchors"].to<JsonArray>();
  for (const auto& a : cache.anchors) {
    JsonObject o = ancArr.add<JsonObject>();
    o["id"] = a.id;
    o["journeyId"] = a.journeyId;
    o["bookId"] = a.bookId;
    o["scope"] = a.scope;
    o["ownerUid"] = a.ownerUid;
    o["isPrivate"] = a.isPrivate;
    if (!a.visibleTo.empty()) {
      JsonArray vt = o["visibleTo"].to<JsonArray>();
      for (const auto& u : a.visibleTo) vt.add(u);
    }
    o["spineIndex"] = a.spineIndex;
    o["chapterHref"] = a.chapterHref;
    o["cfiRange"] = a.cfiRange;
    if (a.sourceStartOffset != UINT32_MAX) o["sourceStartOffset"] = a.sourceStartOffset;
    if (a.sourceEndOffset != UINT32_MAX) o["sourceEndOffset"] = a.sourceEndOffset;
    o["ordinal"] = a.ordinal;
    o["anchorText"] = a.anchorText;
    o["progressPercent"] = a.progressPercent;
    o["updatedAtMs"] = a.updatedAtMs;
    o["lastCommentAtMs"] = a.lastCommentAtMs;
  }

  JsonArray hlArr = doc["highlights"].to<JsonArray>();
  for (const auto& h : cache.highlights) {
    JsonObject o = hlArr.add<JsonObject>();
    o["id"] = h.id;
    o["anchorId"] = h.anchorId;
    o["journeyId"] = h.journeyId;
    o["bookId"] = h.bookId;
    o["uid"] = h.uid;
    o["spineIndex"] = h.spineIndex;
    o["chapterHref"] = h.chapterHref;
    o["cfiRange"] = h.cfiRange;
    if (h.sourceStartOffset != UINT32_MAX) o["sourceStartOffset"] = h.sourceStartOffset;
    if (h.sourceEndOffset != UINT32_MAX) o["sourceEndOffset"] = h.sourceEndOffset;
    o["ordinal"] = h.ordinal;
    o["selectedText"] = h.selectedText;
    o["contextBefore"] = h.contextBefore;
    o["contextAfter"] = h.contextAfter;
    o["color"] = h.color;
    o["scope"] = h.scope;
    o["isPrivate"] = h.isPrivate;
    if (!h.visibleTo.empty()) {
      JsonArray vt = o["visibleTo"].to<JsonArray>();
      for (const auto& u : h.visibleTo) vt.add(u);
    }
    o["anchorResolution"] = h.anchorResolution;
    o["createdAtMs"] = h.createdAtMs;
    o["updatedAtMs"] = h.updatedAtMs;
  }

  JsonArray cmArr = doc["comments"].to<JsonArray>();
  for (const auto& c : cache.comments) {
    JsonObject o = cmArr.add<JsonObject>();
    o["id"] = c.id;
    o["anchorId"] = c.anchorId;
    o["highlightId"] = c.highlightId;
    o["journeyId"] = c.journeyId;
    o["bookId"] = c.bookId;
    o["uid"] = c.uid;
    o["authorName"] = c.authorName;
    o["content"] = c.content;
    if (!c.parentCommentId.empty()) o["parentCommentId"] = c.parentCommentId;
    if (!c.rootCommentId.empty()) o["rootCommentId"] = c.rootCommentId;
    o["scope"] = c.scope;
    o["isPrivate"] = c.isPrivate;
    if (!c.visibleTo.empty()) {
      JsonArray vt = o["visibleTo"].to<JsonArray>();
      for (const auto& u : c.visibleTo) vt.add(u);
    }
    JsonObject rx = o["reactions"].to<JsonObject>();
    for (size_t k = 0; k < REACTION_COUNT; ++k) {
      if (!c.reactions[k].empty()) {
        JsonArray rArr = rx[REACTION_KEYS[k]].to<JsonArray>();
        for (const auto& u : c.reactions[k]) rArr.add(u);
      }
    }
    o["createdAtMs"] = c.createdAtMs;
    o["updatedAtMs"] = c.updatedAtMs;
  }

  std::string serialized;
  serializeJson(doc, serialized);
  return writeAtomicString(bookCachePath(cache.journeyId), serialized);
}

std::string ReadBookYStorage::pageCachePath(const std::string& journeyId) {
  return std::string(PAGE_CACHE_DIR) + "/" + sanitizeFileSegment(journeyId, 64) + ".json";
}

bool ReadBookYStorage::loadPageItems(const std::string& journeyId, std::vector<ReadBookYPageItem>& outItems) {
  outItems.clear();
  std::string raw;
  if (!readStringFile(pageCachePath(journeyId), 256U * 1024U, raw)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  JsonArrayConst arr = doc["items"].as<JsonArrayConst>();
  if (arr.isNull()) return false;

  for (JsonObjectConst o : arr) {
    ReadBookYPageItem item;
    item.requestId = o["requestId"] | "";
    item.threadId = o["threadId"] | "";
    item.journeyId = o["journeyId"] | journeyId;
    item.bookId = o["bookId"] | "";
    item.status = o["status"] | "queued_offline";
    item.question = o["question"] | "";
    item.selectedText = o["selectedText"] | "";
    readAnchorRef(o["selectedAnchor"].as<JsonObjectConst>(), item.selectedAnchor);
    readAnchorRef(o["trustedAnchorAtQuestion"].as<JsonObjectConst>(), item.trustedAnchorAtQuestion);
    item.trustedOrdinalAtQuestion = o["trustedOrdinalAtQuestion"] | 0U;
    item.trustedOrdinalUsed = o["trustedOrdinalUsed"] | 0U;
    item.boundaryLabel = o["boundaryLabel"] | "";
    item.spoilerOverride = o["spoilerOverride"] | false;
    item.answer = o["answer"] | "";
    item.cautious = o["cautious"] | false;
    item.answerConfidence = o["answerConfidence"] | "";
    item.unread = o["unread"] | false;
    item.retryable = o["retryable"] | false;
    item.errorMessage = o["errorMessage"] | "";
    item.queuedAtMs = o["queuedAtMs"] | 0LL;
    item.completedAtMs = o["completedAtMs"] | 0LL;
    for (JsonObjectConst cObj : o["citations"].as<JsonArrayConst>()) {
      ReadBookYPageCitation cit;
      cit.anchorId = cObj["anchorId"] | "";
      cit.chapterTitle = cObj["chapterTitle"] | "";
      cit.snippet = cObj["snippet"] | "";
      cit.ordinal = cObj["ordinal"] | 0U;
      cit.isPriorBook = cObj["isPriorBook"] | false;
      item.citations.push_back(std::move(cit));
    }
    if (!item.requestId.empty() || !item.question.empty()) {
      outItems.push_back(std::move(item));
    }
  }
  return true;
}

bool ReadBookYStorage::savePageItems(const std::string& journeyId, const std::vector<ReadBookYPageItem>& items) {
  if (journeyId.empty()) return false;
  ensureDirectories();

  JsonDocument doc;
  JsonArray arr = doc["items"].to<JsonArray>();
  const size_t limit = std::min(items.size(), MAX_PAGE_HISTORY_ITEMS);
  for (size_t i = 0; i < limit; ++i) {
    const auto& item = items[i];
    JsonObject o = arr.add<JsonObject>();
    o["requestId"] = item.requestId;
    o["threadId"] = item.threadId;
    o["journeyId"] = item.journeyId;
    o["bookId"] = item.bookId;
    o["status"] = item.status;
    o["question"] = item.question;
    o["selectedText"] = item.selectedText;
    if (item.selectedAnchor.valid) {
      writeAnchorRef(o["selectedAnchor"].to<JsonObject>(), item.selectedAnchor);
    }
    if (item.trustedAnchorAtQuestion.valid) {
      writeAnchorRef(o["trustedAnchorAtQuestion"].to<JsonObject>(), item.trustedAnchorAtQuestion);
    }
    o["trustedOrdinalAtQuestion"] = item.trustedOrdinalAtQuestion;
    o["trustedOrdinalUsed"] = item.trustedOrdinalUsed;
    o["boundaryLabel"] = item.boundaryLabel;
    o["spoilerOverride"] = item.spoilerOverride;
    o["answer"] = item.answer;
    o["cautious"] = item.cautious;
    o["answerConfidence"] = item.answerConfidence;
    o["unread"] = item.unread;
    o["retryable"] = item.retryable;
    o["errorMessage"] = item.errorMessage;
    o["queuedAtMs"] = item.queuedAtMs;
    o["completedAtMs"] = item.completedAtMs;
    if (!item.citations.empty()) {
      JsonArray cArr = o["citations"].to<JsonArray>();
      for (const auto& cit : item.citations) {
        JsonObject cObj = cArr.add<JsonObject>();
        cObj["anchorId"] = cit.anchorId;
        cObj["chapterTitle"] = cit.chapterTitle;
        cObj["snippet"] = cit.snippet;
        cObj["ordinal"] = cit.ordinal;
        cObj["isPriorBook"] = cit.isPriorBook;
      }
    }
  }

  std::string serialized;
  serializeJson(doc, serialized);
  return writeAtomicString(pageCachePath(journeyId), serialized);
}

void ReadBookYStorage::clearUserCaches() {
  Storage.remove(LIBRARY_CACHE_PATH);
  Storage.removeDir(OUTBOX_DIR);
  Storage.removeDir(BOOKS_CACHE_DIR);
  Storage.removeDir(PAGE_CACHE_DIR);
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
