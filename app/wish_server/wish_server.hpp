// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server.hpp
 * @brief wish GUI server application — integrates wish::server with srv_app.
 */
#pragma once

#include "src/app/srv/srv_app.hpp"

namespace bdg::wish {

/**
 * @brief Server application that opens an SDL3 window and accepts wish
 *        clients over any bison transport.
 *
 * Extends `bison::app::srv_app` so it inherits transport selection (socket
 * or PTY on Linux) and gflags-based CLI handling.  Only wish-specific
 * behaviour is added here:
 *
 * - `register_classes()` registers the wish UI class hierarchy.
 * - `run_with_transport()` creates a `wish::server` with an SDL3 renderer
 *   and blocks until the window is closed.
 * - On Linux, `run_pty()` drives the PTY session loop with the same renderer.
 *
 * CLI flags (defined in `main.cpp`, used here via DECLARE_*):
 *   --title TITLE    Window title  (default: wish)
 *   --width N        Window width  (default: 1280)
 *   --height N       Window height (default: 720)
 *
 * Plus all flags inherited from srv_app (--host, --port, --pty, --verbose).
 */
class wish_server : public bison::app::srv_app {
 public:
  std::string server_description() const override;

  void on_listening() const override;

#if defined(__linux__)
  void on_listening_pty() const override;
#endif

 protected:
  void register_classes() override;

  int run_with_transport(
      bison::rmi::transport::server_transport_iface& transport) override;

#if defined(__linux__)
  int run_pty() override;
#endif
};

} // namespace bdg::wish
