// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.hpp
 * @brief wish GUI server application — integrates wish::server with server_app.
 */
#pragma once

#include "src/app/server/server_app.hpp"

#include <memory>

namespace bdg::wish {

class logger;

/**
 * @brief Server application that accepts wish clients over any bison
 *        transport and renders their UI via a selectable backend.
 *
 * Extends `bison::app::server_app` so it inherits transport selection
 * and gflags-based CLI handling.  Only wish-specific
 * behaviour is added here:
 *
 * - `register_classes()` registers the wish UI class hierarchy.
 * - `run_with_transport()` creates a `wish::server` with the renderer
 *   selected by `--renderer` and blocks until either the renderer requests a
 *   stop (window close for `sdl3`; `request_quit()` for `web`) or, when
 *   `--transport=term` spawned a terminal, that terminal process exits
 *   (e.g. the operator typed `exit`) -- the latter is how `--renderer web`
 *   is normally stopped, since it has no window to close and nothing
 *   installs a Ctrl+C/SIGINT handler for it.
 *
 * CLI flags (defined in `wish_server_app.cpp`, used here via DECLARE_*
 * where shared with other subcommands):
 *   --renderer NAME  Rendering backend: sdl3 or web (default: sdl3)
 *   --title TITLE    Window title, --renderer sdl3 only  (default: wish)
 *   --width N        Window width, --renderer sdl3 only  (default: 1280)
 *   --height N       Window height, --renderer sdl3 only (default: 720)
 *   --font_size N    Base font size in pixels             (default: 16)
 *   --web_port N     HTTP/WebSocket port, --renderer web only (default: 8080)
 *   --web_bind ADDR  Bind address, --renderer web only (default: 127.0.0.1)
 *
 * Plus all flags inherited from server_app (--transport, --host, --port,
 * --name, --cmd, --verbose). Note `--port` is bison's TCP RMI transport
 * port -- unrelated to `--web_port`, the web renderer's HTTP port.
 *
 * A backend requested via `--renderer` that wasn't compiled in (see
 * `WISH_ENABLE_SDL3`/`WISH_ENABLE_WEB`) makes `run_with_transport()` throw
 * `std::runtime_error` rather than silently falling back to another backend.
 */
class wish_server_app : public bison::app::server_app {
 public:
  std::string server_description() const override;

  void on_listening() const override;

  /// @brief Route verbose trace lines through the server logger (file + stdout).
  void on_verbose_trace(bison::key_t session_id, const std::string& line) const override;

 protected:
  void register_classes() override;

  int run_with_transport(
      bison::rmi::transport::server_transport_iface& transport,
      std::function<void()> wait_for_shutdown = nullptr,
      std::function<bool()> is_shutdown_requested = nullptr) override;

 private:
  /// @brief Shared logger for all sessions; always active when the server runs.
  std::shared_ptr<logger> server_log_;
};

} // namespace bdg::wish
