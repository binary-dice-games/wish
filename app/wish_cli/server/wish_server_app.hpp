// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.hpp
 * @brief wish GUI server application — integrates wish::server with server_app.
 */
#pragma once

#include "src/app/server/server_app.hpp"

#include <memory>
#include <string>

namespace bdg::wish {

class logger;
class server;

/**
 * @brief A `wish::server` configured from the `wish server` flags
 *        (`--renderer`, `--theme`, `--lang`, `--store_dir`, `--sandbox_root`,
 *        `--settings_admins`, `--profiling_dir`, ...) and started on a
 *        transport. The server is stopped on destruction.
 *
 * Shared by `wish server` and by `wish desktop --with_server`, which runs
 * the server in its own process on a memory transport. The caller must have
 * parsed the flags.
 */
class configured_server {
 public:
  /**
   * @param transport     Transport the server listens on (borrowed; must
   *                      outlive this object).
   * @param listen_params Forwarded to `server::start()`, e.g. TLS material.
   * @throws std::runtime_error for an invalid `--verbose` or a `--renderer`
   *         this binary wasn't built with.
   */
  explicit configured_server(
      bison::rmi::transport::server_transport_iface& transport,
      bison::dynamic listen_params = bison::dynamic{});
  ~configured_server();

  configured_server(const configured_server&) = delete;
  configured_server& operator=(const configured_server&) = delete;

  /// @brief Whether the renderer asked to stop (window closed, or a desktop
  ///        "Shut down server").
  bool should_quit() const;

  /// @brief The server log (`wish_logs/server.log`).
  const std::shared_ptr<logger>& log() const {
    return log_;
  }

  /// @brief Prints how to reach the renderer: the URL to open for
  ///        `--renderer web`, nothing for `sdl3`.
  static void print_renderer_hint();

 private:
  std::shared_ptr<logger> log_;
  std::unique_ptr<server> server_;
};

/**
 * @brief Applies `wish server`'s saved flag defaults (server store entry
 *        `wish.cli.server`, handling `--save_defaults`/`--clear_defaults`;
 *        see config_flags.hpp). Call after parsing the command line.
 * @param log_prefix Message prefix, e.g. `"[wish server]"`.
 */
void load_server_cli_config(const std::string& log_prefix);

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
 *   stop (window close for `sdl3`; `request_quit()` for `web`) or
 *   `is_shutdown_requested()` reports the spawned terminal exited (e.g. the
 *   operator typed `exit`), when `--transport=term` selected it -- the
 *   latter is how `--renderer web` is normally stopped, since it has no
 *   window to close and nothing installs a Ctrl+C/SIGINT handler for it.
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
 * --name, --cmd, --verbose, and the --cert_file/--key_file/--ca_file/etc.
 * TLS flags for --transport=tls -- see `server_app`'s own doc comment for
 * the full list). Note `--port` is bison's TCP RMI transport port --
 * unrelated to `--web_port`, the web renderer's HTTP port.
 *
 * A backend requested via `--renderer` that wasn't compiled in (see
 * `WISH_ENABLE_SDL3`/`WISH_ENABLE_WEB`) makes `run_with_transport()` throw
 * `std::runtime_error` rather than silently falling back to another backend.
 */
class wish_server_app : public bison::app::server_app {
 public:
  /// @brief Resolves every flag as command line > WISH_<FLAG> env var
  ///        (env_flags.hpp) > saved default in the server store
  ///        (config_flags.hpp, handling --save_defaults/--clear_defaults)
  ///        > built-in default, then delegates to server_app::run().
  int run(int argc, char** argv) override;

  std::string server_description() const override;

  std::string terminal_label() const override {
    return "wish-server";
  }

  void on_listening() const override;

  /// @brief Route verbose trace lines through the server logger (file + stdout).
  void on_verbose_trace(bison::key_t session_id, const std::string& line) const override;

 protected:
  void register_classes() override;

  /// @brief Create a wish::server with the renderer selected by --renderer,
  ///        instead of the generic internal server bison::app::server_app
  ///        would otherwise build.
  std::unique_ptr<bison::rmi::server> make_server(bison::rmi::transport::server_transport_iface& transport) override;

  int run_with_transport(bison::rmi::transport::server_transport_iface& transport) override;

 private:
  /// @brief Shared logger for all sessions; always active when the server runs.
  std::shared_ptr<logger> server_log_;
};

} // namespace bdg::wish
