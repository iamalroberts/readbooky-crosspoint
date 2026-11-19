#include "ReadBookYAnchors.h"

#ifdef ENABLE_READBOOKY

#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace readbooky {

std::string ReadBookYAnchors::generateUuidV4() {
  uint8_t b[16] = {};
  esp_fill_random(b, sizeof(b));
  b[6] = static_cast<uint8_t>((b[6] & 0x0FU) | 0x40U);  // Version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3FU) | 0x80U);  // Variant RFC 4122

  char out[40];
  snprintf(out, sizeof(out),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(out);
}

void ReadBookYAnchors::mirrorToCrossPointClippings(const ReadBookYBookCache& cache) {
  // Collect currently active ReadBookY highlight IDs that have valid CrossPoint source offsets.
  std::unordered_set<std::string> activeIds;
  activeIds.reserve(cache.highlights.size());

  for (const auto& h : cache.highlights) {
    if (h.id.empty()) continue;
    activeIds.insert(h.id);
    if (h.sourceStartOffset == UINT32_MAX || h.sourceEndOffset == UINT32_MAX ||
        h.sourceEndOffset <= h.sourceStartOffset) {
      continue;
    }

    Clipping c{};
    c.spineIndex = static_cast<uint16_t>(std::max(0, h.spineIndex));
    c.startOffset = h.sourceStartOffset;
    c.endOffset = h.sourceEndOffset;
    c.timestamp = h.updatedAtMs > 0 ? static_cast<uint32_t>(h.updatedAtMs / 1000LL) : 0U;
    c.syncRevision = h.updatedAtMs > 0 ? static_cast<uint64_t>(h.updatedAtMs) : 1ULL;
    c.pendingUpload = false;
    snprintf(c.id, sizeof(c.id), "%s", h.id.c_str());
    snprintf(c.chapterTitle, sizeof(c.chapterTitle), "%s", h.chapterHref.c_str());

    CLIPPINGS.applyRemote(c, h.selectedText, /*deleted=*/false);
  }

  // Remove stale ReadBookY-synced clippings that were deleted or restricted remotely.
  // Iterate backwards because removeClippingAt shifts indices.
  for (size_t i = CLIPPINGS.clippingCount(); i > 0; --i) {
    const Clipping* existing = CLIPPINGS.clippingAt(i - 1);
    if (!existing) continue;
    // Only prune clippings that came from ReadBookY (pendingUpload == false and non-empty id)
    if (!existing->pendingUpload && existing->id[0] != '\0' && activeIds.find(existing->id) == activeIds.end()) {
      Clipping tomb = *existing;
      CLIPPINGS.applyRemote(tomb, "", /*deleted=*/true);
    }
  }
}

const ReadBookYHighlight* ReadBookYAnchors::findHighlightForClipping(const ReadBookYBookCache& cache,
                                                                     const Clipping& clipping) {
  if (clipping.id[0] != '\0') {
    for (const auto& h : cache.highlights) {
      if (h.id == clipping.id) return &h;
    }
  }
  if (clipping.startOffset != UINT32_MAX && clipping.endOffset != UINT32_MAX) {
    return findHighlightByRange(cache, static_cast<int>(clipping.spineIndex), clipping.startOffset, clipping.endOffset);
  }
  return nullptr;
}

const ReadBookYHighlight* ReadBookYAnchors::findHighlightByRange(const ReadBookYBookCache& cache, const int spineIndex,
                                                                 const uint32_t startOffset,
                                                                 const uint32_t endOffset) {
  for (const auto& h : cache.highlights) {
    if (h.spineIndex != spineIndex) continue;
    if (h.sourceStartOffset == UINT32_MAX || h.sourceEndOffset == UINT32_MAX) continue;
    if (endOffset > h.sourceStartOffset && startOffset < h.sourceEndOffset) {
      return &h;
    }
  }
  return nullptr;
}

std::string ReadBookYAnchors::normalizeText(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool prevSpace = true;
  for (size_t i = 0; i < raw.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    // UTF-8 curly quotes U+201C/201D (E2 80 9C/9D) -> '"', U+2018/2019 (E2 80 98/99) -> '\''
    if (c == 0xE2 && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x80) {
      const unsigned char b2 = static_cast<unsigned char>(raw[i + 2]);
      if (b2 == 0x9C || b2 == 0x9D) {
        out.push_back('"');
        prevSpace = false;
        i += 2;
        continue;
      }
      if (b2 == 0x98 || b2 == 0x99) {
        out.push_back('\'');
        prevSpace = false;
        i += 2;
        continue;
      }
    }
    out.push_back(c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
    prevSpace = false;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
