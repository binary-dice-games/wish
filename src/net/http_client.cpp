// MIT License © 2025 Binary Dice Games
/// @file http_client.cpp
/// @brief Implementation of bdg::wish::net::http_get, built on civetweb.
#include <net/http_client.hpp>

#include <civetweb.h>

#include <mutex>
#include <optional>
#include <vector>

namespace bdg::wish::net {

namespace {

struct parsed_url {
  bool secure = false;
  std::string host;
  int port = 0;
  std::string path; // includes leading '/', query string, and fragment
};

std::optional<parsed_url> parse_url(const std::string& url) {
  parsed_url out;
  std::string rest;
  if (url.starts_with("https://")) {
    out.secure = true;
    out.port = 443;
    rest = url.substr(8);
  } else if (url.starts_with("http://")) {
    out.secure = false;
    out.port = 80;
    rest = url.substr(7);
  } else {
    return std::nullopt;
  }
  if (rest.empty())
    return std::nullopt;

  auto slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (authority.empty())
    return std::nullopt;

  auto colon = authority.find(':');
  if (colon == std::string::npos) {
    out.host = authority;
  } else {
    out.host = authority.substr(0, colon);
    try {
      out.port = std::stoi(authority.substr(colon + 1));
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  if (out.host.empty())
    return std::nullopt;
  return out;
}

// mg_init_library() must run exactly once, before any client/server use, and
// is not itself thread-safe -- std::call_once serializes that first call.
void ensure_civetweb_initialized() {
  static std::once_flag flag;
  std::call_once(flag, [] { mg_init_library(MG_FEATURES_TLS); });
}

} // namespace

http_response http_get(const std::string& url, size_t max_bytes, int timeout_ms) {
  ensure_civetweb_initialized();

  auto parsed = parse_url(url);
  if (!parsed)
    return {.ok = false, .error = "wish::net::http_get: invalid or unsupported URL: " + url};

  char ebuf[256] = {};
  mg_connection* conn = nullptr;
  if (parsed->secure) {
    mg_client_options opts{};
    opts.host = parsed->host.c_str();
    opts.port = parsed->port;
    conn = mg_connect_client_secure(&opts, ebuf, sizeof(ebuf));
  } else {
    conn = mg_connect_client(parsed->host.c_str(), parsed->port, /*use_ssl=*/0, ebuf, sizeof(ebuf));
  }
  if (!conn)
    return {.ok = false, .error = std::string("wish::net::http_get: connect failed: ") + ebuf};

  std::string request = "GET " + parsed->path + " HTTP/1.1\r\nHost: " + parsed->host + "\r\nConnection: close\r\n\r\n";
  mg_write(conn, request.data(), request.size());

  if (mg_get_response(conn, ebuf, sizeof(ebuf), timeout_ms) < 0) {
    std::string error = std::string("wish::net::http_get: no response: ") + ebuf;
    mg_close_connection(conn);
    return {.ok = false, .error = std::move(error)};
  }

  const mg_response_info* info = mg_get_response_info(conn);
  if (!info) {
    mg_close_connection(conn);
    return {.ok = false, .error = "wish::net::http_get: missing response info"};
  }
  int status_code = info->status_code;

  std::string body;
  std::vector<char> buf(64 * 1024);
  for (;;) {
    int n = mg_read(conn, buf.data(), buf.size());
    if (n < 0)
      break;
    if (n == 0)
      break;
    if (body.size() + static_cast<size_t>(n) > max_bytes) {
      mg_close_connection(conn);
      return {.ok = false, .error = "wish::net::http_get: response exceeds max_bytes (" + std::to_string(max_bytes) + ")"};
    }
    body.append(buf.data(), static_cast<size_t>(n));
  }
  mg_close_connection(conn);

  if (status_code < 200 || status_code >= 300)
    return {.ok = false, .error = "wish::net::http_get: HTTP status " + std::to_string(status_code)};

  return {.ok = true, .body = std::move(body)};
}

} // namespace bdg::wish::net
