// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_app.hpp
 * @brief wish CLI client mode — connects to a server and runs an embedded app.
 */
#pragma once

#include "app/wish_cli/client/wish_app_host.hpp"

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
 *
 * Also implements `wish_app_host` by forwarding to `wish::client`'s own
 * `instantiate`/`upload_file`/`download_file`, so the app runner functions
 * (`run_calculator` et al.) can be written against `wish_app_host&` and work
 * unmodified under both this transport-backed session and the in-process
 * `wish_standalone_session` used by `wish standalone`.
 *
 * CLI flags (defined in `main.cpp`, used here via DECLARE_*), same
 * `--transport` scheme as `wish_server_app` (see
 * `src/app/transport_flags.hpp`):
 *   --transport T  tcp (default), pipe, pty, or console
 *   --host H       Connect host address  (transport=tcp)
 *   --port P       Connect port          (transport=tcp)
 *   --name PATH    Named-pipe / Unix-socket path (transport=pipe)
 *
 * Anything after a literal `--` on the command line is left unparsed by
 * gflags and forwarded verbatim to the app function as `app_args()`, e.g.
 * `wish client --run=notepad -- path/to/file`.
 */
class wish_client_session : public client, public wish_app_host {
 public:
  using client::client;

  /// @brief Store a proxy to keep the remote object alive for the session.
  void keep_alive(bison::rmi::proxy::dynamic&& proxy) override;

  /// @brief Unblock on_session() — call from a "closed" event handler.
  void signal_done() override;

  /// @brief Positional arguments given after `--` on the command line.
  const std::vector<std::string>& app_args() const override {
    return app_args_;
  }

  std::future<bison::rmi::proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{}) override {
    return client::instantiate(ns, klass, std::move(params));
  }

  std::future<void> upload_file(const std::string& name, const std::string& data) override {
    return client::upload_file(name, data);
  }

  std::future<std::string> download_file(const std::string& name) override {
    return client::download_file(name);
  }

  void on_session() override;
  void on_disconnect() override;

 private:
  std::string app_name_;
  std::vector<std::string> app_args_;
  std::promise<void> done_;
  std::future<void> done_future_{done_.get_future()};
  std::vector<bison::rmi::proxy::dynamic> live_proxies_;

  friend int run_client_mode(int argc, char** argv);
};

/// @brief Entry point for `wish client [flags]`.
int run_client_mode(int argc, char** argv);

} // namespace bdg::wish
