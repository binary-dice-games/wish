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
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bdg::wish {

/**
 * @brief Extends rmi::bridge with a desktop shell rendered on the upstream
 *        session: a menu bar (File -> Quit, with a live clock on the right)
 *        over a full-viewport dockable area.
 *
 * The shell is built once, unconditionally, as soon as the upstream
 * connection is up (`build_chrome()`, called by `wish_desktop_app` right
 * after the bridge starts listening) -- it does not wait for a downstream
 * client to connect, since it must be usable even with no clients attached
 * (e.g. to later host client processes spawned by the desktop itself).
 * Downstream clients' own Windows dock into it automatically: ImGui docks
 * any window without `NoDocking` into whichever dockspace is open that
 * frame, so no per-client bookkeeping is needed here.
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
   * `instantiate_template` use), wires the "Quit" menu item to terminate the
   * process, and starts the clock-update thread.
   */
  void build_chrome();

 private:
  void run_clock();

  std::atomic<bool> chrome_built_{false};

  std::optional<bison::rmi::proxy::dynamic> quit_proxy_;
  std::optional<bison::rmi::proxy::dynamic> clock_proxy_;

  std::thread clock_thread_;
  std::mutex clock_mtx_;
  std::condition_variable clock_cv_;
  std::atomic<bool> stop_clock_{false};
};

/**
 * @brief CLI scaffold for `wish desktop` -- multiplexing bridge with a
 *        desktop shell.
 *
 * Extends `bison::app::bridge_app` so it inherits transport selection
 * (downstream `--downstream_transport`/`--downstream_host`/
 * `--downstream_port`/`--downstream_name`/`--cmd`, upstream
 * `--upstream_transport`/`--upstream_host`/`--upstream_port`/
 * `--upstream_name`, shared `--timeout`/`--verbose`/`--debugger`) and the
 * start/stop lifecycle. Only wish-specific behaviour is added here:
 *
 * - `make_bridge()` constructs a `wish_desktop` instead of the generic
 *   internal bridge `bison::app::bridge_app` would otherwise build, and
 *   keeps a non-owning pointer to it so `on_listening()` can trigger chrome
 *   construction.
 * - `on_listening()` calls `wish_desktop::build_chrome()` right after the
 *   bridge starts listening, so the desktop shell exists unconditionally,
 *   not gated on any client connecting.
 * - `bridge_description()` supplies the `OP_HELP` preamble.
 */
class wish_desktop_app : public bison::app::bridge_app {
 protected:
  std::string bridge_description() const override;

  std::unique_ptr<bison::rmi::bridge> make_bridge(
      bison::rmi::transport::server_transport_iface& downstream,
      std::unique_ptr<bison::rmi::transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params) override;

  void on_listening() const override;

 private:
  /** Non-owning: valid for the lifetime of the `wish_desktop` `run_with_transport()` owns. */
  wish_desktop* desktop_{nullptr};
};

} // namespace bdg::wish
