// MIT License © 2025 Binary Dice Games
/**
 * @file wish_bridge_app.hpp
 * @brief wish CLI bridge mode — multiplexing bridge with desktop chrome.
 */
#pragma once

#include "src/app/bridge/bridge_app.hpp"
#include "src/rmi/bridge/bridge.hpp"
#include "src/rmi/client/proxy.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bdg::wish {

/**
 * @brief Extends rmi::bridge with a minimal desktop Window on the upstream
 *        session that shows the number of currently connected clients.
 *
 * On the first downstream client connect a Window is instantiated on the
 * upstream server.  Its title is updated on every subsequent connect and
 * disconnect.  The Window is destroyed when the bridge stops.
 */
class wish_bridge : public bison::rmi::bridge {
 public:
  using bridge::bridge;

 protected:
  void on_client_connected(bison::rmi::context& ctx) override;
  void on_client_disconnected(bison::rmi::context& ctx) override;

 private:
  std::string desktop_title() const;
  void update_title();

  std::mutex desktop_mtx_;
  int client_count_{0};
  std::optional<bison::rmi::proxy::dynamic> desktop_window_;
};

/**
 * @brief CLI scaffold for `wish bridge` -- multiplexing bridge with desktop
 *        chrome.
 *
 * Extends `bison::app::bridge_app` so it inherits transport selection
 * (downstream `--transport`/`--host`/`--port`/`--name`/`--cmd`, upstream
 * `--upstream_transport`/`--upstream_host`/`--upstream_port`/
 * `--upstream_name`, shared `--timeout`/`--verbose`/`--debugger`) and the
 * start/stop lifecycle. Only wish-specific behaviour is added here:
 *
 * - `make_bridge()` constructs a `wish_bridge` instead of the generic
 *   internal bridge `bison::app::bridge_app` would otherwise build, so the
 *   desktop-chrome hooks (`on_client_connected`/`on_client_disconnected`,
 *   which call `upstream()` directly) run.
 * - `bridge_description()` supplies the `OP_HELP` preamble.
 */
class wish_bridge_app : public bison::app::bridge_app {
 protected:
  std::string bridge_description() const override;

  std::unique_ptr<bison::rmi::bridge> make_bridge(
      bison::rmi::transport::server_transport_iface& downstream,
      std::unique_ptr<bison::rmi::transport::client_transport_iface> upstream_transport,
      bison::dynamic upstream_params) override;
};

} // namespace bdg::wish
