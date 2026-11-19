#include "ReadBookYAuth.h"

#ifdef ENABLE_READBOOKY

#include <ArduinoJson.h>
#include <DeviceSecret.h>
#include <HalMemory.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ReadBookYClient.h"
#include "ReadBookYConfig.h"
#include "ReadBookYStorage.h"

namespace readbooky {

namespace {

constexpr uint8_t BLOB_VERSION = 1;
constexpr size_t IV_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t MAX_REFRESH_TOKEN_LEN = 1024;
constexpr int REFRESH_MAX_ATTEMPTS = 3;
constexpr uint32_t REFRESH_BACKOFF_MS[REFRESH_MAX_ATTEMPTS - 1] = {1000U, 2000U};

// Safe console diagnostics for auth refresh (never prints tokens, keys, passwords, or bodies).
void rbyAuthLog(const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  esp_rom_printf("%s\n", buf);
}

// Returns true ONLY for explicit Firebase Auth rejection codes indicating the stored
// refresh token / user session is invalid, revoked, or disabled.
bool isFirebaseAuthRejection(const std::string& responseBody, std::string& codeOut) {
  codeOut.clear();
  if (responseBody.empty()) return false;
  JsonDocument errDoc;
  if (deserializeJson(errDoc, responseBody)) return false;

  std::string msg = errDoc["error"]["message"] | "";
  if (msg.empty()) {
    msg = errDoc["error"]["errors"][0]["message"] | "";
  }
  codeOut = msg;
  return msg == "INVALID_REFRESH_TOKEN" || msg == "TOKEN_EXPIRED" || msg == "USER_DISABLED" ||
         msg == "USER_NOT_FOUND" || msg == "INVALID_GRANT" || msg == "INVALID_GRANT_TYPE" ||
         msg == "PROJECT_NUMBER_MISMATCH";
}

void deriveKeyWithDomain(const uint8_t (&secret)[32], const char* domain, uint8_t (&outKey)[32]) {
  wc_Sha256 sha;
  wc_InitSha256(&sha);
  wc_Sha256Update(&sha, secret, sizeof(secret));
  wc_Sha256Update(&sha, reinterpret_cast<const uint8_t*>(domain), static_cast<word32>(strlen(domain)));
  wc_Sha256Final(&sha, outKey);
  wc_Sha256Free(&sha);
}

bool nvsReadString(nvs_handle_t handle, const char* key, std::string& out) {
  out.clear();
  size_t len = 0;
  if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len == 0 || len > 512) return false;
  out.resize(len);
  if (nvs_get_str(handle, key, out.data(), &len) != ESP_OK) {
    out.clear();
    return false;
  }
  if (!out.empty() && out.back() == '\0') out.pop_back();
  return true;
}

}  // namespace

ReadBookYAuth& ReadBookYAuth::getInstance() {
  static ReadBookYAuth inst;
  return inst;
}

void ReadBookYAuth::secureScrub(std::string& s) {
  if (!s.empty()) {
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(s.data());
    for (size_t i = 0; i < s.size(); ++i) {
      p[i] = 0;
    }
  }
  s.clear();
  s.shrink_to_fit();
}

void ReadBookYAuth::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) return;
  initialized_ = true;
  loadFromNvsLocked();
}

bool ReadBookYAuth::isSignedIn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_.signedIn && !session_.uid.empty();
}

ReadBookYSession ReadBookYAuth::session() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_;
}

std::string ReadBookYAuth::deviceId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_.deviceId;
}

bool ReadBookYAuth::wrapRefreshToken(const std::string& plaintext, std::string& blobOut) {
  blobOut.clear();
  if (plaintext.empty() || plaintext.size() > MAX_REFRESH_TOKEN_LEN) return false;

  uint8_t secret[32] = {};
  if (!deviceSecret(secret)) return false;

  uint8_t encKey[32] = {};
  deriveKeyWithDomain(secret, "rby:rtok:gcm:v1", encKey);
  memset(secret, 0, sizeof(secret));

  uint8_t iv[IV_LEN] = {};
  esp_fill_random(iv, sizeof(iv));

  // AES-256-GCM authenticated encryption (version byte bound as AAD)
  std::string cipher(plaintext.size(), '\0');
  uint8_t tag[TAG_LEN] = {};
  Aes aes;
  const bool encOk =
      wc_AesInit(&aes, nullptr, INVALID_DEVID) == 0 &&
      wc_AesGcmSetKey(&aes, encKey, sizeof(encKey)) == 0 &&
      wc_AesGcmEncrypt(&aes, reinterpret_cast<byte*>(cipher.data()),
                       reinterpret_cast<const byte*>(plaintext.data()), static_cast<word32>(plaintext.size()), iv,
                       static_cast<word32>(sizeof(iv)), tag, static_cast<word32>(sizeof(tag)), &BLOB_VERSION, 1) == 0;
  wc_AesFree(&aes);
  memset(encKey, 0, sizeof(encKey));
  if (!encOk) {
    return false;
  }

  blobOut.reserve(1 + IV_LEN + TAG_LEN + cipher.size());
  blobOut.push_back(static_cast<char>(BLOB_VERSION));
  blobOut.append(reinterpret_cast<const char*>(iv), sizeof(iv));
  blobOut.append(reinterpret_cast<const char*>(tag), sizeof(tag));
  blobOut.append(cipher);
  return true;
}

bool ReadBookYAuth::unwrapRefreshToken(const std::string& blob, std::string& plaintextOut) {
  plaintextOut.clear();
  if (blob.size() <= 1 + IV_LEN + TAG_LEN) return false;
  if (static_cast<uint8_t>(blob[0]) != BLOB_VERSION) return false;

  const size_t cipherLen = blob.size() - (1 + IV_LEN + TAG_LEN);
  if (cipherLen == 0 || cipherLen > MAX_REFRESH_TOKEN_LEN) return false;

  uint8_t secret[32] = {};
  if (!deviceSecret(secret)) return false;

  uint8_t encKey[32] = {};
  deriveKeyWithDomain(secret, "rby:rtok:gcm:v1", encKey);
  memset(secret, 0, sizeof(secret));

  const uint8_t* aad = reinterpret_cast<const uint8_t*>(blob.data());
  const uint8_t* iv = reinterpret_cast<const uint8_t*>(blob.data() + 1);
  const uint8_t* tag = reinterpret_cast<const uint8_t*>(blob.data() + 1 + IV_LEN);
  const uint8_t* cipher = reinterpret_cast<const uint8_t*>(blob.data() + 1 + IV_LEN + TAG_LEN);

  plaintextOut.resize(cipherLen);
  Aes aes;
  const bool decOk =
      wc_AesInit(&aes, nullptr, INVALID_DEVID) == 0 &&
      wc_AesGcmSetKey(&aes, encKey, sizeof(encKey)) == 0 &&
      wc_AesGcmDecrypt(&aes, reinterpret_cast<byte*>(plaintextOut.data()), cipher, static_cast<word32>(cipherLen), iv,
                       static_cast<word32>(IV_LEN), tag, static_cast<word32>(TAG_LEN), aad, 1) == 0;
  wc_AesFree(&aes);
  memset(encKey, 0, sizeof(encKey));
  if (!decOk) {
    secureScrub(plaintextOut);
    LOG_ERR("RBYAUTH", "Refresh token integrity check failed");
    return false;
  }
  return true;
}

std::string ReadBookYAuth::ensureDeviceIdLocked() {
  if (!session_.deviceId.empty()) return session_.deviceId;

  uint8_t secret[32] = {};
  if (deviceSecret(secret)) {
    uint8_t hash[32] = {};
    deriveKeyWithDomain(secret, "rby:device-id:v1", hash);
    memset(secret, 0, sizeof(secret));
    char buf[40];
    snprintf(buf, sizeof(buf), "x4p_%02x%02x%02x%02x%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3], hash[4],
             hash[5], hash[6], hash[7]);
    session_.deviceId = buf;
  } else {
    uint8_t rnd[8] = {};
    esp_fill_random(rnd, sizeof(rnd));
    char buf[40];
    snprintf(buf, sizeof(buf), "x4p_%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5],
             rnd[6], rnd[7]);
    session_.deviceId = buf;
  }
  return session_.deviceId;
}

bool ReadBookYAuth::loadFromNvsLocked() {
  nvs_handle_t handle;
  if (nvs_open(NVS_AUTH_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    ensureDeviceIdLocked();
    return false;
  }

  nvsReadString(handle, "devid", session_.deviceId);
  const bool hasUid = nvsReadString(handle, "uid", session_.uid);
  nvsReadString(handle, "email", session_.email);
  nvsReadString(handle, "dname", session_.displayName);
  nvsReadString(handle, "color", session_.color);
  if (session_.color.empty()) session_.color = "#999999";

  size_t rtokLen = 0;
  const bool hasRefreshBlob = (nvs_get_blob(handle, "rtok", nullptr, &rtokLen) == ESP_OK && rtokLen > 1 + IV_LEN + TAG_LEN);
  nvs_close(handle);

  ensureDeviceIdLocked();
  session_.signedIn = hasUid && !session_.uid.empty() && hasRefreshBlob;
  if (session_.signedIn) {
    LOG_INF("RBYAUTH", "Restored ReadBookY session for %s (refresh token only; ID token RAM-only)",
            session_.email.c_str());
  }
  return session_.signedIn;
}

bool ReadBookYAuth::saveIdentityLocked() {
  nvs_handle_t handle;
  if (nvs_open(NVS_AUTH_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    LOG_ERR("RBYAUTH", "Failed opening NVS namespace %s", NVS_AUTH_NAMESPACE);
    return false;
  }
  const std::string devId = ensureDeviceIdLocked();
  bool ok = nvs_set_str(handle, "uid", session_.uid.c_str()) == ESP_OK &&
            nvs_set_str(handle, "email", session_.email.c_str()) == ESP_OK &&
            nvs_set_str(handle, "dname", session_.displayName.c_str()) == ESP_OK &&
            nvs_set_str(handle, "color", session_.color.c_str()) == ESP_OK &&
            nvs_set_str(handle, "devid", devId.c_str()) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

bool ReadBookYAuth::saveRefreshTokenLocked(const std::string& refreshToken) {
  std::string wrapped;
  if (!wrapRefreshToken(refreshToken, wrapped)) {
    LOG_ERR("RBYAUTH", "Failed wrapping refresh token");
    return false;
  }
  nvs_handle_t handle;
  if (nvs_open(NVS_AUTH_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  const bool ok = nvs_set_blob(handle, "rtok", wrapped.data(), wrapped.size()) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

bool ReadBookYAuth::loadRefreshTokenLocked(std::string& refreshTokenOut) const {
  refreshTokenOut.clear();
  nvs_handle_t handle;
  if (nvs_open(NVS_AUTH_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t len = 0;
  if (nvs_get_blob(handle, "rtok", nullptr, &len) != ESP_OK || len == 0 || len > MAX_REFRESH_TOKEN_LEN + 64) {
    nvs_close(handle);
    return false;
  }
  std::string blob(len, '\0');
  const bool readOk = nvs_get_blob(handle, "rtok", blob.data(), &len) == ESP_OK;
  nvs_close(handle);
  if (!readOk) return false;
  return unwrapRefreshToken(blob, refreshTokenOut);
}

bool ReadBookYAuth::signIn(const std::string& email, std::string& passwordInOut, std::string& errorOut) {
  errorOut.clear();
  if (email.empty() || passwordInOut.empty()) {
    secureScrub(passwordInOut);
    errorOut = "Email and password are required";
    return false;
  }

  JsonDocument reqDoc;
  reqDoc["email"] = email;
  reqDoc["password"] = passwordInOut;
  reqDoc["returnSecureToken"] = true;

  std::string reqBody;
  serializeJson(reqDoc, reqBody);
  reqDoc.clear();
  // Scrub caller's plaintext password immediately before even waiting on network
  secureScrub(passwordInOut);

  const HttpResponse res = ReadBookYClient::postJson(READBOOKY_AUTH_SIGNIN_URL, reqBody);
  // Scrub serialized request body containing password immediately after send
  secureScrub(reqBody);

  if (!res.ok()) {
    JsonDocument errDoc;
    if (!res.body.empty() && !deserializeJson(errDoc, res.body)) {
      const std::string msg = errDoc["error"]["message"] | "";
      if (msg == "INVALID_LOGIN_CREDENTIALS" || msg == "INVALID_PASSWORD" || msg == "EMAIL_NOT_FOUND") {
        errorOut = "Invalid email or password";
      } else if (msg == "TOO_MANY_ATTEMPTS_TRY_LATER") {
        errorOut = "Too many attempts; try later";
      } else if (!msg.empty()) {
        errorOut = msg;
      }
    }
    if (errorOut.empty()) {
      if (res.isTransportFailure()) {
        errorOut = "Can't reach ReadBooky right now.";
      } else {
        errorOut = res.error.empty() ? "Sign-in failed" : res.error;
      }
    }
    return false;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, res.body)) {
    errorOut = "Invalid auth response";
    return false;
  }

  std::string idToken = respDoc["idToken"] | "";
  std::string refreshToken = respDoc["refreshToken"] | "";
  const std::string uid = respDoc["localId"] | "";
  const std::string respEmail = respDoc["email"] | email;
  const std::string dname = respDoc["displayName"] | "";
  const int64_t expiresIn = atoll(respDoc["expiresIn"] | "3600");

  if (idToken.empty() || refreshToken.empty() || uid.empty()) {
    secureScrub(idToken);
    secureScrub(refreshToken);
    errorOut = "Incomplete auth response";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    ensureDeviceIdLocked();
    session_.uid = uid;
    session_.email = respEmail;
    session_.displayName = dname.empty() ? respEmail : dname;
    session_.signedIn = true;

    // Persist ONLY wrapped refresh token + identity in NVS. ID token stays strictly in RAM!
    const bool savedTok = saveRefreshTokenLocked(refreshToken);
    const bool savedId = saveIdentityLocked();
    secureScrub(refreshToken);

    if (!savedTok || !savedId) {
      secureScrub(idToken);
      session_.signedIn = false;
      errorOut = "Failed saving session to NVS";
      return false;
    }

    ramIdToken_ = std::move(idToken);
    const int64_t nowSec = trustedtime::trustedNow();
    ramIdTokenExpiryEpochSec_ = nowSec + (expiresIn > 120 ? expiresIn : 3600);
  }

  // Register device and fetch authoritative ReadBookY profile displayName + color
  std::string regErr;
  if (!registerDevice(regErr)) {
    LOG_ERR("RBYAUTH", "Device registration deferred: %s", regErr.c_str());
  }
  return true;
}

bool ReadBookYAuth::ensureIdToken(std::string& idTokenOut, std::string& errorOut) {
  idTokenOut.clear();
  errorOut.clear();

  std::string refreshToken;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_.signedIn || session_.uid.empty()) {
      errorOut = "Not signed in to ReadBooky";
      return false;
    }
    const int64_t nowSec = trustedtime::trustedNow();
    if (!ramIdToken_.empty() && nowSec > 0 && ramIdTokenExpiryEpochSec_ > nowSec + ID_TOKEN_REFRESH_MARGIN_SEC) {
      idTokenOut = ramIdToken_;
      return true;
    }
    if (!loadRefreshTokenLocked(refreshToken)) {
      errorOut = "Refresh token missing; please sign in again";
      return false;
    }
  }

  JsonDocument reqDoc;
  reqDoc["grant_type"] = "refresh_token";
  reqDoc["refresh_token"] = refreshToken;
  std::string reqBody;
  serializeJson(reqDoc, reqBody);
  reqDoc.clear();

  HttpResponse res;
  for (int attempt = 1; attempt <= REFRESH_MAX_ATTEMPTS; ++attempt) {
    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    const int rssi = wifiConnected ? WiFi.RSSI() : 0;
    const auto heap = HalMemory::getInternalHeap();

    rbyAuthLog("[RBY-AUTH] refresh attempt %d/%d", attempt, REFRESH_MAX_ATTEMPTS);
    rbyAuthLog("[RBY-AUTH] host=securetoken.googleapis.com");
    rbyAuthLog("[RBY-AUTH] wifiConnected=%s", wifiConnected ? "true" : "false");
    rbyAuthLog("[RBY-AUTH] RSSI=%d", rssi);
    rbyAuthLog("[RBY-AUTH] freeHeap=%zu", heap.freeBytes);
    rbyAuthLog("[RBY-AUTH] largestBlock=%zu", heap.largestBlockBytes);

    if (!wifiConnected) {
      res = {};
      res.errorKind = HttpErrorKind::NoWifi;
      res.error = "Wi-Fi not connected";
      rbyAuthLog("[RBY-AUTH] open=NO_WIFI");
      break;
    }

    res = ReadBookYClient::postJson(READBOOKY_AUTH_REFRESH_URL, reqBody);

    if (res.espErr != 0) {
      rbyAuthLog("[RBY-AUTH] open=%s (errno=%d tlsErr=0x%x certFlags=0x%x)",
                 esp_err_to_name(static_cast<esp_err_t>(res.espErr)), res.sockErrno, res.tlsErr, res.tlsCertFlags);
    } else if (res.isHttpResponse()) {
      rbyAuthLog("[RBY-AUTH] open=ESP_OK httpStatus=%d", res.statusCode);
    } else {
      rbyAuthLog("[RBY-AUTH] open=ESP_FAIL kind=%u", static_cast<unsigned>(res.errorKind));
    }

    if (res.ok()) {
      break;
    }

    // Never retry client-side auth rejections (400/401/403) or non-transient security/clock errors
    if (res.isHttpResponse() && res.statusCode >= 400 && res.statusCode < 500) {
      break;
    }
    if (res.errorKind == HttpErrorKind::ClockNotSynced || res.errorKind == HttpErrorKind::SecurityBlocked) {
      break;
    }

    // Retry transient transport/5xx failures with bounded backoff (1s, then 2s) while Wi-Fi remains connected
    if (attempt < REFRESH_MAX_ATTEMPTS && (res.isTransportFailure() || res.statusCode >= 500) &&
        WiFi.status() == WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(REFRESH_BACKOFF_MS[attempt - 1]));
    } else {
      break;
    }
  }
  secureScrub(reqBody);

  if (!res.ok()) {
    secureScrub(refreshToken);

    // Only treat real HTTP 400/401/403 with Firebase auth rejection codes as requiring re-sign-in.
    // Network/transport failures ALWAYS preserve the existing refresh token and signed-in session.
    if (res.isHttpResponse() && (res.statusCode == 400 || res.statusCode == 401 || res.statusCode == 403)) {
      std::string fbErrCode;
      if (isFirebaseAuthRejection(res.body, fbErrCode)) {
        rbyAuthLog("[RBY-AUTH] rejected code=%s", fbErrCode.c_str());
        errorOut = "Session expired; please sign in again.";
      } else {
        errorOut = "Can't verify account right now (HTTP " + std::to_string(res.statusCode) +
                   "). Your account is still signed in.";
      }
    } else if (res.errorKind == HttpErrorKind::ClockNotSynced) {
      errorOut = "Clock not synced for secure sign-in. Your account is still signed in.";
    } else if (res.errorKind == HttpErrorKind::NoWifi) {
      errorOut = "Wi-Fi not connected. Your account is still signed in.";
    } else if (res.isTransportFailure() || !res.isHttpResponse()) {
      errorOut = "Can't reach ReadBooky right now. Your account is still signed in.";
    } else {
      errorOut = "ReadBooky temporarily unavailable (HTTP " + std::to_string(res.statusCode) +
                 "). Your account is still signed in.";
    }
    return false;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, res.body)) {
    secureScrub(refreshToken);
    errorOut = "Invalid token refresh response";
    return false;
  }

  std::string newIdToken = respDoc["id_token"] | "";
  std::string newRefreshToken = respDoc["refresh_token"] | "";
  const int64_t expiresIn = atoll(respDoc["expires_in"] | "3600");

  if (newIdToken.empty()) {
    secureScrub(refreshToken);
    secureScrub(newRefreshToken);
    errorOut = "Empty refreshed ID token";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    ramIdToken_ = newIdToken;
    const int64_t nowSec = trustedtime::trustedNow();
    ramIdTokenExpiryEpochSec_ = nowSec + (expiresIn > 120 ? expiresIn : 3600);

    // Avoid unnecessary NVS writes: only update NVS if Google rotated the refresh token!
    if (!newRefreshToken.empty() && newRefreshToken != refreshToken) {
      saveRefreshTokenLocked(newRefreshToken);
    }
  }

  secureScrub(refreshToken);
  secureScrub(newRefreshToken);
  idTokenOut = std::move(newIdToken);
  return true;
}

bool ReadBookYAuth::registerDevice(std::string& errorOut) {
  std::string idToken;
  if (!ensureIdToken(idToken, errorOut)) return false;

  const std::string devId = deviceId();
  JsonDocument reqDoc;
  reqDoc["deviceId"] = devId;
  reqDoc["deviceModel"] = DEVICE_MODEL;
#ifdef CROSSPOINT_VERSION
  reqDoc["firmwareVersion"] = CROSSPOINT_VERSION;
#else
  reqDoc["firmwareVersion"] = "2.0.0-readbooky";
#endif
  JsonArray caps = reqDoc["capabilities"].to<JsonArray>();
  caps.add("highlights");
  caps.add("comments");
  caps.add("reactions");
  caps.add("page_async");
  caps.add("range_download");
  caps.add("offline_dict");

  std::string reqBody;
  serializeJson(reqDoc, reqBody);

  const std::string url = std::string(READBOOKY_API_BASE_URL) + "/devices/register";
  const HttpResponse res = ReadBookYClient::postJson(url, reqBody, idToken);
  secureScrub(idToken);

  if (!res.ok()) {
    errorOut = res.error.empty() ? "Device registration failed" : res.error;
    return false;
  }

  JsonDocument respDoc;
  if (!deserializeJson(respDoc, res.body)) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string dname = respDoc["user"]["displayName"] | "";
    const std::string color = respDoc["user"]["color"] | "";
    bool changed = false;
    if (!dname.empty() && dname != session_.displayName) {
      session_.displayName = dname;
      changed = true;
    }
    if (!color.empty() && color != session_.color) {
      session_.color = color;
      changed = true;
    }
    if (changed) saveIdentityLocked();
  }
  return true;
}

void ReadBookYAuth::signOut(const bool clearCaches) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    secureScrub(ramIdToken_);
    ramIdTokenExpiryEpochSec_ = 0;

    nvs_handle_t handle;
    if (nvs_open(NVS_AUTH_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
      nvs_erase_all(handle);
      nvs_commit(handle);
      nvs_close(handle);
    }

    const std::string preservedDevId = session_.deviceId;
    session_ = {};
    session_.deviceId = preservedDevId;
  }

  if (clearCaches) {
    ReadBookYStorage::clearUserCaches();
  }
  LOG_INF("RBYAUTH", "Signed out of ReadBookY and cleared session tokens");
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
