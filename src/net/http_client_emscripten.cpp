// MIT License © 2026 Binary Dice Games
/// @file http_client_emscripten.cpp
/// @brief bdg::wish::net::http_get for the browser (Emscripten) build, on
///        the browser's own fetch() via emscripten_fetch.
///
/// Uses emscripten_fetch's synchronous mode, which blocks the calling
/// thread. That is only allowed off the browser main thread (pthreads
/// build), so a call from the main thread fails with an error instead of
/// hanging the page. Same-origin and CORS rules of the hosting page apply.
#include <net/http_client.hpp>

#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/threading.h>

#include <cstring>

namespace bdg::wish::net {

http_response http_get(const std::string& url, size_t max_bytes, int timeout_ms) {
  http_response resp;
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    resp.error = "unsupported URL scheme: " + url;
    return resp;
  }
  if (emscripten_is_main_browser_thread()) {
    resp.error = "http_get: synchronous fetch is not allowed on the browser main thread";
    return resp;
  }

  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  std::strcpy(attr.requestMethod, "GET");
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
  attr.timeoutMSecs = timeout_ms > 0 ? static_cast<unsigned long>(timeout_ms) : 0;

  emscripten_fetch_t* fetch = emscripten_fetch(&attr, url.c_str());
  if (!fetch) {
    resp.error = "http_get: emscripten_fetch failed to start for " + url;
    return resp;
  }

  if (fetch->status >= 200 && fetch->status < 300) {
    if (static_cast<size_t>(fetch->numBytes) > max_bytes) {
      resp.error = "response body exceeds " + std::to_string(max_bytes) + " bytes";
    } else {
      resp.ok = true;
      resp.body.assign(fetch->data, static_cast<size_t>(fetch->numBytes));
    }
  } else if (fetch->status == 0) {
    resp.error = "http_get: request failed (network error, timeout or CORS) for " + url;
  } else {
    resp.error = "HTTP status " + std::to_string(fetch->status) + " for " + url;
  }
  emscripten_fetch_close(fetch);
  return resp;
}

} // namespace bdg::wish::net
