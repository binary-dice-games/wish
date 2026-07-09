// MIT License © 2025 Binary Dice Games
/**
 * @file wish_standalone_app.hpp
 * @brief wish CLI standalone mode — server and client fused in one process,
 *        no transport.
 */
#pragma once

#include "app/wish_cli/client/wish_app_host.hpp"

#include <standalone/standalone.hpp>

#include <future>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief Extends wish::standalone with the same lifecycle helpers
 *        `wish_client_session` gives CLI client mode, so the same embedded
 *        app runners (`run_calculator` et al.) work unmodified here.
 *
 * Unlike `wish_client_session`, there is no remote server to disconnect
 * from -- the session ends either when the app itself signals completion
 * (`signal_done()`, typically from a "closed" event handler) or when the
 * user closes the SDL3 window directly (`should_quit()`). `run_standalone_mode`
 * blocks on both.
 *
 * `--transport`/`--host`/`--port`/`--name` are not accepted in this mode:
 * there is no transport to configure.
 */
class wish_standalone_session : public standalone, public wish_app_host {
 public:
  using standalone::standalone;

  /// @brief Store a proxy to keep the local object alive for the session.
  void keep_alive(bison::rmi::proxy::dynamic&& proxy) override;

  /// @brief Unblock run_standalone_mode() — call from a "closed" event handler.
  void signal_done() override;

  /// @brief Positional arguments given after `--` on the command line.
  const std::vector<std::string>& app_args() const override {
    return app_args_;
  }

  std::future<bison::rmi::proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{}) override {
    return standalone::instantiate(ns, klass, std::move(params));
  }

  std::future<void> upload_file(const std::string& name, const std::string& data) override {
    return standalone::upload_file(name, data);
  }

  std::future<std::string> download_file(const std::string& name) override {
    return standalone::download_file(name);
  }

  /// @brief Standalone mode has no transport contending for stdin, so this
  ///        is just a direct `std::cin` read.
  bool read_console_line(std::string& line) override;

 private:
  std::vector<std::string> app_args_;
  std::promise<void> done_;
  std::future<void> done_future_{done_.get_future()};
  std::vector<bison::rmi::proxy::dynamic> live_proxies_;

  friend int run_standalone_mode(int argc, char** argv);
};

/// @brief Entry point for `wish standalone [flags]`.
int run_standalone_mode(int argc, char** argv);

} // namespace bdg::wish
