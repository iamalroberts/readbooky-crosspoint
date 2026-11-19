#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace readbooky {

struct HttpResponse {
  int statusCode = 0;
  std::string body;
  std::string error;
  bool ok() const { return statusCode >= 200 && statusCode < 300; }
};

using DownloadProgressCallback = std::function<void(uint64_t downloadedBytes, uint64_t totalBytes)>;

// Verified-HTTPS-only HTTP client for ReadBookY on XTEINK X4 Pro.
// Security invariants:
//  - Uses ESP-IDF `esp_http_client` with the platform CA bundle (`esp_crt_bundle_attach`).
//  - Verifies certificate chain, SNI, and hostname on every connection.
//  - Enforces a valid wall clock via `trustedtime` before TLS handshakes (fails closed).
//  - Rejects any non-HTTPS URL and any HTTPS -> HTTP redirect downgrade.
//  - Isolates `Authorization: Bearer <idToken>` strictly to the ReadBookY API origin.
class ReadBookYClient {
 public:
  // Verifies Wi-Fi connectivity and a trustworthy system clock for TLS verification.
  static bool ensureReadyForTls(std::string& errorOut);

  static bool isHttpsUrl(const std::string& url);
  static bool matchesApiOrigin(const std::string& url);

  static HttpResponse getJson(const std::string& url, const std::string& bearerToken = "");
  static HttpResponse postJson(const std::string& url, const std::string& jsonBody,
                               const std::string& bearerToken = "");

  // Streams a verified HTTPS file to `<destPath>.part` with HTTP `Range` resume when a matching
  // `.part` + `.part.meta` exists, verifies full streaming SHA-256 against `expectedSha256`,
  // and commits atomically via `Storage.replaceFile()`.
  static bool downloadFileResumable(const std::string& httpsUrl, const std::string& destPath,
                                    const std::string& expectedSha256, uint64_t expectedSize,
                                    std::string& errorOut, const DownloadProgressCallback& onProgress = nullptr);

  // Computes lowercase hex SHA-256 of an SD file using streaming reads.
  static bool computeFileSha256(const std::string& path, std::string& hexOut);
};

}  // namespace readbooky
