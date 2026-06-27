// MIT License © 2025 Binary Dice Games
/**
 * @file wish_bridge_app.cpp
 * @brief wish CLI bridge mode implementation.
 */
#include "app/wish_cli/bridge/wish_bridge_app.hpp"

#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"

#if defined(__linux__) || defined(_WIN32)
#  include "src/rmi/transport/pty_client_transport.hpp"
#  include "src/rmi/transport/pty_server_transport.hpp"
#endif

#include "src/bison/bison.hpp"

#include <gflags/gflags.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

// ── Shared flags (defined in main.cpp) ───────────────────────────────────────
DECLARE_bool  (verbose);

// ── Upstream (client side) flags ─────────────────────────────────────────────
DEFINE_string(up_host, "127.0.0.1", "Upstream server host address");
DEFINE_int32 (up_port, 7070,        "Upstream server port");
DEFINE_string(up_pipe, "",           "Upstream named-pipe / Unix-socket path");
#if defined(__linux__) || defined(_WIN32)
DEFINE_bool  (up_pty,  false,        "Connect upstream via PTY");
#endif

// ── Downstream (server side) flags ───────────────────────────────────────────
DEFINE_string(down_host, "0.0.0.0", "Downstream bind host");
DEFINE_int32 (down_port, 7071,      "Downstream listen port");
DEFINE_string(down_pipe, "",         "Downstream named-pipe / Unix-socket path");

namespace bdg::wish {

using namespace bison;

// ── Desktop chrome ────────────────────────────────────────────────────────────

std::string wish_bridge_app::desktop_title() const {
  return "Bridge Desktop - " + std::to_string(client_count_) +
         (client_count_ == 1 ? " client" : " clients") + " connected";
}

void wish_bridge_app::update_title() {
  if (!desktop_window_.has_value()) return;
  dynamic fields;
  fields["title"_key] = desktop_title();
  try {
    desktop_window_->set(std::move(fields)).get();
  } catch (...) {}
}

void wish_bridge_app::on_client_connected(rmi::context& /*ctx*/) {
  std::lock_guard<std::mutex> lk(desktop_mtx_);
  ++client_count_;

  if (!desktop_window_.has_value()) {
    try {
      auto proxy = upstream().instantiate("wish"_key, "Window"_key).get();
      dynamic fields;
      fields["title"_key]  = desktop_title();
      fields["width"_key]  = int32_t{400};
      fields["height"_key] = int32_t{120};
      proxy.set(std::move(fields)).get();
      desktop_window_ = std::move(proxy);
    } catch (const std::exception& ex) {
      std::cerr << "[bridge] desktop chrome error: " << ex.what() << '\n';
    }
  } else {
    update_title();
  }
}

void wish_bridge_app::on_client_disconnected(rmi::context& /*ctx*/) {
  std::lock_guard<std::mutex> lk(desktop_mtx_);
  if (client_count_ > 0) --client_count_;
  update_title();
}

// ── run ───────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_quit{false};

int wish_bridge_app::run(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // Build upstream (client-side) transport.
  std::unique_ptr<rmi::transport::client_transport_iface> up_transport;
#if defined(__linux__) || defined(_WIN32)
  if (FLAGS_up_pty) {
    up_transport = std::make_unique<rmi::transport::pty_client_transport>();
  } else
#endif
  if (!FLAGS_up_pipe.empty()) {
    up_transport = std::make_unique<rmi::transport::named_pipe_client_transport>(
        FLAGS_up_pipe);
  } else {
    up_transport = std::make_unique<rmi::transport::socket_client_transport>(
        FLAGS_up_host, static_cast<uint16_t>(FLAGS_up_port));
  }

  // Build downstream (server-side) transport.
  std::unique_ptr<rmi::transport::server_transport_iface> down_transport;
  if (!FLAGS_down_pipe.empty()) {
    down_transport = std::make_unique<rmi::transport::named_pipe_server_transport>(
        FLAGS_down_pipe);
  } else {
    down_transport = std::make_unique<rmi::transport::socket_server_transport>(
        FLAGS_down_host, static_cast<uint16_t>(FLAGS_down_port));
  }

  // Install SIGINT handler.
  std::signal(SIGINT, [](int) { g_quit.store(true); });

  try {
    wish_bridge_app bridge{std::move(down_transport), std::move(up_transport)};

    if (!FLAGS_down_pipe.empty()) {
      std::cout << "[bridge] downstream on pipe " << FLAGS_down_pipe << '\n';
    } else {
      std::cout << "[bridge] downstream on "
                << FLAGS_down_host << ':' << FLAGS_down_port << '\n';
    }
#if defined(__linux__) || defined(_WIN32)
    if (FLAGS_up_pty) {
      std::cout << "[bridge] upstream via PTY\n";
    } else
#endif
    if (!FLAGS_up_pipe.empty()) {
      std::cout << "[bridge] upstream via pipe " << FLAGS_up_pipe << '\n';
    } else {
      std::cout << "[bridge] upstream " << FLAGS_up_host
                << ':' << FLAGS_up_port << '\n';
    }

    bridge.start();
    std::cout << "[bridge] running - press Ctrl+C to stop\n" << std::flush;

    while (!g_quit.load())
      std::this_thread::sleep_for(std::chrono::milliseconds{100});

    std::cout << "\n[bridge] stopping...\n" << std::flush;
    bridge.stop();
    return 0;

  } catch (const std::exception& ex) {
    std::cerr << "[bridge] error: " << ex.what() << '\n';
    return 1;
  }
}

} // namespace bdg::wish
