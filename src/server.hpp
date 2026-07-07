// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief wish::server — RMI server with wish session lifecycle management.
 */
#pragma once

#include <context.hpp>
#include <logger.hpp>
#include <renderer.hpp>
#include "src/rmi/server/server.hpp"

#include <atomic>
#include <chrono>
#include <memory>
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

  /** @brief Register wish classes, start the render loop, then begin accepting
   *         client connections. */
  void start();

  /**
   * @brief Attach a shared logger that all sessions will write through.
   *
   * The logger is set as the `logger_service` of every new session; all
   * sessions therefore share a single log file.  Pass `nullptr` to disable
   * client-side logging.
   * Must be called before `start()`.
   */
  void set_logger(logger_ptr logger) {
    logger_ = std::move(logger);
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

  // Returns session-aware objects for protocol classes (__WishTemplate,
  // __WishFileSystem); falls back to plain instantiate otherwise.
  bison::dynamic_ptr on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) override final;

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
  void on_after_dispatch(bison::rmi::context& ctx) noexcept override;

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
  logger_ptr logger_;
  bool allow_absolute_paths_{false};
};

} // namespace bdg::wish
