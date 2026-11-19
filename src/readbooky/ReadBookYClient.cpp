#include "ReadBookYClient.h"

#ifdef ENABLE_READBOOKY

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "ReadBookYConfig.h"
#include "ReadBookYStorage.h"

namespace readbooky {

namespace {

constexpr int MAX_HTTPS_REDIRECTS = 4;

std::string extractOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return "";
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  std::string origin = pathStart == std::string::npos ? url : url.substr(0, pathStart);
  for (char& c : origin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return origin;
}

std::string toLowerHex(const uint8_t* bytes, const size_t len) {
  static constexpr char kHexChars[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHexChars[(bytes[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kHexChars[bytes[i] & 0x0F];
  }
  return out;
}

struct HeaderCapture {
  std::string location;
  std::string contentRange;
};

esp_err_t captureHeadersEventHandler(esp_http_client_event_t* evt) {
  if (!evt || evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) return ESP_OK;
  auto* cap = static_cast<HeaderCapture*>(evt->user_data);
  if (evt->header_key && evt->header_value) {
    if (strcasecmp(evt->header_key, "Location") == 0) {
      cap->location = evt->header_value;
    } else if (strcasecmp(evt->header_key, "Content-Range") == 0) {
      cap->contentRange = evt->header_value;
    }
  }
  return ESP_OK;
}

HttpResponse executeJsonRequest(const esp_http_client_method_t method, const std::string& initialUrl,
                                const std::string& requestBody, const std::string& bearerToken) {
  HttpResponse res;
  if (!ReadBookYClient::ensureReadyForTls(res.error)) {
    return res;
  }

  std::string currentUrl = initialUrl;
  const std::string initialOrigin = extractOrigin(initialUrl);

  for (int redirect = 0; redirect <= MAX_HTTPS_REDIRECTS; ++redirect) {
    if (!ReadBookYClient::isHttpsUrl(currentUrl)) {
      res.error = "Blocked non-HTTPS URL";
      LOG_ERR("RBYHTTP", "Refused non-HTTPS URL: %s", currentUrl.c_str());
      return res;
    }

    HeaderCapture headers;
    esp_http_client_config_t cfg = {};
    cfg.url = currentUrl.c_str();
    cfg.method = method;
    cfg.timeout_ms = static_cast<int>(HTTP_TIMEOUT_MS);
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // Platform CA bundle: chain + SNI + hostname verification
    cfg.skip_cert_common_name_check = false;
    cfg.disable_auto_redirect = true;  // Manually enforce HTTPS-only redirects & Bearer origin isolation
    cfg.event_handler = captureHeadersEventHandler;
    cfg.user_data = &headers;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
      res.error = "HTTP client init failed";
      return res;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ReadBookY-CrossPoint-X4Pro/1.0");
    if (!requestBody.empty()) {
      esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    // Send Bearer token ONLY to ReadBookY's configured API origin (and never after cross-origin redirects)
    if (!bearerToken.empty() && ReadBookYClient::matchesApiOrigin(currentUrl) &&
        extractOrigin(currentUrl) == initialOrigin) {
      const std::string authVal = "Bearer " + bearerToken;
      esp_http_client_set_header(client, "Authorization", authVal.c_str());
    }

    esp_err_t err = esp_http_client_open(client, static_cast<int>(requestBody.size()));
    if (err != ESP_OK) {
      res.error = "HTTPS connection failed (" + std::string(esp_err_to_name(err)) + ")";
      LOG_ERR("RBYHTTP", "HTTPS open failed (%s) for %s", esp_err_to_name(err), currentUrl.c_str());
      esp_http_client_cleanup(client);
      return res;
    }

    if (!requestBody.empty()) {
      size_t sent = 0;
      while (sent < requestBody.size()) {
        const int w = esp_http_client_write(client, requestBody.data() + sent,
                                            static_cast<int>(requestBody.size() - sent));
        if (w <= 0) {
          res.error = "HTTPS request write failed";
          esp_http_client_close(client);
          esp_http_client_cleanup(client);
          return res;
        }
        sent += static_cast<size_t>(w);
      }
    }

    const int64_t contentLen = esp_http_client_fetch_headers(client);
    res.statusCode = esp_http_client_get_status_code(client);

    // Handle HTTPS redirects safely
    if ((res.statusCode == 301 || res.statusCode == 302 || res.statusCode == 303 || res.statusCode == 307 ||
         res.statusCode == 308) &&
        !headers.location.empty()) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      if (!ReadBookYClient::isHttpsUrl(headers.location)) {
        res.statusCode = 0;
        res.error = "Blocked HTTPS-to-HTTP redirect";
        LOG_ERR("RBYHTTP", "Refused insecure redirect to %s", headers.location.c_str());
        return res;
      }
      currentUrl = headers.location;
      continue;
    }

    if (contentLen > static_cast<int64_t>(MAX_JSON_RESPONSE_BYTES)) {
      res.error = "Response exceeds safety cap";
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return res;
    }

    res.body.clear();
    if (contentLen > 0) {
      res.body.reserve(static_cast<size_t>(contentLen));
    }

    auto buf = makeUniqueNoThrow<char[]>(2048);
    if (!buf) {
      res.error = "Out of memory reading response";
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return res;
    }

    bool overflow = false;
    while (true) {
      const int n = esp_http_client_read(client, buf.get(), 2048);
      if (n < 0) {
        res.error = "HTTPS read error";
        break;
      }
      if (n == 0) break;
      if (res.body.size() + static_cast<size_t>(n) > MAX_JSON_RESPONSE_BYTES) {
        overflow = true;
        res.error = "Response exceeded 256 KB limit";
        break;
      }
      res.body.append(buf.get(), static_cast<size_t>(n));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (overflow) {
      res.body.clear();
      res.statusCode = 0;
    } else if (!res.ok() && res.error.empty()) {
      res.error = "HTTP " + std::to_string(res.statusCode);
    }
    return res;
  }

  res.statusCode = 0;
  res.error = "Too many HTTPS redirects";
  return res;
}

bool readPartMeta(const std::string& metaPath, std::string& shaOut, uint64_t& sizeOut) {
  std::string raw;
  if (!ReadBookYStorage::readStringFile(metaPath, 1024, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  shaOut = doc["sha256"] | "";
  sizeOut = doc["expectedSize"] | 0ULL;
  return !shaOut.empty();
}

void writePartMeta(const std::string& metaPath, const std::string& sha256, const uint64_t expectedSize) {
  JsonDocument doc;
  doc["sha256"] = sha256;
  doc["expectedSize"] = expectedSize;
  std::string out;
  serializeJson(doc, out);
  ReadBookYStorage::writeAtomicString(metaPath, out);
}

}  // namespace

bool ReadBookYClient::isHttpsUrl(const std::string& url) {
  return url.size() > 8 && strncasecmp(url.c_str(), "https://", 8) == 0;
}

bool ReadBookYClient::matchesApiOrigin(const std::string& url) {
  const std::string apiOrigin = extractOrigin(READBOOKY_API_BASE_URL);
  const std::string urlOrigin = extractOrigin(url);
  return !apiOrigin.empty() && apiOrigin == urlOrigin;
}

bool ReadBookYClient::ensureReadyForTls(std::string& errorOut) {
  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "Wi-Fi not connected";
    return false;
  }
  if (trustedtime::trustedNow() == 0) {
    LOG_INF("RBYHTTP", "Syncing clock via SNTP before verified HTTPS");
    if (!trustedtime::syncNow(CLOCK_SYNC_TIMEOUT_MS) || trustedtime::trustedNow() == 0) {
      errorOut = "Clock not synced (TLS requires valid time)";
      LOG_ERR("RBYHTTP", "%s", errorOut.c_str());
      return false;
    }
  }
  return true;
}

HttpResponse ReadBookYClient::getJson(const std::string& url, const std::string& bearerToken) {
  return executeJsonRequest(HTTP_METHOD_GET, url, "", bearerToken);
}

HttpResponse ReadBookYClient::postJson(const std::string& url, const std::string& jsonBody,
                                       const std::string& bearerToken) {
  return executeJsonRequest(HTTP_METHOD_POST, url, jsonBody, bearerToken);
}

bool ReadBookYClient::computeFileSha256(const std::string& path, std::string& hexOut) {
  hexOut.clear();
  HalFile file;
  if (!Storage.openFileForRead("RBYSHA", path, file)) {
    return false;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(DOWNLOAD_CHUNK_BYTES);
  if (!buf) {
    file.close();
    return false;
  }

  wc_Sha256 sha;
  if (wc_InitSha256(&sha) != 0) {
    file.close();
    return false;
  }

  while (true) {
    const int n = file.read(buf.get(), DOWNLOAD_CHUNK_BYTES);
    if (n < 0) {
      wc_Sha256Free(&sha);
      file.close();
      return false;
    }
    if (n == 0) break;
    wc_Sha256Update(&sha, buf.get(), static_cast<word32>(n));
  }

  uint8_t digest[WC_SHA256_DIGEST_SIZE] = {};
  wc_Sha256Final(&sha, digest);
  wc_Sha256Free(&sha);
  file.close();

  hexOut = toLowerHex(digest, sizeof(digest));
  return true;
}

bool ReadBookYClient::downloadFileResumable(const std::string& httpsUrl, const std::string& destPath,
                                            const std::string& expectedSha256, const uint64_t expectedSize,
                                            std::string& errorOut, const DownloadProgressCallback& onProgress) {
  if (!ensureReadyForTls(errorOut)) return false;
  if (!isHttpsUrl(httpsUrl)) {
    errorOut = "Blocked non-HTTPS download URL";
    return false;
  }

  std::string lowerExpectedSha = expectedSha256;
  for (char& c : lowerExpectedSha) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  const std::string partPath = destPath + ".part";
  const std::string metaPath = destPath + ".part.meta";

  uint64_t resumeOffset = 0;
  if (Storage.exists(partPath.c_str())) {
    std::string metaSha;
    uint64_t metaSize = 0;
    if (readPartMeta(metaPath, metaSha, metaSize) && metaSha == lowerExpectedSha &&
        (expectedSize == 0 || metaSize == expectedSize)) {
      HalFile existing;
      if (Storage.openFileForRead("RBYDL", partPath, existing)) {
        const uint64_t sz = existing.fileSize64();
        existing.close();
        if (sz > 0 && (expectedSize == 0 || sz < expectedSize)) {
          resumeOffset = sz;
          LOG_INF("RBYDL", "Resuming partial download at %llu bytes", static_cast<unsigned long long>(resumeOffset));
        } else if (expectedSize > 0 && sz == expectedSize) {
          // Already fully downloaded to .part; verify SHA-256 directly!
          std::string actualSha;
          if (computeFileSha256(partPath, actualSha) && (lowerExpectedSha.empty() || actualSha == lowerExpectedSha)) {
            if (Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
              Storage.remove(metaPath.c_str());
              if (onProgress) onProgress(expectedSize, expectedSize);
              return true;
            }
          }
          Storage.remove(partPath.c_str());
          Storage.remove(metaPath.c_str());
        }
      }
    } else {
      Storage.remove(partPath.c_str());
      Storage.remove(metaPath.c_str());
    }
  }

  writePartMeta(metaPath, lowerExpectedSha, expectedSize);

  std::string currentUrl = httpsUrl;
  for (int redirect = 0; redirect <= MAX_HTTPS_REDIRECTS; ++redirect) {
    if (!isHttpsUrl(currentUrl)) {
      errorOut = "Blocked HTTPS-to-HTTP redirect";
      return false;
    }

    HeaderCapture headers;
    esp_http_client_config_t cfg = {};
    cfg.url = currentUrl.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = static_cast<int>(HTTP_TIMEOUT_MS);
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.skip_cert_common_name_check = false;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = captureHeadersEventHandler;
    cfg.user_data = &headers;
    cfg.buffer_size = static_cast<int>(DOWNLOAD_CHUNK_BYTES);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
      errorOut = "HTTP client init failed";
      return false;
    }

    // Never send ReadBookY Authorization Bearer token to external GCS/Storage signed download URLs!
    esp_http_client_set_header(client, "User-Agent", "ReadBookY-CrossPoint-X4Pro/1.0");
    if (resumeOffset > 0) {
      char rangeBuf[64];
      snprintf(rangeBuf, sizeof(rangeBuf), "bytes=%llu-", static_cast<unsigned long long>(resumeOffset));
      esp_http_client_set_header(client, "Range", rangeBuf);
    }

    const esp_err_t openErr = esp_http_client_open(client, 0);
    if (openErr != ESP_OK) {
      errorOut = "Download connect failed (" + std::string(esp_err_to_name(openErr)) + ")";
      esp_http_client_cleanup(client);
      return false;
    }

    const int64_t contentLen = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        !headers.location.empty()) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      if (!isHttpsUrl(headers.location)) {
        errorOut = "Blocked insecure download redirect";
        return false;
      }
      currentUrl = headers.location;
      continue;
    }

    if (status == 416) {
      // Requested range invalid -> restart cleanly from byte 0
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      Storage.remove(partPath.c_str());
      resumeOffset = 0;
      continue;
    }

    if (status != 200 && status != 206) {
      errorOut = "Download HTTP " + std::to_string(status);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }

    const bool appending = (status == 206 && resumeOffset > 0);
    uint64_t writtenTotal = appending ? resumeOffset : 0;
    const uint64_t totalExpected = expectedSize > 0
                                       ? expectedSize
                                       : (contentLen > 0 ? (writtenTotal + static_cast<uint64_t>(contentLen)) : 0);

    HalFile outFile = appending ? Storage.open(partPath.c_str(), O_RDWR | O_CREAT | O_APPEND)
                                : Storage.open(partPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!outFile) {
      errorOut = "Failed to open .part file on SD";
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }

    auto buf = makeUniqueNoThrow<uint8_t[]>(DOWNLOAD_CHUNK_BYTES);
    if (!buf) {
      errorOut = "Out of memory during download";
      outFile.close();
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }

    bool ioOk = true;
    uint64_t lastReported = writtenTotal;
    if (onProgress) onProgress(writtenTotal, totalExpected);

    while (true) {
      const int n = esp_http_client_read(client, reinterpret_cast<char*>(buf.get()), DOWNLOAD_CHUNK_BYTES);
      if (n < 0) {
        errorOut = "Network interrupted during download (resumable)";
        ioOk = false;
        break;
      }
      if (n == 0) break;

      const size_t w = outFile.write(buf.get(), static_cast<size_t>(n));
      if (w != static_cast<size_t>(n)) {
        errorOut = "SD card write failed";
        ioOk = false;
        break;
      }
      writtenTotal += static_cast<uint64_t>(w);
      if (onProgress && (writtenTotal - lastReported >= 32768 || (totalExpected > 0 && writtenTotal >= totalExpected))) {
        onProgress(writtenTotal, totalExpected);
        lastReported = writtenTotal;
      }
    }

    outFile.flush();
    outFile.close();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ioOk) {
      // Keep `.part` and `.part.meta` intact so next attempt resumes via HTTP Range!
      return false;
    }

    if (expectedSize > 0 && writtenTotal != expectedSize) {
      errorOut = "Incomplete download (" + std::to_string(writtenTotal) + "/" + std::to_string(expectedSize) + ")";
      return false;
    }

    if (!lowerExpectedSha.empty()) {
      std::string actualSha;
      if (!computeFileSha256(partPath, actualSha)) {
        errorOut = "Failed computing SHA-256";
        return false;
      }
      if (actualSha != lowerExpectedSha) {
        LOG_ERR("RBYDL", "SHA-256 mismatch: expected %s, got %s", lowerExpectedSha.c_str(), actualSha.c_str());
        Storage.remove(partPath.c_str());
        Storage.remove(metaPath.c_str());
        errorOut = "SHA-256 verification failed";
        return false;
      }
    }

    if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
      errorOut = "Failed finalizing downloaded file";
      return false;
    }
    Storage.remove(metaPath.c_str());
    if (onProgress) onProgress(writtenTotal, totalExpected > 0 ? totalExpected : writtenTotal);
    return true;
  }

  errorOut = "Too many download redirects";
  return false;
}

}  // namespace readbooky

#endif  // ENABLE_READBOOKY
