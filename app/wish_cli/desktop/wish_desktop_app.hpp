// MIT License © 2025 Binary Dice Games
/**
 * @file wish_desktop_app.hpp
 * @brief wish CLI desktop mode — multiplexing bridge with a desktop shell.
 */
#pragma once

#include "src/app/bridge/bridge_app.hpp"
#include "src/rmi/bridge/bridge.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bdg::wish {

/**
 * @brief Extends rmi::bridge with a desktop shell rendered on the upstream
 *        session: a Desktop -> Quit menu spliced into the server's own chrome
 *        menu bar (see `MenuBarExtension`).
 *
 * The shell is built once, unconditionally, as soon as the upstream
 * connection is up (`build_chrome()`, called by `wish_desktop_app` right
 * after the bridge starts listening) -- it does not wait for a downstream
 * client to connect, since it must be usable even with no clients attached
 * (e.g. to later host client processes spawned by the desktop itself).
 * Downstream clients' own Windows dock into it automatically: an
 * un-positioned, dockable top-level `Window` picks up the host chrome's
 * dockspace as its default dock target (see `render_window` and
 * `imgui_renderer::ambient_dockspace_id()`), so no per-client bookkeeping is
 * needed here.
 */
class wish_desktop : public bison::rmi::bridge {
 public:
  using bridge::bridge;

  ~wish_desktop();

  /**
   * @brief Build the menu bar + dockspace shell on the upstream session.
   *
   * Idempotent: only the first call has an effect. Registers and
   * instantiates a UI template via the upstream `__WishTemplate` protocol
   * object (the same protocol `wish::client::register_template`/
   * `instantiate_template` use), and wires the "Quit" menu item to invoke
   * `request_quit()`.
   */
  void build_chrome();

  /**
   * @brief Request an orderly shutdown. Safe from any thread.
   *
   * Called by the "clicked" handler `build_chrome()` wires to the Quit menu
   * item. Wakes `wait_for_quit()`, causing the identical orderly
   * `bridge_app::run()` unwind (`br->stop()`, then RAII release of terminal
   * state) that pressing Enter at the console uses.
   */
  void request_quit();

  /** @brief Block until `request_quit()` is called. */
  void wait_for_quit();

  /**
   * @brief Block until `request_quit()` is called, or `timeout` elapses.
   *
   * Lets a caller poll another shutdown condition (e.g. an anchor
   * terminal exiting) alongside the Quit menu item instead of blocking on
   * `wait_for_quit()` indefinitely.
   *
   * @return `true` if `request_quit()` was called before `timeout`
   *         elapsed; `false` on timeout.
   */
  bool wait_for_quit_for(std::chrono::milliseconds timeout);

 private:
  std::atomic<bool> chrome_built_{false};

  std::optional<bison::rmi::proxy::dynamic> quit_proxy_;

  std::mutex quit_mtx_;
  std::condition_variable quit_cv_;
  bool quit_requested_{false};
};

/**
 * @brief CLI scaffold for `wish desktop` -- multiplexing bridge with a
 *        desktop shell.
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
 *   internal bridge `bison::app::bridge_app` would otherwise build, and
 *   keeps a non-owning pointer to it so `on_listening()` can trigger chrome
 *   construction.
 * - `on_listening()` calls `wish_desktop::build_chrome()` right after the
 *   bridge starts listening, so the desktop shell exists unconditionally,
 *   not gated on any client connecting.
 * - `bridge_description()` supplies the `OP_HELP` preamble.
 * - `wait_for_shutdown()` is overridden so the Quit menu item can trigger
 *   an orderly shutdown alongside console Enter.
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

  /**
   * @brief Block until the anchor/downstream terminal exits or the Quit
   *        menu item fires -- whichever happens first.
   *
   * `bridge_app::run()` now spawns an `active_term_` unconditionally (an
   * anchor terminal, or the downstream terminal itself for
   * `--downstream_transport=term`) and pumps real stdin into it, so a
   * separate `std::getline(std::cin, ...)` thread here would race it for
   * stdin. Instead, this polls `wish_desktop::wait_for_quit_for()`
   * alongside `active_term_->has_exited()`, returning as soon as either
   * the Quit menu item's "clicked" handler calls
   * `wish_desktop::request_quit()`, or the operator exits the terminal.
   * Either path lets `bridge_app::run_with_transport()`/`run()` proceed
   * through `br->stop()` and unwind normally, releasing terminal state
   * (`scoped_terminal_config`/`terminal`) via RAII.
   */
  void wait_for_shutdown() override;

 private:
  /** Non-owning: valid for the lifetime of the `wish_desktop` `run_with_transport()` owns. */
  wish_desktop* desktop_{nullptr};
};

} // namespace bdg::wish
