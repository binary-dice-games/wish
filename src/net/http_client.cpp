// MIT License © 2025 Binary Dice Games
/// @file http_client.cpp
/// @brief Implementation of bdg::wish::net::http_get, built on libcurl.
#include <net/http_client.hpp>

#include <curl/curl.h>

#include <mutex>

namespace bdg::wish::net {

namespace {

// curl_global_init() must run exactly once, before any other libcurl call,
// and is not itself thread-safe -- std::call_once serializes that first call.
void ensure_curl_initialized() {
  static std::once_flag flag;
  std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct write_context {
  std::string* body = nullptr;
  size_t max_bytes = 0;
  bool truncated = false;
};

size_t on_write(char* data, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<write_context*>(userdata);
  size_t n = size * nmemb;
  if (ctx->body->size() + n > ctx->max_bytes) {
    ctx->truncated = true;
    return 0; // abort the transfer
  }
  ctx->body->append(data, n);
  return n;
}

} // namespace

http_response http_get(const std::string& url, size_t max_bytes, int timeout_ms) {
  ensure_curl_initialized();

  CURL* curl = curl_easy_init();
  if (!curl)
    return {.ok = false, .error = "wish::net::http_get: curl_easy_init failed"};

  std::string body;
  write_context ctx{&body, max_bytes, false};

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    std::string error = ctx.truncated
        ? "wish::net::http_get: response exceeds max_bytes (" + std::to_string(max_bytes) + ")"
        : std::string("wish::net::http_get: ") + curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return {.ok = false, .error = std::move(error)};
  }

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_cleanup(curl);

  if (status_code < 200 || status_code >= 300)
    return {.ok = false, .error = "wish::net::http_get: HTTP status " + std::to_string(status_code)};

  return {.ok = true, .body = std::move(body)};
}

} // namespace bdg::wish::net
