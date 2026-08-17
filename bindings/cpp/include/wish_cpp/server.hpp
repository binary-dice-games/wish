// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief RAII wrapper around `wish_server_handle` -- hosts and renders a
 *        real wish session, built entirely on `wish_server_c.h` (plus
 *        `bison_c.h`) and the prebuilt `wish_server_dll` shared library.
 *
 * A *separate* binding target from `wish_cpp` (`client.hpp`): `wish_server_dll`
 * is a distinct shared library from `wish_client_dll` (see
 * `include/wish_server_c.h`'s file doc comment) that embeds its own copy of
 * the `bison_c.h`/`rmi_c.h` C ABI. Do not link both `wish_client_dll` and
 * `wish_server_dll` into the same binary that also uses `value` (the
 * `bison_handle` wrapper shared with `client.hpp`) -- both libraries export
 * identically-named `bison_*` symbols, and which one a `bison_handle` call
 * actually resolves against becomes link-order-dependent. See
 * `bindings/python/wish/_server_native.py`'s module doc comment for the
 * identical hazard (and fix) in the Python binding.
 */
#pragma once

#include "error.hpp"
#include "value.hpp"

#include <wish_server_c.h>

#include <cstdint>
#include <string>

namespace bdg::wish::binding {

namespace detail {

inline const char* wish_server_error_message(wish_server_error rc) {
  switch (rc) {
    case WISH_SERVER_ERR_NULL:
      return "null handle or pointer";
    case WISH_SERVER_ERR_TRANSPORT:
      return "transport listen failed";
    case WISH_SERVER_ERR_EXCEPTION:
      return "internal C++ exception";
    case WISH_SERVER_ERR_BAD_RENDERER:
      return "unknown renderer_kind, or this library wasn't built with support for it";
    default:
      return "unknown error";
  }
}

// Appends server-specific detail (wish_server_last_error) when a server
// handle is available, mirroring error.hpp's throw_if_wish_error().
inline void throw_if_wish_server_error(
    wish_server_error rc, const std::string& context, wish_server_handle server = nullptr) {
  if (rc == WISH_SERVER_OK) return;
  std::string msg = context + ": " + wish_server_error_message(rc);
  if (server) {
    const char* detail = wish_server_last_error(server);
    if (detail && *detail) msg += " (" + std::string(detail) + ")";
  }
  throw error(rc, msg);
}

}  // namespace detail

/**
 * @brief RAII wrapper around a `wish_server_handle`.
 *
 * Construct via `tcp()`, `pipe()`, `tls()`, or `term()`, then `start()` to
 * build the requested renderer and begin accepting client connections.
 */
class server {
 public:
  server(const server&) = delete;
  server& operator=(const server&) = delete;

  server(server&& other) noexcept : h_(other.h_), started_(other.started_) { other.h_ = nullptr; }
  server& operator=(server&& other) noexcept {
    if (this != &other) {
      destroy();
      h_ = other.h_;
      started_ = other.started_;
      other.h_ = nullptr;
    }
    return *this;
  }

  ~server() { destroy(); }

  static server tcp(const std::string& host, uint16_t port) {
    wish_server_handle h = wish_server_tcp_create(host.c_str(), port);
    if (!h) throw error(WISH_SERVER_ERR_EXCEPTION, "server::tcp: wish_server_tcp_create failed");
    return server(h);
  }

  static server pipe(const std::string& path) {
    wish_server_handle h = wish_server_pipe_create(path.c_str());
    if (!h) throw error(WISH_SERVER_ERR_EXCEPTION, "server::pipe: wish_server_pipe_create failed");
    return server(h);
  }

  /**
   * @brief Create a TLS-secured TCP socket server (not yet listening).
   *
   * TLS material (`cert_file`/`cert_pem`, `key_file`/`key_pem`,
   * `key_password`, and optionally `client_auth`/`ca_file`/`ca_pem` for
   * mutual TLS) is supplied via `start()`'s `params`.
   */
  static server tls(const std::string& host, uint16_t port) {
    wish_server_handle h = wish_server_tls_create(host.c_str(), port);
    if (!h) throw error(WISH_SERVER_ERR_EXCEPTION, "server::tls: wish_server_tls_create failed");
    return server(h);
  }

  /**
   * @brief Create a terminal (OSC-99 framed) server by spawning a child
   *        process attached to a new pseudo-terminal.
   *
   * The spawned child is expected to be a wish client process using
   * `client::term()` (or an equivalent term-transport client) over its own
   * inherited stdio. `should_quit()` also returns `true` once the spawned
   * child exits, in addition to any renderer close signal.
   *
   * @param cmd Command to exec in the child. Empty (default) spawns the
   *            operator's `$SHELL`/`cmd.exe`.
   */
  static server term(const std::string& cmd = {}) {
    wish_server_handle h = wish_server_term_create(cmd.empty() ? nullptr : cmd.c_str());
    if (!h) throw error(WISH_SERVER_ERR_EXCEPTION, "server::term: wish_server_term_create failed");
    return server(h);
  }

  /** @brief Last error message recorded for this server (empty if none). */
  std::string last_error() const {
    const char* msg = wish_server_last_error(h_);
    return msg ? msg : "";
  }

  /** @brief Enable/disable verbose trace logging of RMI dispatch to stdout. Must be called before `start()`. */
  void set_verbose(bool verbose = true) {
    detail::throw_if_wish_server_error(wish_server_set_verbose(h_, verbose ? 1 : 0), "server::set_verbose", h_);
  }

  /**
   * @brief Build the requested renderer and begin accepting client
   *        connections.
   * @param renderer_kind "sdl3", "web", or "console".
   * @param params        Renderer-specific fields (see `wish_server_start()`'s
   *                      doc comment in `wish_server_c.h`); also forwarded
   *                      unchanged as transport listen params -- e.g. TLS
   *                      material for a `tls()` server.
   */
  void start(const std::string& renderer_kind, const value& params = value{}) {
    detail::throw_if_wish_server_error(
        wish_server_start(h_, renderer_kind.c_str(), params.handle()), "server::start(" + renderer_kind + ")", h_);
    started_ = true;
  }

  /** @brief Stop the accept loop, render loop, and join all threads. */
  void stop() {
    if (!started_) return;
    detail::throw_if_wish_server_error(wish_server_stop(h_), "server::stop", h_);
    started_ = false;
  }

  /**
   * @brief Returns true once the renderer signals it should close (e.g. an
   *        SDL3 window close), or -- for a `term()` server -- once the
   *        spawned child process has exited. The web/console renderers
   *        never set this on their own; call `stop()` explicitly for those.
   */
  bool should_quit() const { return wish_server_should_quit(h_) != 0; }

  wish_server_handle handle() const noexcept { return h_; }

 private:
  explicit server(wish_server_handle h) : h_(h) {}

  void destroy() {
    if (h_) wish_server_destroy(h_);
    h_ = nullptr;
  }

  wish_server_handle h_;
  bool started_ = false;
};

}  // namespace bdg::wish::binding
