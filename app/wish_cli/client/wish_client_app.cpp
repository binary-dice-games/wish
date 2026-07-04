// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_app.cpp
 * @brief wish CLI client mode implementation.
 */
#include "app/wish_cli/client/wish_client_app.hpp"
#include "app/wish_cli/client/apps/calculator.hpp"
#include "app/wish_cli/client/apps/notepad.hpp"

#include "src/app/transport_flags.hpp"
#include "src/pty/raw_mode_guard.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/stdio_transport.hpp"

#include <gflags/gflags.h>

#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>

// ── Shared flags (defined in main.cpp) ────────────────────────────────────────
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);

// ── Client-mode flags ─────────────────────────────────────────────────────────
DEFINE_bool(list, false, "List available embedded applications and exit");
DEFINE_string(run, "", "Name of the embedded application to run");
DEFINE_int32(timeout, 30000, "Connection timeout in milliseconds");

namespace bdg::wish {

using namespace bison;

// ── App registry ──────────────────────────────────────────────────────────────

using AppFn = std::function<void(wish_client_session&)>;

static const std::map<std::string, AppFn> kApps = {
    {"calculator", run_calculator},
    {"notepad", run_notepad},
};

// ── wish_client_session ───────────────────────────────────────────────────────

void wish_client_session::keep_alive(rmi::proxy::dynamic&& proxy) {
  live_proxies_.push_back(std::move(proxy));
}

void wish_client_session::signal_done() {
  try {
    done_.set_value();
  } catch (const std::future_error&) {
    // Already signalled — ignore.
  }
}

void wish_client_session::on_disconnect() {
  signal_done();
}

void wish_client_session::on_session() {
  auto it = kApps.find(app_name_);
  if (it == kApps.end())
    throw std::runtime_error("unknown app: " + app_name_);

  it->second(*this); // set up proxies and event handlers
  done_future_.wait(); // block until signal_done() fires
}

// ── run_client_mode ───────────────────────────────────────────────────────────

int run_client_mode(int argc, char** argv) {
  // Override the shared --host default: 0.0.0.0 is a valid bind address for
  // the server but not a connectable address for a client.
  gflags::SetCommandLineOptionWithMode("host", "127.0.0.1", gflags::SET_FLAGS_DEFAULT);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_list) {
    std::cout << "Available applications:\n";
    for (const auto& [name, _] : kApps)
      std::cout << "  " << name << '\n';
    return 0;
  }

  if (FLAGS_run.empty()) {
    std::cerr << "[wish client] use --list to see apps or --run=<name> to launch one\n";
    return 1;
  }
  if (kApps.find(FLAGS_run) == kApps.end()) {
    std::cerr << "[wish client] unknown app '" << FLAGS_run << "'. Use --list to see available apps.\n";
    return 1;
  }

  try {
    std::unique_ptr<rmi::transport::client_transport_iface> transport;
    // Only constructed for --transport=pty; kept alive for the session's
    // duration (restores the fd's original terminal mode on scope exit).
    std::optional<pty::raw_mode_guard> raw_mode;

    switch (bison::app::selected_transport()) {
      case bison::app::transport_kind::pipe:
        transport = std::make_unique<rmi::transport::named_pipe_client_transport>(FLAGS_name);
        break;
      case bison::app::transport_kind::pty:
        // See raw_mode_guard's doc comment: without this, a pty slave's
        // cooked-mode line buffering stalls the BISON<...> framing read from
        // fd 0. wish's client apps never read console input themselves, so
        // unlike bison::app::client_app there's no passthrough/echo to wire up.
        raw_mode.emplace(0);
        transport = std::make_unique<rmi::transport::stdio_client_transport>(0, 1);
        break;
      case bison::app::transport_kind::console:
        transport = std::make_unique<rmi::transport::stdio_client_transport>(0, 1);
        break;
      case bison::app::transport_kind::tcp:
        transport =
            std::make_unique<rmi::transport::socket_client_transport>(FLAGS_host, static_cast<uint16_t>(FLAGS_port));
        break;
    }

    wish_client_session session{std::move(transport)};
    session.app_name_ = FLAGS_run;

    bison::dynamic params;
    params["timeout_ms"_key] = int32_t{FLAGS_timeout};
    session.connect(std::move(params));

    try {
      session.on_session();
    } catch (...) {
      session.disconnect();
      throw;
    }
    session.disconnect();
    return 0;

  } catch (const std::exception& ex) {
    std::cerr << "[wish client] error: " << ex.what() << '\n';
    return 1;
  }
}

} // namespace bdg::wish
