// MIT License © 2025 Binary Dice Games
/// @file civetweb_server.hpp
/// @brief Pimpl wrapper isolating civetweb's C API from web_renderer.hpp.
#pragma once

#ifdef WISH_WEB_ENABLED

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace bdg::wish {

/// @brief Opaque per-connection handle (wraps a civetweb `mg_connection*`).
///        Only meaningful as an argument back to `send_to()`.
using ws_connection_id = void*;

/**
 * @brief Thin HTTP + WebSocket server wrapper around civetweb.
 *
 * `civetweb_server.hpp` never includes `<civetweb.h>` — that isolation
 * lives entirely in `civetweb_server.cpp` — so `web_renderer.hpp` (which
 * includes this header) never needs civetweb in its include path.
 *
 * Static file serving (from `document_root`) is handled entirely inside
 * civetweb; no wish code runs on that request path. WebSocket connections
 * are accepted at `/ws`; all callbacks are invoked from civetweb's own
 * worker threads, never the render thread — callers are responsible for
 * their own synchronization (see `web_renderer`'s use of
 * `bison::synchronized<T>`).
 */
class civetweb_server {
 public:
  using on_connect_fn = std::function<void(ws_connection_id)>;
  using on_disconnect_fn = std::function<void(ws_connection_id)>;
  using on_message_fn = std::function<void(ws_connection_id, std::span<const std::byte>)>;

  /**
   * @param bind_addr      Address to bind to (e.g. "127.0.0.1"); empty
   *                        binds all interfaces.
   * @param port            TCP port to listen on; `0` requests an
   *                        OS-assigned ephemeral port (see `actual_port()`).
   * @param document_root  Directory civetweb serves static files from.
   * @param on_connect     Invoked (on a civetweb worker thread) once a new
   *                        WebSocket connection at `/ws` is ready.
   * @param on_disconnect  Invoked (on a civetweb worker thread) when a
   *                        WebSocket connection closes.
   * @param on_message     Invoked (on a civetweb worker thread) for every
   *                        inbound WebSocket binary message.
   */
  civetweb_server(
      std::string bind_addr,
      int port,
      std::filesystem::path document_root,
      on_connect_fn on_connect = nullptr,
      on_disconnect_fn on_disconnect = nullptr,
      on_message_fn on_message = nullptr);
  ~civetweb_server();

  civetweb_server(const civetweb_server&) = delete;
  civetweb_server& operator=(const civetweb_server&) = delete;

  /// @brief Start listening. Throws `std::runtime_error` on bind failure.
  void start();

  /// @brief Stop listening; blocks until civetweb's worker threads exit.
  ///        Safe to call when not started, or more than once.
  void stop();

  /// @brief The actual bound port (resolves ephemeral port `0` after
  ///        `start()`); `0` if not currently started.
  int actual_port() const;

  /// @brief Send @p bytes as one binary WebSocket message to every
  ///        currently-connected client.
  void broadcast(std::span<const std::byte> bytes);

  /// @brief Send @p bytes as one binary WebSocket message to a single
  ///        connection (e.g. the initial texture sync for a new client).
  ///        No-op if @p id is no longer connected.
  void send_to(ws_connection_id id, std::span<const std::byte> bytes);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
