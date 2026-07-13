// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_app.hpp
 * @brief wish CLI client application — connects to a server and runs an
 *        embedded app.
 */
#pragma once

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/app/client/client_app.hpp"
#include "src/client/client.hpp"

#include <future>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief Client application that connects to a wish server and runs an
 *        embedded application module.
 *
 * Extends `bison::app::client_app` so it inherits transport selection
 * and gflags-based CLI handling. Only wish-specific behaviour is added here:
 *
 * - `on_session(c)` looks up the registered app by name and runs it,
 *   implements `wish_app_host` so app runners can instantiate remote objects
 *   and stay connected until the app signals completion.
 * - `on_connect_params()` populates the connection timeout from `FLAGS_timeout`.
 * - `make_client()` constructs a `wish::client` instead of the generic
 *   `bison::rmi::client`, so `on_session()` can access wish-specific methods
 *   like `upload_file()`/`download_file()`.
 *
 * Session-runner functions (e.g. run_calculator) call `keep_alive()` to store
 * proxy handles that must remain valid until the session ends, and call
 * `signal_done()` (typically from an event handler) to unblock `on_session()`.
 *
 * CLI flags (consumed via gflags DECLARE_*), same `--transport` scheme as
 * `wish_server_app`:
 *   --transport T  tcp (default), pipe, pty, or console
 *   --host H       Connect host address  (transport=tcp)
 *   --port P       Connect port          (transport=tcp)
 *   --name PATH    Named-pipe / Unix-socket path (transport=pipe)
 *
 * Plus wish-specific flags:
 *   --list                List available embedded apps and exit
 *   --run=<name>          Launch the named app (required unless --list)
 *   --describe=<name>     Print app description and exit
 *   --timeout MS          Connection timeout in milliseconds (default: 30000)
 *
 * Anything after a literal `--` on the command line is forwarded to the app
 * via `app_args()`, e.g. `wish client --run=notepad -- path/to/file`.
 */
class wish_client_app : public bison::app::client_app, public wish_app_host {
 public:
  wish_client_app() = default;

  /// @brief Entry point — parse flags and run the client application.
  /// 
  /// Handles --list and --describe flags which exit early, and requires
  /// --run=<name> for normal operation.
  int run(int argc, char** argv) override;

  /// @brief Store a proxy to keep the remote object alive for the session.
  void keep_alive(bison::rmi::proxy::dynamic&& proxy) override;

  /// @brief Unblock on_session() — call from a "closed" event handler.
  void signal_done() override;

  /// @brief Positional arguments given after `--` on the command line.
  const std::vector<std::string>& app_args() const override {
    return app_args_;
  }

  /// @brief Forwards to the inherited (protected) `client_app::read_console_line()`.
  bool read_console_line(std::string& line) override;

  std::future<bison::rmi::proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{}) override;

  std::future<void> upload_file(const std::string& name, const std::string& data) override;

  std::future<std::string> download_file(const std::string& name) override;

 protected:
  void on_connect_params(bison::dynamic& params) const override;

  int on_session(bison::rmi::client& c) override;

  /// @brief Create a wish::client instead of a generic bison::rmi::client,
  ///        so on_session() can access wish-specific methods like
  ///        upload_file() and download_file() via a static_cast.
  std::unique_ptr<bison::rmi::client> make_client(
      std::unique_ptr<bison::rmi::transport::client_transport_iface> transport) const override;

 private:
  std::string app_name_; // as given on the command line (short or qualified), for messages/logging
  const app_info* resolved_app_ = nullptr; // resolved once in run(), used by on_session()
  std::vector<std::string> app_args_;
  std::promise<void> done_;
  std::future<void> done_future_{done_.get_future()};
  std::vector<bison::rmi::proxy::dynamic> live_proxies_;
  wish::client* wish_client_ = nullptr;
};

} // namespace bdg::wish
