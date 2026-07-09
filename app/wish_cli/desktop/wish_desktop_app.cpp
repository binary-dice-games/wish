// MIT License © 2025 Binary Dice Games
/**
 * @file wish_bridge_app.cpp
 * @brief wish CLI bridge mode implementation.
 */
#include "app/wish_cli/bridge/wish_bridge_app.hpp"

#include "src/bison/bison.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <mutex>
#include <stdexcept>

// ── Shared flags (defined in main.cpp for wish-cli, or bridge/standalone_main.cpp
// for the standalone wish-bridge binary) ─────────────────────────────────────
DECLARE_string(transport);
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_string(cmd);
DECLARE_bool(verbose);
DECLARE_bool(debugger);
// Shared with wish client's --timeout: both express "per-request timeout to
// the remote peer" (there, the wish server; here, the upstream server).
DECLARE_int32(timeout);

// ── Upstream (client side) flags ─────────────────────────────────────────────
DEFINE_string(upstream_transport, "term", "Upstream transport to use: tcp, pipe, or term");
DEFINE_string(upstream_host, "127.0.0.1", "Upstream host address (upstream_transport=tcp)");
DEFINE_int32(upstream_port, 7070, "Upstream port (upstream_transport=tcp)");
DEFINE_string(upstream_name, "", "Upstream named-pipe / Unix-socket path (upstream_transport=pipe)");

namespace bdg::wish {

using namespace bison;

// ── wish_bridge — desktop chrome ───────────────────────────────────────────────

std::string wish_bridge::desktop_title() const {
  return "Bridge Desktop - " + std::to_string(client_count_) + (client_count_ == 1 ? " client" : " clients") +
      " connected";
}

void wish_bridge::update_title() {
  if (!desktop_window_.has_value())
    return;
  dynamic fields;
  fields["title"_key] = desktop_title();
  try {
    desktop_window_->set(std::move(fields)).get();
  } catch (...) {
  }
}

void wish_bridge::on_client_connected(rmi::context& /*ctx*/) {
  std::lock_guard<std::mutex> lk(desktop_mtx_);
  ++client_count_;

  if (!desktop_window_.has_value()) {
    try {
      auto proxy = upstream().instantiate("wish"_key, "Window"_key).get();
      dynamic fields;
      fields["title"_key] = desktop_title();
      fields["width"_key] = int32_t{400};
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

void wish_bridge::on_client_disconnected(rmi::context& /*ctx*/) {
  std::lock_guard<std::mutex> lk(desktop_mtx_);
  if (client_count_ > 0)
    --client_count_;
  update_title();
}

// ── wish_bridge_app ─────────────────────────────────────────────────────────────

std::string wish_bridge_app::bridge_description() const {
  return "wish bridge.\n"
         "Multiplexes downstream clients into one upstream wish server, "
         "with a small desktop window showing the connected client count.";
}

std::unique_ptr<bison::rmi::bridge> wish_bridge_app::make_bridge(
    bison::rmi::transport::server_transport_iface& downstream,
    std::unique_ptr<bison::rmi::transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params) {
  return std::make_unique<wish_bridge>(downstream, std::move(upstream_transport), std::move(upstream_params));
}

} // namespace bdg::wish
