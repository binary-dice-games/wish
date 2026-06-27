// MIT License © 2025 Binary Dice Games
/**
 * @file wish_bridge_app.hpp
 * @brief wish CLI bridge mode — multiplexing bridge with desktop chrome.
 */
#pragma once

#include "src/rmi/bridge/bridge.hpp"
#include "src/rmi/client/proxy.hpp"

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
class wish_bridge_app : public bison::rmi::bridge {
 public:
  using bridge::bridge;

  /// @brief Parse flags, build transports, run bridge, block until SIGINT.
  static int run(int argc, char** argv);

 protected:
  void on_client_connected(bison::rmi::context& ctx) override;
  void on_client_disconnected(bison::rmi::context& ctx) override;

 private:
  std::string desktop_title() const;
  void update_title();

  std::mutex  desktop_mtx_;
  int         client_count_{0};
  std::optional<bison::rmi::proxy::dynamic> desktop_window_;
};

} // namespace bdg::wish
