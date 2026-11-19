#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "ReadBookYModels.h"

namespace readbooky {

// Manages ReadBookY email/password authentication and persistent NVS session on XTEINK X4 Pro.
//
// Security boundaries:
//  - Plaintext user passwords are NEVER stored and are zeroed in RAM immediately after sign-in.
//  - Only the Firebase refresh token + session identity (`uid`, `email`, `displayName`, `color`,
//    `deviceId`) are persisted in NVS namespace `"rby_auth"`.
//  - Short-lived Firebase ID tokens live STRICTLY in RAM and are refreshed on boot/demand.
//  - The persisted refresh token is wrapped with AES-256-GCM using a key derived from
//    `deviceSecret()`. Because `deviceSecret()` lives in NVS on the same flash without
//    ESP32 eFuse flash encryption enabled on stock CrossPoint partitions, this protects against
//    casual extraction and web/plugin API exposure, NOT against a physical flash dump.
class ReadBookYAuth {
 public:
  static ReadBookYAuth& getInstance();

  void begin();
  bool isSignedIn() const;
  ReadBookYSession session() const;
  std::string deviceId() const;

  // Signs in via verified HTTPS to Firebase Auth, persists ONLY wrapped refresh token + identity,
  // scrubs password from memory, and registers the X4 Pro with `/device/v1/devices/register`.
  bool signIn(const std::string& email, std::string& passwordInOut, std::string& errorOut);

  // Returns a fresh RAM-cached Firebase ID token, refreshing via verified HTTPS if absent or expiring.
  bool ensureIdToken(std::string& idTokenOut, std::string& errorOut);

  // Registers/refreshes device record with ReadBookY backend.
  bool registerDevice(std::string& errorOut);

  // Wipes NVS auth namespace, zeroes RAM tokens, and optionally clears ReadBookY SD caches.
  void signOut(bool clearCaches = true);

  // Securely scrubs sensitive string memory before clearing.
  static void secureScrub(std::string& s);

 private:
  ReadBookYAuth() = default;

  bool loadFromNvsLocked();
  bool saveRefreshTokenLocked(const std::string& refreshToken);
  bool loadRefreshTokenLocked(std::string& refreshTokenOut) const;
  bool saveIdentityLocked();
  std::string ensureDeviceIdLocked();

  static bool wrapRefreshToken(const std::string& plaintext, std::string& blobOut);
  static bool unwrapRefreshToken(const std::string& blob, std::string& plaintextOut);

  mutable std::mutex mutex_;
  bool initialized_ = false;
  ReadBookYSession session_;

  // Short-lived ID token lives strictly in RAM (never written to NVS or SD).
  std::string ramIdToken_;
  int64_t ramIdTokenExpiryEpochSec_ = 0;
};

#define READBOOKY_AUTH readbooky::ReadBookYAuth::getInstance()

}  // namespace readbooky
