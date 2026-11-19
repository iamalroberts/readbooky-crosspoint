#pragma once

#include <cstddef>
#include <cstdint>

// Compile-time ReadBookY configuration for XTEINK X4 Pro.
// All endpoints use verified HTTPS (platform CA bundle via esp_crt_bundle_attach).
// Zero server secrets, admin credentials, or Gemini API keys exist in firmware.

#ifndef READBOOKY_API_BASE_URL
#define READBOOKY_API_BASE_URL "https://readbooky.social/device/v1"
#endif

// Public Firebase Web API key (identical to readbooky/public/firebase-config.js).
// This identifies the Firebase project for email/password authentication; it is NOT a secret.
#ifndef READBOOKY_FIREBASE_WEB_API_KEY
#define READBOOKY_FIREBASE_WEB_API_KEY "AIzaSyBR4CRlnd4e0ypTgsh0nYV53B-CJrqgABk"
#endif

#define READBOOKY_AUTH_SIGNIN_URL \
  "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" READBOOKY_FIREBASE_WEB_API_KEY
#define READBOOKY_AUTH_REFRESH_URL \
  "https://securetoken.googleapis.com/v1/token?key=" READBOOKY_FIREBASE_WEB_API_KEY

namespace readbooky {

inline constexpr const char* DEVICE_MODEL = "xteink-x4-pro";
inline constexpr const char* NVS_AUTH_NAMESPACE = "rby_auth";

// SD card storage layout (all accessed strictly through HalStorage / Storage)
inline constexpr const char* ROOT_DIR = "/.readbooky";
inline constexpr const char* OUTBOX_DIR = "/.readbooky/outbox";
inline constexpr const char* OUTBOX_WAL_PATH = "/.readbooky/outbox/queue.jsonl";
inline constexpr const char* OUTBOX_REJECTED_PATH = "/.readbooky/outbox/rejected.jsonl";
inline constexpr const char* LIBRARY_CACHE_PATH = "/.readbooky/library.json";
inline constexpr const char* BOOKS_CACHE_DIR = "/.readbooky/books";
inline constexpr const char* PAGE_CACHE_DIR = "/.readbooky/page";
inline constexpr const char* EPUB_DOWNLOAD_DIR = "/Books/ReadBookY";
inline constexpr const char* DICTIONARY_DIR = "/dictionaries";
inline constexpr const char* SIDECAR_SUFFIX = ".readbooky.json";

// Reaction keys matching ReadBookY web app (`public/js/app.js`) exactly
inline constexpr size_t REACTION_COUNT = 6;
inline constexpr const char* REACTION_KEYS[REACTION_COUNT] = {
    "whiteHeart", "thumbsUp", "laughCry", "angry", "crying", "devilSmile",
};
// Monochrome-friendly e-ink labels for the 6 ReadBookY reactions
inline constexpr const char* REACTION_LABELS[REACTION_COUNT] = {
    "Heart", "Like", "Haha", "Angry", "Sad", "Devil",
};
inline constexpr const char* REACTION_SHORT[REACTION_COUNT] = {
    "<3", "+1", "HA", "!!", ":(", ">:)",
};

// Network, heap, and storage safety budgets
inline constexpr size_t MIN_SYNC_FREE_HEAP = 64U * 1024U;
inline constexpr size_t MAX_JSON_RESPONSE_BYTES = 256U * 1024U;
inline constexpr size_t DOWNLOAD_CHUNK_BYTES = 4096U;
inline constexpr uint32_t HTTP_TIMEOUT_MS = 20000U;
inline constexpr uint32_t CLOCK_SYNC_TIMEOUT_MS = 6000U;
inline constexpr uint32_t ID_TOKEN_REFRESH_MARGIN_SEC = 120U;
inline constexpr size_t MAX_OUTBOX_BATCH_OPS = 25U;
inline constexpr size_t MAX_OUTBOX_LINES = 500U;
inline constexpr size_t MAX_PAGE_HISTORY_ITEMS = 40U;

}  // namespace readbooky
