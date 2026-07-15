// MIT License © 2025 Binary Dice Games
/// @file http_client.hpp
/// @brief Minimal blocking HTTP(S) GET client, built on civetweb's client API.
#pragma once

#include <cstddef>
#include <string>

namespace bdg::wish::net {

/**
 * @brief Result of an `http_get()` call.
 */
struct http_response {
  bool ok = false;    ///< `true` if the request completed with a 2xx status.
  std::string body;   ///< Response body; empty when `ok` is `false`.
  std::string error;  ///< Human-readable failure reason; empty when `ok` is `true`.
};

/**
 * @brief Perform a blocking HTTP(S) GET request.
 *
 * Only `http://` and `https://` URLs are supported. This function blocks for
 * the duration of the connection, request, and response -- callers on a
 * latency-sensitive thread (e.g. the render loop) must invoke it from a
 * background thread instead; see `file_service::resolve_or_fetch()`, the
 * only intended caller.
 *
 * @param url         Absolute URL, e.g. "https://example.com/image.png".
 * @param max_bytes   Response bodies larger than this are rejected instead of
 *                    buffered in full, to bound memory use for an
 *                    attacker-controlled remote host.
 * @param timeout_ms  Connect + per-read timeout, in milliseconds.
 * @return `ok == true` with `body` populated on success; otherwise `ok ==
 *         false` with `error` describing the failure (invalid URL,
 *         connection failure, non-2xx status, timeout, or oversized body).
 */
http_response http_get(const std::string& url, size_t max_bytes = 25 * 1024 * 1024, int timeout_ms = 5000);

} // namespace bdg::wish::net
