// MIT License © 2025 Binary Dice Games
/**
 * @file wish_desktop_app.hpp
 * @brief wish CLI desktop mode — multiplexing bridge with a Windows/macOS-like
 *        desktop shell.
 */
#pragma once

#include "app/wish_cli/desktop/process_launcher.hpp"
#include "src/app/bridge/bridge_app.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/bridge/bridge.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bdg::wish {

/// @brief What the desktop remembers between runs (user store entry
///        `wish.desktop`).
struct desktop_settings {
  std::vector<std::string> pinned; ///< Qualified app names on the taskbar.
  std::vector<std::string> on_desktop; ///< Qualified app names with a desktop icon.
  std::string theme; ///< Style preset ("dark"/"light"), or empty.
};

/**
 * @brief Extends rmi::bridge with a Windows/macOS-like desktop shell
 *        rendered on the upstream session: wallpaper, desktop icons and a
 *        taskbar with a Start menu listing every registered app (see the
 *        `DesktopShell` class and `render_desktop_shell()` on the server).
 *
 * The shell is built once, unconditionally, as soon as the upstream
 * connection is up (`build_chrome()`, called by `wish_desktop_app` right
 * after the bridge starts listening) -- it does not wait for a downstream
 * client to connect.
 *
 * Launching an app (Start menu, pinned taskbar button, desktop icon) runs
 * `<client command> --run=<app>` through the configured
 * `process_launcher_iface`; the child connects back to this bridge's
 * downstream side. Each downstream connection reports the app it runs in
 * its connect params (`"app"`), which the desktop records against the
 * connection's session id -- the object group its upstream objects are
 * filed under -- and publishes on the app's `DesktopApp.sessions`, so the
 * server can tell which windows belong to which app.
 *
 * Shell event handlers run on the upstream client's event thread, which
 * must never block on an upstream round trip; their work is handed to a
 * private worker thread instead.
 */
class wish_desktop : public bison::rmi::bridge {
 public:
  using bridge::bridge;

  ~wish_desktop() override;

  /**
   * @brief Build the desktop shell on the upstream session.
   *
   * Idempotent: only the first call has an effect. Loads the saved
   * settings from the upstream user store (when the session has one),
   * uploads the wallpaper (if set), applies the saved theme, then registers
   * and instantiates the `DesktopShell` template via the upstream
   * `__WishTemplate` protocol object and wires its events.
   */
  void build_chrome();

  /// @brief Program and leading arguments that run a wish client, e.g.
  ///        `{"/usr/bin/wish", "client"}`; `--run=<app>` is appended.
  ///        Must be set before an app is launched.
  void set_client_command(std::vector<std::string> command);

  /// @brief Environment variables given to every launched app (typically
  ///        WISH_TRANSPORT/WISH_HOST/WISH_PORT/WISH_NAME of the downstream
  ///        side) on top of this process's own.
  void set_launch_env(std::vector<std::pair<std::string, std::string>> env);

  /// @brief Directory receiving each launched app's stdout/stderr log.
  void set_log_dir(std::filesystem::path dir);

  /// @brief Non-owning; must outlive this object. Without one, launching
  ///        an app only logs an error.
  void set_launcher(process_launcher_iface* launcher);

  /// @brief Local image file uploaded as the wallpaper by build_chrome().
  void set_wallpaper(std::filesystem::path file);

  /// @brief User name shown in the Start menu.
  void set_user(std::string user);

  /// @brief Request an orderly shutdown. Safe from any thread.
  ///
  /// Called by the shell's "quit" event (Start -> Power -> Quit desktop).
  /// Wakes `wait_for_quit()`, causing the identical orderly
  /// `bridge_app::run()` unwind (`br->stop()`, then RAII release of terminal
  /// state) that pressing Enter at the console uses.
  void request_quit();

  /** @brief Block until `request_quit()` is called. */
  void wait_for_quit();

  /**
   * @brief Block until `request_quit()` is called, or `timeout` elapses.
   *
   * @return `true` if `request_quit()` was called before `timeout`
   *         elapsed; `false` on timeout.
   */
  bool wait_for_quit_for(std::chrono::milliseconds timeout);

  /// @brief Current settings (pins, desktop icons, theme).
  desktop_settings settings() const;

  /// @brief Blocks until every shell event handled so far has been fully
  ///        applied (its upstream updates sent). For tests.
  void flush();

  /// @brief Starts @p app (a qualified name) as a downstream client.
  /// @return false (with a logged error) if it could not be started.
  bool launch_app(const std::string& app);

 protected:
  /// Records the `"app"` connect param of each downstream connection.
  bool try_handle_request(
      bison::rmi::context& ctx,
      const bison::rmi::shared::envelope& env,
      bison::rmi::transport::server_connection_iface& conn) override;

  void on_client_disconnected(bison::rmi::context& ctx) override;

 private:
  struct shared_state {
    desktop_settings settings;
    /// Downstream session id -> qualified app name it runs.
    std::map<bison::hash_t, std::string> session_apps;
    /// Downstream sessions currently connected.
    std::set<bison::hash_t> sessions;
    /// Qualified app name -> its DesktopApp node's upstream object id.
    std::map<std::string, bison::key_t> app_nodes;
    bison::key_t shell_id{0u};
    bool loaded_settings{false};
  };

  /// Runs @p task on the worker thread (never on the event thread).
  void post(std::function<void()> task);
  void worker_loop();
  void stop_worker();

  void on_shell_event(bison::key_t event, const bison::dynamic& payload);
  void push_running_state();
  void save_settings();
  void apply_theme(const std::string& preset);

  std::atomic<bool> chrome_built_{false};

  std::optional<bison::rmi::proxy::dynamic> shell_proxy_;
  std::optional<bison::rmi::proxy::dynamic> user_store_proxy_;
  std::optional<bison::rmi::proxy::dynamic> style_proxy_;

  bison::synchronized<shared_state> state_;

  // Configuration, set before build_chrome().
  std::vector<std::string> client_command_;
  std::vector<std::pair<std::string, std::string>> launch_env_;
  std::filesystem::path log_dir_;
  process_launcher_iface* launcher_{nullptr};
  std::filesystem::path wallpaper_;
  std::string user_;
  std::atomic<int> launch_count_{0};

  struct worker_state {
    std::deque<std::function<void()>> tasks;
    bool stop{false};
    int busy{0};
  };
  bison::synchronized<worker_state> worker_;
  std::thread worker_thread_;

  std::mutex quit_mtx_;
  std::condition_variable quit_cv_;
  bool quit_requested_{false};
};

/**
 * @brief CLI scaffold for `wish desktop` -- multiplexing bridge with a
 *        Windows/macOS-like desktop shell.
 *
 * Extends `bison::app::bridge_app` so it inherits transport selection
 * (downstream `--downstream_transport`/`--downstream_host`/
 * `--downstream_port`/`--downstream_name`/`--cmd`, plus its
 * `--downstream_cert_file`/etc. TLS flags; upstream
 * `--upstream_transport`/`--upstream_host`/`--upstream_port`/
 * `--upstream_name`, plus its `--upstream_ca_file`/etc. TLS flags; shared
 * `--timeout`/`--verbose`/`--debugger`) and the start/stop lifecycle -- see
 * `bridge_app`'s own doc comment for the full TLS flag list. Only
 * wish-specific behaviour is added here:
 *
 * - `make_bridge()` constructs a `wish_desktop` instead of the generic
 *   internal bridge `bison::app::bridge_app` would otherwise build,
 *   configures how it launches apps (client command, environment, logs,
 *   wallpaper, user), and keeps a non-owning pointer to it so
 *   `on_listening()` can trigger chrome construction.
 * - `on_upstream_connect_params()` identifies the upstream session
 *   (`--upstream_username`, default: the OS user), so the desktop's
 *   settings persist in that user's store.
 * - `on_listening()` calls `wish_desktop::build_chrome()` right after the
 *   bridge starts listening, so the desktop shell exists unconditionally,
 *   not gated on any client connecting.
 * - `bridge_description()` supplies the `OP_HELP` preamble.
 * - `wait_for_shutdown()` is overridden so the Start menu's "Quit desktop"
 *   can trigger an orderly shutdown alongside console Enter.
 * - Apps launched from the desktop are terminated when it exits.
 */
class wish_desktop_app : public bison::app::bridge_app {
 public:
  /**
   * @brief Applies WISH_<FLAG> environment-variable defaults for every flag
   *        registered in this process (see env_flags.hpp), parses flags,
   *        then primes WISH_TRANSPORT/WISH_HOST/WISH_PORT/WISH_NAME in the
   *        environment from --downstream_transport/--downstream_host/
   *        --downstream_port/--downstream_name before delegating to
   *        bridge_app::run().
   *
   * Those four variables are set in this (parent) process before
   * bridge_app::run() spawns its anchor/downstream terminal, so the spawned
   * shell -- and any `wish client`/`wish server` invoked inside it --
   * inherits them via `environ` and defaults to this desktop's downstream
   * connection with no flags needed (mirrors the env-inheritance mechanism
   * terminal.hpp already relies on for its --prompt-label PS1/
   * PROMPT_COMMAND overrides).
   */
  int run(int argc, char** argv) override;

  /**
   * @brief Program and leading arguments that run a wish client, e.g.
   *        `{"/usr/bin/wish", "client"}` for the combined `wish` binary or
   *        `{"/usr/bin/wish-client"}` beside a standalone `wish-desktop`.
   *        Set by each entry point before run(); `--run=<app>` is appended
   *        for every launch.
   */
  void set_client_command(std::vector<std::string> command) {
    client_command_ = std::move(command);
  }

 protected:
  std::string bridge_description() const override;

  std::string terminal_label() const override {
    return "wish-desktop";
  }

  std::unique_ptr<bison::rmi::bridge> make_bridge(
      bison::rmi::transport::server_transport_iface& downstream_transport,
      std::unique_ptr<bison::rmi::transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params) override;

  void on_listening() const override;

  void on_upstream_connect_params(bison::dynamic& params) const override;

  /**
   * @brief Block until the anchor/downstream terminal exits or the Start
   *        menu's "Quit desktop" fires -- whichever happens first.
   *
   * `bridge_app::run()` now spawns an `active_term_` unconditionally (an
   * anchor terminal, or the downstream terminal itself for
   * `--downstream_transport=term`) and pumps real stdin into it, so a
   * separate `std::getline(std::cin, ...)` thread here would race it for
   * stdin. Instead, this polls `wish_desktop::wait_for_quit_for()`
   * alongside `active_term_->has_exited()`, returning as soon as either
   * the shell's "quit" event handler calls
   * `wish_desktop::request_quit()`, or the operator exits the terminal.
   * Either path lets `bridge_app::run_with_transport()`/`run()` proceed
   * through `br->stop()` and unwind normally, releasing terminal state
   * (`scoped_terminal_config`/`terminal`) via RAII.
   */
  void wait_for_shutdown() override;

 private:
  /** Non-owning: valid for the lifetime of the `wish_desktop` `run_with_transport()` owns. */
  wish_desktop* desktop_{nullptr};

  std::vector<std::string> client_command_;

  /// Starts the apps launched from the desktop; its destructor terminates
  /// the ones still running when the desktop exits.
  std::unique_ptr<process_launcher> launcher_;
};

} // namespace bdg::wish
