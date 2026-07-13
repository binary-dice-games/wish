// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_app.cpp
 * @brief wish CLI client application implementation.
 */
#include "app/wish_cli/client/wish_client_app.hpp"
#include "src/client/app_registry.hpp"
#include "app/wish_cli/env_flags.hpp"

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
DEFINE_string(theme, "dark", "UI theme preset: dark, light, or classic.");

static bool ValidateTheme(const char* /*flag*/, const std::string& value) {
  return value == "dark" || value == "light" || value == "classic";
}
DEFINE_validator(theme, &ValidateTheme);

namespace bdg::wish {

using namespace bison;

// ── wish_client_app ───────────────────────────────────────────────────────────

int wish_client_app::run(int argc, char** argv) {
  // Override the shared --host default: 0.0.0.0 is a valid bind address for
  // the server but not a connectable address for a client.
  gflags::SetCommandLineOptionWithMode("host", "127.0.0.1", gflags::SET_FLAGS_DEFAULT);
  // WISH_HOST (if set) overrides the 127.0.0.1 default above; WISH_<FLAG>
  // overrides every other registered flag's own default (--transport,
  // --port, --name, --timeout, --theme, ...). An explicit command-line
  // flag always wins over all of these.
  apply_env_flag_defaults();
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_list) {
    std::cout << "Available applications:\n";
    for (const auto& [name, info] : registered_apps())
      std::cout << "  " << qualified_app_name(info) << '\n';
    return 0;
  }

  if (!FLAGS_describe.empty()) {
    auto resolution = resolve_app(FLAGS_describe);
    switch (resolution.status) {
      case app_resolve_status::not_found:
        std::cerr << "[wish client] unknown app '" << FLAGS_describe << "'. Use --list to see available apps.\n";
        return 1;
      case app_resolve_status::ambiguous:
        std::cerr << "[wish client] " << format_ambiguous_error(FLAGS_describe, resolution.candidates) << "\n";
        return 1;
      case app_resolve_status::found:
        describe_app(*resolution.info, std::cout);
        return 0;
    }
  }

  if (FLAGS_run.empty()) {
    std::cerr << "[wish client] use --list to see apps or --run=<name> to launch one\n";
    return 1;
  }
  auto resolution = resolve_app(FLAGS_run);
  switch (resolution.status) {
    case app_resolve_status::not_found:
      std::cerr << "[wish client] unknown app '" << FLAGS_run << "'. Use --list to see available apps.\n";
      return 1;
    case app_resolve_status::ambiguous:
      std::cerr << "[wish client] " << format_ambiguous_error(FLAGS_run, resolution.candidates) << "\n";
      return 1;
    case app_resolve_status::found:
      break;
  }

  // Store app-specific data before delegating to parent's run_with_transport(),
  // which will handle transport creation, connection, and on_session() dispatch.
  app_name_ = FLAGS_run;
  resolved_app_ = resolution.info;
  app_args_.assign(argv + 1, argv + argc);

  // Let parent class handle transport creation and connection lifecycle.
  return bison::app::client_app::run(argc, argv);
}

void wish_client_app::keep_alive(rmi::proxy::dynamic&& proxy) {
  live_proxies_.push_back(std::move(proxy));
}

bool wish_client_app::read_console_line(std::string& line) {
  return client_app::read_console_line(line);
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
  wish_client_ = static_cast<wish::client*>(&c);

  // resolved_app_ is set once in run(), before the transport/connection even
  // exists -- resolving again here would risk a different (ambiguous)
  // outcome if the registry could somehow change mid-process, and duplicates
  // work for no benefit since it can't.
  if (!resolved_app_)
    throw std::runtime_error("unknown app: " + app_name_);

  wish_client_->set_style_preset(FLAGS_theme).get();
  resolved_app_->run(*this); // set up proxies and event handlers
  done_future_.wait(); // block until signal_done() fires
  return 0;
}

std::unique_ptr<bison::rmi::client> wish_client_app::make_client(
    std::unique_ptr<bison::rmi::transport::client_transport_iface> transport) const {
  return std::make_unique<wish::client>(std::move(transport));
}

} // namespace bdg::wish
