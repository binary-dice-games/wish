// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_app.hpp
 * @brief wish CLI client mode — connects to a server and runs an embedded app.
 */
#pragma once

#include <client.hpp>

#include <future>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief Extends wish::client with lifecycle helpers for CLI client mode.
 *
 * App runner functions (e.g. run_calculator) call `keep_alive()` to store
 * proxy handles that must remain valid until the session ends, and call
 * `signal_done()` (typically from an event handler) to unblock `on_session()`.
 */
class wish_client_session : public client {
 public:
  using client::client;

  /// @brief Store a proxy to keep the remote object alive for the session.
  void keep_alive(bison::rmi::proxy::dynamic&& proxy);

  /// @brief Unblock on_session() — call from a "closed" event handler.
  void signal_done();

  void on_session() override;
  void on_disconnect() override;

 private:
  std::string app_name_;
  std::promise<void> done_;
  std::future<void> done_future_{done_.get_future()};
  std::vector<bison::rmi::proxy::dynamic> live_proxies_;

  friend int run_client_mode(int argc, char** argv);
};

/// @brief Entry point for `wish client [flags]`.
int run_client_mode(int argc, char** argv);

} // namespace bdg::wish
