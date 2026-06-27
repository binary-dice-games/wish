// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.hpp
 * @brief wish GUI server application — integrates wish::server with server_app.
 */
#pragma once

#include "src/app/server/server_app.hpp"

#include <memory>

namespace bdg::wish {

class logger;

/**
 * @brief Server application that opens an SDL3 window and accepts wish
 *        clients over any bison transport.
 *
 * Extends `bison::app::server_app` so it inherits transport selection (socket
 * or PTY on Linux) and gflags-based CLI handling.  Only wish-specific
 * behaviour is added here:
 *
 * - `register_classes()` registers the wish UI class hierarchy.
 * - `run_with_transport()` creates a `wish::server` with an SDL3 renderer
 *   and blocks until the window is closed.
 * - On Linux and Windows, `run_pty()` drives the PTY session loop with the same renderer.
 *
 * CLI flags (defined in `main.cpp`, used here via DECLARE_*):
 *   --title TITLE    Window title  (default: wish)
 *   --width N        Window width  (default: 1280)
 *   --height N       Window height (default: 720)
 *
 * Plus all flags inherited from server_app (--host, --port, --pty, --verbose).
 */
class wish_server_app : public bison::app::server_app {
 public:
  std::string server_description() const override;

  void on_listening() const override;

  /// @brief Route verbose trace lines through the server logger (file + stdout).
  void on_verbose_trace(bison::key_t session_id,
                        const std::string& line) const override;

#if defined(__linux__) || defined(_WIN32)
  void on_listening_pty() const override;
#endif

 protected:
  void register_classes() override;

  int run_with_transport(
      bison::rmi::transport::server_transport_iface& transport) override;

#if defined(__linux__) || defined(_WIN32)
  int run_pty() override;
#endif

 private:
  /// @brief Shared logger for all sessions; always active when the server runs.
  std::shared_ptr<logger> server_log_;
};

} // namespace bdg::wish
