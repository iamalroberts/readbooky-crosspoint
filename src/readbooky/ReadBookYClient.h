#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace readbooky {

enum class HttpErrorKind : uint8_t {
  None = 0,
  HttpStatus,        // Real HTTP status (100..599) returned by server
  ConnectFailed,     // TCP/socket connect failure (e.g. ESP_ERR_HTTP_CONNECT)
  Timeout,           // Connection, header, or read timeout
  DnsFailed,         // Hostname DNS resolution failure
  TlsFailed,         // TLS handshake or certificate verification failure
  NoWifi,            // Wi-Fi disconnected
  ClockNotSynced,    // Wall clock not synced for TLS verification
  TransportOther,    // Write/read/peer-close/init failure before a valid HTTP response
  SecurityBlocked,   // Non-HTTPS URL or HTTPS->HTTP downgrade blocked
  ResponseTooLarge,  // Response exceeded safety cap
};

struct HttpResponse {
  int statusCode = 0;  // Strictly 0 on transport failures (never -1); 100..599 only on real HTTP responses
  HttpErrorKind errorKind = HttpErrorKind::None;
  int espErr = 0;        // ESP-IDF esp_err_t code (0 == ESP_OK)
  int sockErrno = 0;     // Socket errno when available
  int tlsErr = 0;        // ESP-TLS / mbedTLS error code when available
  int tlsCertFlags = 0;  // Certificate verification flags when available
  std::string body;
  std::string error;

  bool ok() const { return errorKind == HttpErrorKind::None && statusCode >= 200 && statusCode < 300; }
  bool isHttpResponse() const { return statusCode >= 100 && statusCode < 600; }
  bool isTransportFailure() const {
    return errorKind == HttpErrorKind::ConnectFailed || errorKind == HttpErrorKind::Timeout ||
           errorKind == HttpErrorKind::DnsFailed || errorKind == HttpErrorKind::TlsFailed ||
           errorKind == HttpErrorKind::NoWifi || errorKind == HttpErrorKind::TransportOther;
  }
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
