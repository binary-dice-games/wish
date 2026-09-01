// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief wish::server — RMI server with wish session lifecycle management.
 */
#pragma once

#include <context/context.hpp>
#include <context/logger.hpp>
#include <server/renderer.hpp>
#include "src/rmi/server/server.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace bdg::wish {

/**
 * @brief Hosts wish UI elements over an RMI transport.
 *
 * Inherits all RMI server behaviour from `bison::rmi::server` and adds:
 * - Automatic wish class registration on `start()`.
 * - Per-client `wish::context` lifecycle (created/destroyed with the
 *   connection), stored directly in the base class's `session_contexts()`.
 * - A renderer-driven frame loop that calls `begin_frame` / `render_node` /
 *   `end_frame` on every connected session at a fixed rate.
 *
 * ## Usage
 *
 * Subclass and override the wish-level hooks:
 * ```cpp
 * class my_server : public wish::server {
 *  protected:
 *   void on_session_created(wish::context& s) override { ... }
 *   void on_session_destroyed(wish::context& s) override { ... }
 * };
 * ```
 *
 * @note Call `stop()` before the server is destroyed so that virtual overrides
 *       of the lifecycle callbacks fire correctly on disconnect.
 */
class server : public bison::rmi::server {
 public:
  /**
   * @brief Construct a wish server that borrows an externally-owned transport.
   * @param transport Transport owned by the caller; must outlive the server.
   * @param r         Renderer used by the frame loop.
   */
  explicit server(bison::rmi::transport::server_transport_iface& transport, std::unique_ptr<renderer> r);

  ~server();

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

  /**
   * @brief Register wish classes, start the render loop, then begin accepting
   *        client connections.
   *
   * @param auth_module   Optional authentication hook (see
   *                     `bison::rmi::auth_module_iface`), evaluated once per
   *                     connection. `nullptr` (default) disables the
   *                     feature: no session ever gets a persistent sandbox
   *                     directory, regardless of `set_persistent_sandbox_root`.
   *                     A parameter rather than a setter, since the module
   *                     cannot sensibly change once the accept loop is
   *                     running -- mirrors `bison::rmi::server::listen()`.
   * @param listen_params Forwarded unchanged to the underlying transport's
   *                     `start()` (via `bison::rmi::server::listen()`) --
   *                     e.g. `cert_file`/`key_file`/`ca_file`/etc. for a
   *                     `tls_socket_server_transport` (see
   *                     `docs/tls.md` in bison). Empty (default) for
   *                     transports that need no extra parameters.
   */
  void start(bison::rmi::auth_module_ptr auth_module = nullptr, bison::dynamic listen_params = bison::dynamic{});

  /**
   * @brief Attach a shared logger that all sessions will write through.
   *
   * The logger is set as the `logger_service` of every new session; all
   * sessions therefore share a single log file.  Pass `nullptr` to disable
   * client-side logging.
   * Must be called before `start()`.
   *
   * The logger's `log_level` also drives the underlying bison trace hooks:
   * RMI trace lines (see `on_print()`) are produced only at `info` or above,
   * and carry decoded call payloads (`args=...`, `set` values, response
   * bodies) only at `trace`.  Below `info` the trace string is never even
   * formatted.
   */
  void set_logger(logger_ptr logger) {
    logger_ = std::move(logger);
    const log_level lvl = logger_ ? logger_->level() : log_level::none;
    set_trace_lines(lvl >= log_level::info);
    set_trace_payloads(lvl >= log_level::trace);
  }

  /**
   * @brief Allow widgets to reference files by absolute path.
   *
   * By default, widget file paths (e.g. `TextEditor::file_path`,
   * `Image::src`) must be relative and are sandboxed inside the session's
   * `resource_dir`.  Call this with `true` only for same-process deployments
   * (memory_transport) where the server and client share the host filesystem.
   *
   * Must be called before `start()`.
   */
  void set_allow_absolute_paths(bool allow) {
    allow_absolute_paths_ = allow;
  }

  /**
   * @brief Allow widgets to fetch `http://`/`https://` resource URLs.
   *
   * By default, widget file paths that look like a URL (e.g. `Image::src`,
   * `Element.font_path`) are rejected outright -- no network request is ever
   * made, matching the fail-closed default of `set_allow_absolute_paths()`.
   * Call this with `true` to let `file_service::resolve_or_fetch()` download
   * such URLs into the session sandbox (still subject to its own
   * extension allowlist).
   *
   * Must be called before `start()`.
   */
  void set_allow_url_fetch(bool allow) {
    allow_url_fetch_ = allow;
  }

  /**
   * @brief Set the default UI theme preset applied to each newly-connected
   *        session's `style_service` before the client does anything.
   *
   * A client that never calls `style_service::preset` (or the CLI client's
   * equivalent `--theme`) keeps whatever this sets; a client that does call
   * it overrides this default for its own session only.
   *
   * Must be called before `start()`.
   *
   * @param name A registered theme name, e.g. `"dark"`, `"light"`,
   *             `"classic"`, or `"wish"` (the default) -- see
   *             `imgui_renderer::register_theme()`.
   */
  void set_default_theme(std::string name) {
    default_theme_ = std::move(name);
  }

  /**
   * @brief Enable persistent, identity-keyed session sandbox directories.
   *
   * When set, and a connection both supplies a non-empty identity (via
   * `start()`'s `auth_module`) and passes authentication, `on_authenticated()`
   * switches that session's `resource_dir` to `root / identity` instead of
   * the default throwaway temp directory, so previously uploaded files
   * persist across reconnects. This is the privacy opt-in: persistent,
   * identity-keyed directories are only ever created when an operator has
   * explicitly configured a root here, mirroring `set_allow_absolute_paths`.
   *
   * Empty (default) disables persistence entirely, regardless of whether an
   * auth module is set or what identity it produces.
   *
   * Must be called before `start()`.
   *
   * @param root Directory under which per-identity subdirectories are
   *             created.
   */
  void set_persistent_sandbox_root(std::filesystem::path root) {
    persistent_sandbox_root_ = std::move(root);
  }

  /** @brief Stop the accept loop, render loop, and join all threads. */
  void stop();

  /** @brief Returns true once the renderer signals it should close.
   *
   *  Useful for a standalone server process that wants to poll for shutdown
   *  without subclassing.  Becomes true when `renderer_->should_quit()` fires
   *  inside the render loop; stays true until `stop()` is called.
   */
  bool should_quit() const;

 protected:
  /** @brief Called on the worker thread after a client connects. */
  virtual void on_session_created(context& s) {
    (void)s;
  }

  /** @brief Called on the worker thread just before a client disconnects. */
  virtual void on_session_destroyed(context& s) {
    (void)s;
  }

 private:
  // Factory hook: construct the wish::context for a new session. Called by
  // bison::rmi::server::client_worker before the context is registered in
  // session_contexts() or locked by anything else.
  std::unique_ptr<bison::rmi::context> on_create_context(bison::key_t session_id) override;

  // Bridge bison's context-level hooks to wish session management.
  // Declared final so subclasses use the wish-level hooks above.
  void on_session_created(bison::rmi::context& ctx) override final;
  void on_session_destroyed(bison::rmi::context& ctx) override final;

  // Switches a newly-authenticated session's resource_dir to a persistent,
  // identity-keyed directory under persistent_sandbox_root_ -- see
  // src/auth/DESIGN.md. No-op if persistent_sandbox_root_ or identity is
  // empty, so a client that supplies no identity (or a deployment with no
  // persistent root configured) sees no behavior change from the default
  // temp directory on_session_created already set up.
  void on_authenticated(bison::rmi::context& ctx, const std::string& identity) override final;

  // Receive formatted trace lines from the base class and forward to logger_.
  void on_print(bison::key_t session_id, const std::string& line) override;

  // Set/clear detail::current_context for the duration of each dispatch. The
  // base class (bison::rmi::server::client_worker) already holds the
  // per-session write lock for the whole dispatch, so these hooks no longer
  // need to acquire or release anything themselves. The render loop holds
  // the per-session read lock for the duration of each render_session call,
  // so dispatch and rendering remain serialised per-session without
  // blocking other sessions.
  void on_before_dispatch(bison::rmi::context& ctx) override;
  void on_after_dispatch(bison::rmi::context& ctx, bison::key_t op) noexcept override;

 protected:
  // Returns session-aware objects for protocol classes (__WishTemplate,
  // __WishFileSystem); falls back to plain instantiate otherwise. Not
  // `final`, and `protected` rather than `private`, so a project embedding
  // wish (e.g. genie) can override it to also special-case its own
  // session-scoped singleton services, calling wish::server::on_create_object()
  // for everything it doesn't itself recognize.
  bison::dynamic_ptr on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) override;

  void render_loop();

  std::unique_ptr<renderer> renderer_;
  std::thread render_thread_;
  std::atomic<bool> running_{false};
  // Render-thread-only timestamp of the last drawn frame; caps render rate
  // independent of vsync (which may be unavailable/ineffective) so bursts
  // of low-level input (e.g. high-poll-rate mouse motion) can't drive the
  // frame rate arbitrarily high.
  std::chrono::steady_clock::time_point last_render_time_{};
  // Render-thread-only latch: set when input activity is observed but the
  // frame-rate cap defers the actual render, so the deferred frame isn't
  // lost if no further input arrives before the cap allows rendering again.
  bool pending_render_{false};
  // Render-thread-only timestamp of the last renderer_->tick() call; caps
  // simulation-tick rate the same way last_render_time_ caps draw rate (see
  // renderer::tick()'s doc comment) -- independent of last_render_time_,
  // since tick() runs every iteration regardless of whether a frame draws.
  std::chrono::steady_clock::time_point last_tick_time_{};
  logger_ptr logger_;
  bool allow_absolute_paths_{false};
  bool allow_url_fetch_{false};
  std::string default_theme_{"wish"};
  // Empty (default) disables persistent sandbox directories entirely; see
  // set_persistent_sandbox_root().
  std::filesystem::path persistent_sandbox_root_;
};

} // namespace bdg::wish
