// MIT License © 2025 Binary Dice Games
/**
 * @file standalone.hpp
 * @brief wish::standalone — in-process server+client session, no transport.
 */
#pragma once

#include <client/wish_app_host.hpp>
#include <context/context.hpp>
#include <context/logger.hpp>
#include <server/renderer.hpp>
#include "src/rmi/standalone/standalone.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish {

/**
 * @brief Hosts a single wish UI session entirely in-process, on top of
 *        `bison::rmi::standalone` -- no transport, no envelope serialization.
 *
 * Unlike `wish::client`/`wish::server`, which always operate in pairs
 * connected by a `bison::rmi` transport, `standalone` plays *both* roles in
 * one object: it owns the renderer and the session's UI object tree (the
 * "server" role, mirroring `wish::server`), while also exposing the same
 * `instantiate`/wish-level convenience methods a caller would otherwise reach
 * through `wish::client` (the "client" role). There is exactly one session
 * for the lifetime of the object (unlike `wish::server`'s map of one session
 * per connection).
 *
 * ## Usage
 *
 * ```cpp
 * bdg::wish::standalone sa{std::make_unique<my_renderer>(...)};
 * sa.start();
 * auto calc = sa.instantiate("wish"_key, "Bc"_key).get();
 * calc.onEvent("closed"_key, [](bison::dynamic) { ... });
 * // ... render loop runs on its own thread until the window closes ...
 * sa.destroy(std::move(calc));
 * sa.stop();
 * ```
 *
 * @note Event handlers fire on the render thread (the render loop drains
 *       pending widget events and delivers them via `bison::rmi::standalone`'s
 *       in-process `emit_event`).  Do not block that thread from within an
 *       event handler.
 */
class standalone : public bison::rmi::standalone {
 public:
  /// @param r Renderer used by the render loop; must not be null.
  explicit standalone(std::unique_ptr<renderer> r);

  ~standalone();

  standalone(const standalone&) = delete;
  standalone& operator=(const standalone&) = delete;
  standalone(standalone&&) = delete;
  standalone& operator=(standalone&&) = delete;

  /**
   * @brief Register wish classes, start the render thread, then open the
   *        session (fires `on_session_created`).
   */
  void start();

  /** @brief Close the session (fires `on_session_destroyed`) and stop the
   *         render thread. */
  void stop();

  /** @brief Returns true once the renderer signals it should close. */
  bool should_quit() const;

  /**
   * @brief Attach a logger that the session will write through.
   * Must be called before `start()`.
   */
  void set_logger(logger_ptr logger) {
    logger_ = std::move(logger);
  }

  /**
   * @brief Allow widgets to reference files by absolute path.
   *
   * Safe here since there is only one, same-process, session -- see
   * `wish::server::set_allow_absolute_paths()` for the transport-mode caveat
   * this mirrors. Must be called before `start()`.
   */
  void set_allow_absolute_paths(bool allow) {
    allow_absolute_paths_ = allow;
  }

  // ── wish-level convenience helpers (mirror wish::client) ─────────────────
  //
  // instantiate_template() is intentionally not mirrored here: it needs a
  // make_proxy()-style helper to turn a returned object id into a proxy
  // without going through instantiate(), which bison::rmi::standalone does
  // not expose (unlike bison::rmi::client). None of wish's current
  // standalone-mode consumers use templates; add it if/when one does.

  /// @copydoc bdg::wish::client::register_template
  std::future<void> register_template(bison::key_t name, bison::dynamic descriptor);

  /// @copydoc bdg::wish::client::register_template_from_json
  std::future<void> register_template_from_json(bison::key_t name, const std::string& json);

  /// @copydoc bdg::wish::client::register_template_from_yaml
  std::future<void> register_template_from_yaml(bison::key_t name, const std::string& yaml);

  /// @copydoc bdg::wish::client::upload_file
  std::future<void> upload_file(
      const std::string& name, const std::string& data, transfer_progress_callback on_progress = nullptr);

  /// @copydoc bdg::wish::client::download_file
  std::future<std::string> download_file(const std::string& name, transfer_progress_callback on_progress = nullptr);

  /// @copydoc bdg::wish::client::set_style_preset
  std::future<void> set_style_preset(const std::string& name);

  /// @copydoc bdg::wish::client::set_style
  std::future<void> set_style(bison::dynamic params);

  /// @copydoc bdg::wish::client::get_style
  std::future<bison::dynamic> get_style();

  /// @copydoc bdg::wish::client::log
  std::future<void> log(const std::string& level, const std::string& msg);

  /// @copydoc bdg::wish::client::log_debug
  std::future<void> log_debug(const std::string& msg);
  /// @copydoc bdg::wish::client::log_info
  std::future<void> log_info(const std::string& msg);
  /// @copydoc bdg::wish::client::log_warn
  std::future<void> log_warn(const std::string& msg);
  /// @copydoc bdg::wish::client::log_error
  std::future<void> log_error(const std::string& msg);

  // ── Automation helpers (mirror wish::client) ─────────────────────────────

  /// @copydoc bdg::wish::client::automation_supported
  bool automation_supported() const noexcept {
    return automation_proxy_.has_value();
  }

  /// @copydoc bdg::wish::client::get_automation_tree
  std::future<std::string> get_automation_tree(const std::string& root = "");

  /// @copydoc bdg::wish::client::get_automation_logs
  std::future<std::string> get_automation_logs();

  /// @copydoc bdg::wish::client::take_screenshot
  std::future<std::vector<uint8_t>> take_screenshot();

  /// @copydoc bdg::wish::client::inject_mouse_move
  std::future<void> inject_mouse_move(float x, float y);

  /// @copydoc bdg::wish::client::inject_mouse_button
  std::future<void> inject_mouse_button(int button, bool down);

  /// @copydoc bdg::wish::client::inject_key
  std::future<void> inject_key(int keycode, bool down);

  /// @copydoc bdg::wish::client::inject_text
  std::future<void> inject_text(const std::string& utf8);

 protected:
  /** @brief Called once the session's services are ready, before the first
   *         object may be instantiated. */
  virtual void on_session_created(context& s) {
    (void)s;
  }

  /** @brief Called once, just before the session is torn down. */
  virtual void on_session_destroyed(context& s) {
    (void)s;
  }

  /**
   * @brief Factory hook: construct the wish::context for the standalone
   *        session. Mirrors wish::server::on_create_context() (see
   *        src/server/server.hpp) -- override to attach a project-specific
   *        context subclass (e.g. genie::context) instead of a plain
   *        wish::context.
   *
   * Called from on_session_created(bison::rmi::context&) below, before the
   * result is wrapped in context_ and locked -- do not call back into
   * instantiate()/other operations from an override (there is no session to
   * operate on yet).
   *
   * @param session_id The new session's id, forwarded to wish::context's constructor.
   */
  virtual std::unique_ptr<bison::rmi::context> on_create_context(bison::key_t session_id) {
    return std::make_unique<context>(session_id);
  }

  /**
   * @brief Returns the standalone session's synchronized wish::context, once
   *        created (null before the first on_session_created() call).
   *
   * A subclass's on_session_created(context&) override needs this (not just
   * the bare context& the hook receives) to pass to per-session singleton
   * factories that lock session state from a background thread -- mirrors
   * wish::server::session_contexts(), simplified for standalone's single
   * session. Valid from within/after on_session_created(context&).
   */
  sync_context_ptr session_context_ptr() const {
    return context_;
  }

  // Returns session-aware objects for protocol classes (__WishTemplate,
  // __WishFileSystem); falls back to plain instantiate otherwise. Not
  // `final`, and `protected` rather than `private`, so a project embedding
  // wish (e.g. genie) can override it to also special-case its own
  // session-scoped singleton services, calling wish::standalone::on_create_object()
  // for everything it doesn't itself recognize -- mirrors wish::server::on_create_object().
  bison::dynamic_ptr on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) override;

 private:
  // Bridge bison::rmi::standalone's context-level hooks to wish session
  // management -- mirrors wish::server's bridging of bison::rmi::server's
  // hooks (see src/server.cpp), simplified for exactly one session.
  void on_session_created(bison::rmi::context& ctx) override final;
  void on_session_destroyed(bison::rmi::context& ctx) override final;
  void on_before_dispatch(bison::rmi::context& ctx) override final;
  void on_after_dispatch(bison::rmi::context& ctx, bison::key_t op) noexcept override final;

  void render_loop();

  std::unique_ptr<renderer> renderer_;
  // sync_context_ptr (not a direct bison::synchronized<context>): form and
  // ui_template are shared between wish::server and wish::standalone, and
  // their init() takes a sync_context_ptr -- so both hosts must store the
  // context behind the same unique_ptr<bison::rmi::context> indirection,
  // even though standalone itself never needs the polymorphic storage for
  // its own purposes (it only ever holds one, concretely-typed session).
  sync_context_ptr context_;
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

  // Populated by on_session_created(context&); mirrors wish::client::on_connect().
  std::optional<bison::rmi::proxy::dynamic> template_proxy_;
  std::optional<bison::rmi::proxy::dynamic> fs_proxy_;
  std::optional<bison::rmi::proxy::dynamic> style_proxy_;
  std::optional<bison::rmi::proxy::dynamic> log_proxy_;
  // Resolved non-fatally in on_session_created() -- mirrors
  // wish::client::automation_proxy_'s doc comment.
  std::optional<bison::rmi::proxy::dynamic> automation_proxy_;
};

} // namespace bdg::wish
