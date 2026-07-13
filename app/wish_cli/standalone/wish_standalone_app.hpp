// MIT License © 2025 Binary Dice Games
/**
 * @file wish_standalone_app.hpp
 * @brief wish CLI standalone mode — server and client fused in one process,
 *        no transport.
 */
#pragma once

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/app/standalone/standalone_app.hpp"

#include <standalone/standalone.hpp>

#include <future>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief Extends wish::standalone with the same lifecycle helpers
 *        `wish_client_app` gives CLI client mode, so the same embedded
 *        app runners (`run_calculator` et al.) work unmodified here.
 *
 * Unlike `wish_client_app`'s session, there is no remote server to disconnect
 * from -- the session ends either when the app itself signals completion
 * (`signal_done()`, typically from a "closed" event handler) or when the
 * user closes the SDL3 window directly (`should_quit()`). `wait_until_done()`
 * blocks on both.
 */
class wish_standalone_session : public standalone, public wish_app_host {
 public:
  wish_standalone_session(std::unique_ptr<renderer> r, std::vector<std::string> app_args);

  /// @brief Store a proxy to keep the local object alive for the session.
  void keep_alive(bison::rmi::proxy::dynamic&& proxy) override;

  /// @brief Unblock wait_until_done() — call from a "closed" event handler.
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

  /// @brief Blocks until either the app signals completion (`signal_done()`)
  ///        or the user closes the render window (`should_quit()`).
  void wait_until_done();

 private:
  std::vector<std::string> app_args_;
  std::promise<void> done_;
  std::future<void> done_future_{done_.get_future()};
  std::vector<bison::rmi::proxy::dynamic> live_proxies_;
};

/**
 * @brief Entry point for `wish standalone [flags]`.
 *
 * Extends `bison::app::standalone_app` so it inherits gflags-based CLI
 * handling.  Only wish-specific behaviour is added here:
 *
 * - `run()` rejects `--transport`/`--host`/`--port`/`--name` (standalone mode
 *   fuses server and client into one process and has no transport to
 *   configure), and handles `--list`/`--describe`/`--run` before delegating
 *   to the base class.
 * - `register_classes()` registers the wish UI class hierarchy.
 * - `make_standalone()` creates a `wish_standalone_session` with the renderer
 *   selected by `--renderer`.
 * - `open_session()`/`close_session()` call `start()`/`stop()` instead of the
 *   base class's `connect()`/`disconnect()`, since `wish_standalone_session`
 *   also owns a render thread that must be started/joined.
 * - `on_session()` looks up the registered app by name, runs it, and blocks
 *   via `wait_until_done()`.
 */
class wish_standalone_app : public bison::app::standalone_app {
 public:
  int run(int argc, char** argv) override;

 protected:
  void register_classes() override;

  std::unique_ptr<bison::rmi::standalone> make_standalone() override;

  void open_session(bison::rmi::standalone& sa) override;
  void close_session(bison::rmi::standalone& sa) override;

  int on_session(bison::rmi::standalone& sa) override;

  void on_error(const std::string& msg) const override;

 private:
  std::string app_name_; // as given on the command line (short or qualified), for messages/logging
  const app_info* resolved_app_ = nullptr; // resolved once in run(), used by on_session()
  std::vector<std::string> app_args_;
};

/// @brief Entry point for `wish standalone [flags]`; equivalent to
///        `wish_standalone_app{}.run(argc, argv)`.
int run_standalone_mode(int argc, char** argv);

} // namespace bdg::wish
