// MIT License © 2025 Binary Dice Games
/// @file wish_standalone_app.cpp
/// @brief wish CLI standalone mode implementation.
#include "app/wish_cli/standalone/wish_standalone_app.hpp"
#include "app/wish_cli/client/app_registry.hpp"

#include <context/logger.hpp>
#include <server/registry.hpp>
#include <sdl/sdl3_renderer.hpp>
#include <web/web_renderer.hpp>

#include <gflags/gflags.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

// ── Shared flags — reused from main.cpp / wish_server_app.cpp / ──────────────
// wish_client_app.cpp so this mode doesn't redefine (and collide with) them.
DECLARE_string(transport);
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_bool(verbose);

DECLARE_string(title);
DECLARE_int32(width);
DECLARE_int32(height);

#ifdef WISH_CLI_BUILD
DECLARE_int32(font_size);
DECLARE_string(renderer);
DECLARE_int32(web_port);
DECLARE_string(web_bind);
#else
DEFINE_int32(font_size, 16, "Font size in pixels");
DEFINE_string(renderer, "web", "Rendering backend: sdl3 or web");
DEFINE_int32(web_port, 8080, "HTTP/WebSocket port for --renderer web");
DEFINE_string(web_bind, "127.0.0.1", "Bind address for --renderer web (localhost-only by default)");
#endif

DECLARE_bool(list);
DECLARE_string(run);
DECLARE_string(describe);

namespace bdg::wish {

using namespace bison;

void wish_standalone_session::keep_alive(rmi::proxy::dynamic&& proxy) {
  live_proxies_.push_back(std::move(proxy));
}

bool wish_standalone_session::read_console_line(std::string& line) {
  return static_cast<bool>(std::getline(std::cin, line));
}

void wish_standalone_session::signal_done() {
  try {
    done_.set_value();
  } catch (const std::future_error&) {
    // Already signalled — ignore.
  }
}

namespace {

static std::unique_ptr<renderer> make_renderer() {
  if (FLAGS_renderer == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    return std::make_unique<sdl3_renderer>(FLAGS_title.c_str(), FLAGS_width, FLAGS_height, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=sdl3 requested but this binary was built with WISH_ENABLE_SDL3=OFF");
#endif
  }
  if (FLAGS_renderer == "web") {
#ifdef WISH_WEB_ENABLED
    return std::make_unique<web_renderer>(FLAGS_web_bind, FLAGS_web_port, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=web requested but this binary was built with WISH_ENABLE_WEB=OFF");
#endif
  }
  throw std::runtime_error("unknown --renderer value '" + FLAGS_renderer + "' (expected sdl3 or web)");
}

std::shared_ptr<logger> make_standalone_logger() {
  return std::make_shared<logger>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishLogger"}),
      FLAGS_verbose,
      std::filesystem::path{"wish_logs"} / "standalone.log");
}

/// @brief Reject flags that only make sense with a real transport — standalone
///        mode fuses server and client into one process and has none.
/// @return true (and prints an error) if a rejected flag was set explicitly.
bool reject_transport_flags() {
  static const char* kTransportFlags[] = {"transport", "host", "port", "name"};
  for (const char* flag : kTransportFlags) {
    if (!gflags::GetCommandLineFlagInfoOrDie(flag).is_default) {
      std::cerr << "[wish standalone] --" << flag
                << " is not supported: standalone mode runs the server and "
                   "client in one process, with no transport.\n";
      return true;
    }
  }
  return false;
}

} // namespace

int run_standalone_mode(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (reject_transport_flags())
    return 1;

  if (FLAGS_list) {
    std::cout << "Available applications:\n";
    for (const auto& [app_name, _] : registered_apps())
      std::cout << "  " << app_name << '\n';
    return 0;
  }

  if (!FLAGS_describe.empty()) {
    const auto& apps = registered_apps();
    auto it = apps.find(FLAGS_describe);
    if (it == apps.end()) {
      std::cerr << "[wish standalone] unknown app '" << FLAGS_describe << "'. Use --list to see available apps.\n";
      return 1;
    }
    describe_app(it->second, std::cout);
    return 0;
  }

  if (FLAGS_run.empty()) {
    std::cerr << "[wish standalone] use --list to see apps or --run=<name> to launch one\n";
    return 1;
  }
  const auto& apps = registered_apps();
  auto it = apps.find(FLAGS_run);
  if (it == apps.end()) {
    std::cerr << "[wish standalone] unknown app '" << FLAGS_run << "'. Use --list to see available apps.\n";
    return 1;
  }

  try {
    // register_all() also runs inside session.start(); calling it here first
    // lets make_standalone_logger() instantiate "__WishLogger" up front
    // (mirrors wish_server_app's register_classes()-before-logger ordering).
    register_all();

#ifndef WISH_SDL3_ENABLED
    // Standalone mode fuses server and client into one interactive process
    // and has always been SDL3-only; unlike `wish server`, it has no
    // --renderer flag / web backend option. wish-cli itself may still be
    // built with WISH_ENABLE_SDL3=OFF (e.g. a web-only deployment), in
    // which case this subcommand isn't available at runtime.
    std::cerr << "[wish standalone] this binary was built with WISH_ENABLE_SDL3=OFF; "
                 "standalone mode requires the SDL3 renderer.\n";
    return 1;
#else
    wish_standalone_session session{make_renderer()};
    session.set_logger(make_standalone_logger());
    session.app_args_.assign(argv + 1, argv + argc);
    session.start();

    if (FLAGS_renderer == "web") {
      std::cout << "[wish] open http://" << FLAGS_web_bind << ':' << FLAGS_web_port << " in a browser\n" << std::flush;
    }

    it->second.run(session); // set up proxies and event handlers

    // Block until either the app signals completion (e.g. a "closed" event
    // handler calling signal_done()) or the user closes the SDL3 window.
    using namespace std::chrono_literals;
    while (!session.should_quit()) {
      if (session.done_future_.wait_for(50ms) == std::future_status::ready)
        break;
    }

    session.stop();
    return 0;
#endif

  } catch (const std::exception& ex) {
    std::cerr << "[wish standalone] error: " << ex.what() << '\n';
    return 1;
  }
}

} // namespace bdg::wish
