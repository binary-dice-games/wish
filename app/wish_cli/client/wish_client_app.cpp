// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_app.cpp
 * @brief wish CLI client application implementation.
 */
#include "app/wish_cli/client/wish_client_app.hpp"
#include "app/wish_cli/client/app_registry.hpp"

#include <gflags/gflags.h>

#include <iostream>
#include <stdexcept>

// ── Shared flags (defined in main.cpp) ────────────────────────────────────────
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);

// ── Client-mode flags ─────────────────────────────────────────────────────────
DEFINE_bool(list, false, "List available embedded applications and exit");
DEFINE_string(run, "", "Name of the embedded application to run");
DEFINE_string(describe, "", "Print name, description, and parameters for a specific embedded application and exit");
DEFINE_int32(timeout, 30000, "Connection timeout in milliseconds");

namespace bdg::wish {

using namespace bison;

// ── wish_client_app ───────────────────────────────────────────────────────────

int wish_client_app::run(int argc, char** argv) {
  // Override the shared --host default: 0.0.0.0 is a valid bind address for
  // the server but not a connectable address for a client.
  gflags::SetCommandLineOptionWithMode("host", "127.0.0.1", gflags::SET_FLAGS_DEFAULT);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_list) {
    std::cout << "Available applications:\n";
    for (const auto& [name, _] : registered_apps())
      std::cout << "  " << name << '\n';
    return 0;
  }

  if (!FLAGS_describe.empty()) {
    const auto& apps = registered_apps();
    auto it = apps.find(FLAGS_describe);
    if (it == apps.end()) {
      std::cerr << "[wish client] unknown app '" << FLAGS_describe << "'. Use --list to see available apps.\n";
      return 1;
    }
    describe_app(it->second, std::cout);
    return 0;
  }

  if (FLAGS_run.empty()) {
    std::cerr << "[wish client] use --list to see apps or --run=<name> to launch one\n";
    return 1;
  }
  if (registered_apps().find(FLAGS_run) == registered_apps().end()) {
    std::cerr << "[wish client] unknown app '" << FLAGS_run << "'. Use --list to see available apps.\n";
    return 1;
  }

  // Store app-specific data before delegating to parent's run_with_transport(),
  // which will handle transport creation, connection, and on_session() dispatch.
  app_name_ = FLAGS_run;
  app_args_.assign(argv + 1, argv + argc);

  // Let parent class handle transport creation and connection lifecycle.
  return bison::app::client_app::run(argc, argv);
}

void wish_client_app::keep_alive(rmi::proxy::dynamic&& proxy) {
  live_proxies_.push_back(std::move(proxy));
}

void wish_client_app::signal_done() {
  try {
    done_.set_value();
  } catch (const std::future_error&) {
    // Already signalled — ignore.
  }
}

std::future<bison::rmi::proxy::dynamic>
wish_client_app::instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params) {
  if (!wish_client_)
    throw std::runtime_error("client not connected");
  return wish_client_->instantiate(ns, klass, std::move(params));
}

std::future<void> wish_client_app::upload_file(const std::string& name, const std::string& data) {
  if (!wish_client_)
    throw std::runtime_error("client not connected");
  return wish_client_->upload_file(name, data);
}

std::future<std::string> wish_client_app::download_file(const std::string& name) {
  if (!wish_client_)
    throw std::runtime_error("client not connected");
  return wish_client_->download_file(name);
}

void wish_client_app::on_connect_params(bison::dynamic& params) const {
  params["timeout_ms"_key] = int32_t{FLAGS_timeout};
}

int wish_client_app::on_session(bison::rmi::client& c) {
  if (!wish_client_)
    throw std::runtime_error("wish_client not initialized");

  const auto& apps = registered_apps();
  auto it = apps.find(app_name_);
  if (it == apps.end())
    throw std::runtime_error("unknown app: " + app_name_);

  try {
    it->second.run(*this); // set up proxies and event handlers
    done_future_.wait(); // block until signal_done() fires
  } catch (...) {
    throw;
  }
  return 0;
}

int wish_client_app::run_with_transport(
    std::unique_ptr<bison::rmi::transport::client_transport_iface> transport) {
  // Create a wish::client with the transport provided by the parent.
  // This allows on_session() to access wish-specific methods.
  wish::client client{std::move(transport)};
  wish_client_ = &client;

  try {
    bison::dynamic params;
    on_connect_params(params);
    client.connect(std::move(params));

    try {
      on_connected();
      int result = on_session(client);
      client.disconnect();
      return result;
    } catch (...) {
      client.disconnect();
      throw;
    }
  } catch (const std::exception& ex) {
    on_error(ex.what());
    return 1;
  }
}

} // namespace bdg::wish
