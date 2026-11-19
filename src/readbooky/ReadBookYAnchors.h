#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ClippingStore.h"
#include "ReadBookYModels.h"

namespace readbooky {

// Utilities for ReadBookY <-> CrossPoint anchor/clipping interoperability and stable UUIDv4 generation.
class ReadBookYAnchors {
 public:
  // Generates a cryptographically random RFC 4122 UUIDv4 using ESP32 hardware RNG (`esp_fill_random`).
  static std::string generateUuidV4();

  // Mirrors visible ReadBookY highlights for a downloaded ReadBookY EPUB into CrossPoint's
  // existing `CLIPPINGS` store (using `CLIPPINGS.applyRemote`) so CrossPoint's native reader
  // renders highlights with zero modifications to `lib/Epub/` or `ClippingStore` v4 format.
  static void mirrorToCrossPointClippings(const ReadBookYBookCache& cache);

  // Maps a tapped or long-pressed CrossPoint `Clipping` back to its corresponding
  // `ReadBookYHighlight` (by exact ID match first, then overlapping spineIndex + source offsets).
  // Returns nullptr if not found.
  static const ReadBookYHighlight* findHighlightForClipping(const ReadBookYBookCache& cache,
                                                            const Clipping& clipping);

  // Finds a highlight overlapping the given spineIndex and visible-codepoint range.
  static const ReadBookYHighlight* findHighlightByRange(const ReadBookYBookCache& cache, int spineIndex,
                                                        uint32_t startOffset, uint32_t endOffset);

  // Normalizes whitespace & curly quotes (mirrors ReadBookY `normalizeText` for local matching).
  static std::string normalizeText(const std::string& raw);
};

}  // namespace readbooky
