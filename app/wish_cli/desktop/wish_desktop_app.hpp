// MIT License © 2025 Binary Dice Games
/**
 * @file wish_desktop_app.hpp
 * @brief wish CLI desktop mode — multiplexing bridge with a Windows/macOS-like
 *        desktop shell.
 */
#pragma once

#include "app/wish_cli/desktop/desktop_config.hpp"
#include "app/wish_cli/desktop/multi_server_transport.hpp"
#include "app/wish_cli/desktop/process_launcher.hpp"
#include "src/app/bridge/bridge_app.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/bridge/bridge.hpp"
#include "src/rmi/client/proxy.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"

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
  /// Whether the user changed the pins/desktop icons; until then they come
  /// from the configuration file.
  bool saved_pins{false};
  std::vector<std::string> pinned; ///< Entry ids on the taskbar.
  std::vector<std::string> on_desktop; ///< Entry ids with a desktop icon.
  std::string theme; ///< Style preset ("dark"/"light"), or empty.
};

/**
 * @brief Extends rmi::bridge with a Windows/macOS-like desktop shell
 *        rendered on the upstream session: wallpaper, desktop icons and a
 *        taskbar with a Start menu (see the `DesktopShell` class and
 *        `render_desktop_shell()` on the server).
 *
 * The Start menu comes from the desktop configuration file (see
 * desktop_config.hpp and docs/desktop-config.md): a tree of entries, each
 * running a command or a desktop action. The file is watched; when it
 * changes, the shell is rebuilt from it. It is built once, unconditionally,
 * as soon as the upstream connection is up (`build_chrome()`, called by
 * `wish_desktop_app` right after the bridge starts listening).
 *
 * Starting a `run` entry runs its command through the configured
 * `process_launcher_iface`, with the downstream side's WISH_* variables, so
 * a wish client (embedded module or client-ABI app) connects back to this
 * bridge. Each downstream connection reports the app it runs in its connect
 * params (`"app"`, plus optional `"app_label"`/`"app_icon"`), which the
 * desktop records against the connection's session id -- the object group
 * its upstream objects are filed under -- and publishes on the matching
 * entries' `DesktopApp.sessions`, so the server can tell which windows
 * belong to which entry. An app no entry names gets an unlisted
 * `DesktopApp` of its own, so it still shows on the taskbar with its name
 * and icon.
 *
 * The desktop's actions (quit, launch, set_theme, ...) are also served to
 * downstream clients as a `wish/__WishDesktop` object, answered by the
 * bridge itself rather than relayed upstream; clients can start menu
 * entries by id, never arbitrary commands.
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
   * settings from the upstream user store (when the session has one), the
   * configuration file (writing the default one if it is missing), uploads
   * the wallpaper (if any), applies the configured or saved theme, builds
   * the shell and starts watching the configuration file and the
   * wallpaper image.
   */
  void build_chrome();

  /// @brief Program and leading arguments that run a wish client, e.g.
  ///        `{"/usr/bin/wish", "client"}` (the config's `{wish_client}`).
  void set_client_command(std::vector<std::string> command);

  /// @brief Environment variables given to every started command
  ///        (typically WISH_TRANSPORT/WISH_HOST/WISH_PORT/WISH_NAME of the
  ///        downstream side) on top of this process's own.
  void set_launch_env(std::vector<std::pair<std::string, std::string>> env);

  /// @brief Directory receiving each started command's stdout/stderr log.
  void set_log_dir(std::filesystem::path dir);

  /// @brief Non-owning; must outlive this object. Without one, starting a
  ///        `run` entry only logs an error.
  void set_launcher(process_launcher_iface* launcher);

  /// @brief Local image file uploaded as the wallpaper by build_chrome();
  ///        overrides the config file's `appearance.wallpaper`.
  void set_wallpaper(std::filesystem::path file);

  /// @brief User name shown in the Start menu.
  void set_user(std::string user);

  /// @brief The desktop configuration file; must be set before
  ///        build_chrome(). Created with the default content if missing.
  void set_config_file(std::filesystem::path file);

  /// @brief How often build_chrome()'s watcher checks the configuration
  ///        file for changes (default 500 ms). For tests.
  void set_config_poll_interval(std::chrono::milliseconds interval);

  /// @brief Request an orderly shutdown. Safe from any thread. The "quit"
  ///        action; wakes `wait_for_quit()`.
  void request_quit();

  /** @brief Block until `request_quit()` is called. */
  void wait_for_quit();

  /**
   * @brief Block until `request_quit()` is called, or `timeout` elapses.
   * @return `true` if `request_quit()` was called before `timeout`
   *         elapsed; `false` on timeout.
   */
  bool wait_for_quit_for(std::chrono::milliseconds timeout);

  /// @brief Current settings (pins, desktop icons, theme).
  desktop_settings settings() const;

  /// @brief The Start menu in use.
  desktop::start_menu start_menu() const;

  /// @brief The configuration file's current error, or empty.
  std::string config_error() const;

  /// @brief Blocks until every shell event handled so far has been fully
  ///        applied (its upstream updates sent). For tests.
  void flush();

  /// @brief Stops the configuration watcher and the worker thread (pending
  ///        work is dropped). Call before stop(), so no work is cut off
  ///        midway by the upstream connection closing (its requests would
  ///        fail). Idempotent; the destructor calls it too.
  void stop_tasks();

  /**
   * @brief Starts the Start menu entry @p id: runs its command, or its
   *        action.
   * @return false (with a logged error) for an unknown id, a folder or
   *         separator, or a command that could not be started.
   */
  bool launch_entry(const std::string& id);

  /**
   * @brief Runs a desktop action -- the ones `action:` entries and the
   *        `__WishDesktop` RMI object expose (see `desktop::known_actions()`).
   * @return The action's result (`items`: the entries; `launch`: `{ok}`).
   * @throws std::runtime_error for an unknown action or bad arguments.
   */
  bison::dynamic run_action(const std::string& action, const bison::dynamic& args);

  /// @brief Re-reads the configuration file and rebuilds the shell from it;
  ///        an invalid file keeps the current menu and shows the error.
  void reload_config();

 protected:
  /// Records the `"app"` connect param of each downstream connection, and
  /// answers requests for the bridge's own `__WishDesktop` objects locally.
  bool try_handle_request(
      bison::rmi::context& ctx,
      const bison::rmi::shared::envelope& env,
      bison::rmi::transport::server_connection_iface& conn) override;

  void on_client_disconnected(bison::rmi::context& ctx) override;

  /// `wish/__WishDesktop` is served here, not by the class registry.
  bool on_check_class(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) override;
  bison::dynamic_ptr on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) override;

 private:
  struct shared_state {
    desktop_settings settings;
    desktop::start_menu menu;
    desktop::appearance look;
    desktop::taskbar_settings taskbar;
    std::string config_error;
    /// Why the wallpaper can't be shown; empty when it can (or there is none).
    std::string wallpaper_error;
    /// The wallpaper image in use (set_wallpaper() or the config's), for the
    /// file watcher; empty for none.
    std::filesystem::path wallpaper_file;
    /// Downstream session id -> app name it runs.
    std::map<bison::hash_t, std::string> session_apps;
    /// Downstream sessions currently connected.
    std::set<bison::hash_t> sessions;
    /// Entry id -> its DesktopApp node in the current shell.
    std::map<std::string, bison::key_t> entry_nodes;
    /// App name -> its unlisted, top-level DesktopApp (apps no entry names).
    std::map<std::string, bison::key_t> extra_nodes;
    /// App name -> {label, icon} from connect params.
    std::map<std::string, std::pair<std::string, std::string>> app_meta;
    bison::key_t shell_id{0u};
    int templates{0}; ///< Templates registered so far (unique names).
  };

  /// Runs @p task on the worker thread (never on the event thread).
  void post(std::function<void()> task);
  void worker_loop();
  void stop_worker();
  void stop_watcher();

  /// Worker thread: (re)builds the shell from the current menu, replacing
  /// the previous one.
  void build_shell();
  desktop::placeholders placeholders() const;
  void on_shell_event(bison::key_t event, const bison::dynamic& payload);
  void push_running_state();
  void add_unknown_apps();
  void save_settings();
  void apply_theme(const std::string& preset);
  /// Worker thread: uploads the wallpaper image if it, or the file behind
  /// it, changed since the last upload. Returns whether what the shell shows
  /// (its wallpaper or the error) changed.
  bool refresh_wallpaper();
  /// Uploads @p file into the upstream sandbox under a new name (so the
  /// renderer's texture cache can't serve the old image) and erases the
  /// previous upload. Returns its sandbox path. Throws on failure.
  std::string upload_wallpaper(const std::filesystem::path& file);

  std::atomic<bool> chrome_built_{false};

  std::optional<bison::rmi::proxy::dynamic> shell_proxy_;
  std::optional<bison::rmi::proxy::dynamic> template_proxy_;
  std::optional<bison::rmi::proxy::dynamic> user_store_proxy_;
  std::optional<bison::rmi::proxy::dynamic> style_proxy_;

  bison::synchronized<shared_state> state_;

  // Configuration, set before build_chrome().
  std::vector<std::string> client_command_;
  std::vector<std::pair<std::string, std::string>> launch_env_;
  std::filesystem::path log_dir_;
  process_launcher_iface* launcher_{nullptr};
  std::filesystem::path wallpaper_; ///< From set_wallpaper(); wins over the config's.

  // Wallpaper upload state; worker thread only (and build_chrome() before
  // the watcher starts).
  std::string wallpaper_src_; ///< Uploaded image's path in the upstream sandbox.
  std::filesystem::path wallpaper_uploaded_; ///< The local file uploaded.
  /// That file's modification time and size when it was uploaded.
  std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>> wallpaper_stamp_;
  int wallpaper_uploads_{0};

  std::string user_;
  std::filesystem::path config_file_;
  std::chrono::milliseconds config_poll_{500};
  /// The config file's modification time and size when it was loaded.
  std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>> config_stamp_;
  std::atomic<int> launch_count_{0};

  struct worker_state {
    std::deque<std::function<void()>> tasks;
    bool stop{false};
    int busy{0};
  };
  bison::synchronized<worker_state> worker_;
  std::thread worker_thread_;

  bison::synchronized<bool> watcher_stop_{false};
  std::thread watcher_thread_;

  std::mutex quit_mtx_;
  std::condition_variable quit_cv_;
  bool quit_requested_{false};
};

/**
 * @brief Starts listening on the desktop's fixed endpoint @p name (see
 *        `desktop_endpoint()`), which doubles as the single-instance lock.
 *
 * @return The started listener, or `nullptr` if another process already
 *         listens on @p name, i.e. a desktop is already running.
 * @throws std::runtime_error on any other failure to listen.
 */
std::unique_ptr<bison::rmi::transport::named_pipe_server_transport> claim_desktop_endpoint(const std::string& name);

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
 * - `run()` first claims the fixed endpoint (`desktop_endpoint()`); if
 *   another desktop holds it, it reports that and exits with 1 before
 *   connecting anything. `make_bridge()` then serves clients on that
 *   endpoint as well as on the configured downstream transport, through a
 *   `multi_server_transport`.
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
   * It also returns when the upstream connection is lost
   * (`bridge::upstream_connected()`), so a desktop whose server is gone
   * releases its fixed endpoint instead of holding it.
   * Either path lets `bridge_app::run_with_transport()`/`run()` proceed
   * through `br->stop()` and unwind normally, releasing terminal state
   * (`scoped_terminal_config`/`terminal`) via RAII.
   */
  void wait_for_shutdown() override;

 private:
  /** Non-owning: valid for the lifetime of the `wish_desktop` `run_with_transport()` owns. */
  wish_desktop* desktop_{nullptr};

  std::vector<std::string> client_command_;

  /// The fixed endpoint, claimed by run(); moved into downstream_ by
  /// make_bridge(). Null when the downstream transport is the endpoint.
  std::unique_ptr<bison::rmi::transport::named_pipe_server_transport> endpoint_;

  /// The configured downstream transport plus endpoint_; borrowed by the
  /// bridge, so it is declared here to outlive it.
  std::unique_ptr<multi_server_transport> downstream_;

  /// Starts the apps launched from the desktop; its destructor terminates
  /// the ones still running when the desktop exits.
  std::unique_ptr<process_launcher> launcher_;
};

} // namespace bdg::wish
