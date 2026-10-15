// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.cpp
 * @brief wish GUI server application implementation.
 */
#include "app/wish_cli/server/wish_server_app.hpp"

#include "app/wish_cli/config_flags.hpp"
#include "app/wish_cli/env_flags.hpp"
#include "app/wish_cli/host_renderer.hpp"

#include <auth/local_auth_module.hpp>
#include <context/logger.hpp>
#include <imgui/imgui_keyboard.hpp>
#include <server/registry.hpp>
#include <sdl/sdl3_renderer.hpp>
#include <server/server.hpp>
#include <web/web_renderer.hpp>

#include "src/app/transport_flags.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"

#include <gflags/gflags.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

DECLARE_string(verbose);
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_string(cmd);
DECLARE_string(theme);
DECLARE_string(lang);

DEFINE_string(title, "wish", "Window title");
DEFINE_int32(width, 1280, "Window width in pixels");
DEFINE_int32(height, 720, "Window height in pixels");
DEFINE_int32(font_size, 16, "Font size in pixels");
DEFINE_string(renderer, "web", "Rendering backend: sdl3 or web (standalone also takes none: no UI)");
// Deliberately NOT named --port: that flag is already bison's TCP RMI
// transport port (see main.cpp), an unrelated concept from the web
// renderer's HTTP/WebSocket port.
DEFINE_int32(web_port, 8080, "HTTP/WebSocket port for --renderer web");
DEFINE_string(web_bind, "127.0.0.1", "Bind address for --renderer web (localhost-only by default)");
DEFINE_string(profiling_dir, "", "Directory for Perfetto trace output; empty disables profiling");
DEFINE_bool(profiling_autostart, false, "Start capture immediately on server startup (requires --profiling_dir)");
DEFINE_bool(allow_absolute_paths, false,
            "Allow widgets (e.g. editor, nano) to reference files by "
            "absolute path outside the session sandbox. Only enable for trusted, "
            "single-operator deployments -- see wish::server::set_allow_absolute_paths().");

DEFINE_string(sandbox_root, "",
              "Directory under which session sandboxes persist across connections: each client gets "
              "<sandbox_root>/<username> (its --username, or 'default') instead of a temp directory "
              "deleted on disconnect. Trusts the client-supplied username -- local/single-user "
              "deployments only; see wish::server::set_persistent_sandbox_root().");
DEFINE_string(
    settings_admins,
    "",
    "Comma-separated identities (--username) allowed to change server-wide settings, such as "
    "the saved default theme, from the settings tool (bdg/desktop/conf). Empty: nobody. "
    "Trusts the client-supplied username -- local/single-user deployments only.");
DEFINE_string(overlay_hotkey, "Shift+F1",
              "Key combination that opens the server overlay (clients, settings, logs, ...), e.g. "
              "Shift+F1 or Ctrl+Shift+O; empty disables the overlay and its startup notification.");
DEFINE_string(screenshot_dir, "wish_screenshots",
              "Directory the server overlay's Screenshot button saves PNG files to (created on demand).");
DEFINE_string(store_dir, "",
              "Directory holding the persistent server store and per-user stores (see "
              "docs/persistent-store.md); empty: ~/.wish. Clients get a user store only when they "
              "connect with --username.");

#ifndef WISH_VERSION_STRING
#define WISH_VERSION_STRING "dev"
#endif

namespace bdg::wish {

namespace {

// ── Helpers ───────────────────────────────────────────────────────────────────

// Flags --save_defaults never saves, on top of the ones config_flag_names()
// always leaves out (--store_dir, secrets, gflags' own): the debug and
// security switches. Every other flag can be saved.
const std::vector<std::string> kServerExcludedFlags = {
    "debugger",
    "profiling_dir",
    "profiling_autostart",
    "allow_absolute_paths",
    "settings_admins", // never from the store: an admin could otherwise make it permanent
};

// The renderer selected by --renderer; @p host (optional) receives its
// overlay face, to enable the server overlay once the server exists.
static std::unique_ptr<renderer> make_renderer(server_overlay_host** host = nullptr) {
  if (FLAGS_renderer == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    auto r = std::make_unique<host_renderer<sdl3_renderer>>(
        FLAGS_title.c_str(), FLAGS_width, FLAGS_height, FLAGS_font_size);
    if (host)
      *host = r.get();
    return r;
#else
    throw std::runtime_error("--renderer=sdl3 requested but this binary was built with WISH_ENABLE_SDL3=OFF");
#endif
  }
  if (FLAGS_renderer == "web") {
#ifdef WISH_WEB_ENABLED
    auto r = std::make_unique<host_renderer<web_renderer>>(FLAGS_web_bind, FLAGS_web_port, FLAGS_font_size);
    if (host)
      *host = r.get();
    return r;
#else
    throw std::runtime_error("--renderer=web requested but this binary was built with WISH_ENABLE_WEB=OFF");
#endif
  }
  throw std::runtime_error("unknown --renderer value '" + FLAGS_renderer + "' (expected sdl3 or web)");
}

// Parse --verbose into a log_level, or abort with a clear message.
static log_level verbose_flag_level() {
  if (auto lvl = parse_log_level(FLAGS_verbose))
    return *lvl;
  throw std::runtime_error(
      "--verbose: invalid level '" + FLAGS_verbose + "' (expected none|fatal|error|warning|info|trace)");
}

static std::shared_ptr<logger> make_server_logger() {
  return std::make_shared<logger>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishLogger"}),
      verbose_flag_level(),
      std::filesystem::path{"wish_logs"} / "server.log");
}

} // namespace

std::string server_endpoint_description() {
  using bison::app::transport_kind;
  switch (bison::app::selected_transport()) {
    case transport_kind::pipe:
      return "pipe " + FLAGS_name;
    case transport_kind::term:
      return "terminal (" + (FLAGS_cmd.empty() ? std::string{"default shell"} : FLAGS_cmd) + ")";
    case transport_kind::tls:
      return "tls " + FLAGS_host + ':' + std::to_string(FLAGS_port);
    case transport_kind::tcp:
    default:
      return "tcp " + FLAGS_host + ':' + std::to_string(FLAGS_port);
  }
}

void load_server_cli_config(const std::string& log_prefix) {
  load_cli_config(FLAGS_store_dir, kServerConfigEntry, kServerExcludedFlags, log_prefix);
}

// ── server_app overrides ──────────────────────────────────────────────────────

int wish_server_app::run(int argc, char** argv) {
  // Parse a copy first: server_app::run() parses argv again, and gflags
  // strips recognized flags in place (see wish_client_app::run()).
  std::vector<char*> parse_argv_storage(argv, argv + argc);
  int parse_argc = argc;
  char** parse_argv = parse_argv_storage.data();

  // Command line > WISH_<FLAG> env var > saved default > built-in default.
  apply_env_flag_defaults();
  gflags::ParseCommandLineFlags(&parse_argc, &parse_argv, true);
  load_server_cli_config("[wish server]");
  return bison::app::server_app::run(argc, argv);
}

std::string wish_server_app::server_description() const {
  return "wish GUI server.\n"
         "Opens an SDL3 window, or (with --renderer web) a browser endpoint,\n"
         "and renders UI pushed by connected clients.";
}

void wish_server_app::register_classes() {
  register_all();
}

void wish_server_app::on_listening() const {
  // "close the window to stop" is SDL3-specific wording; the web renderer
  // has no window to close (Ctrl+C stops the process), and prints where to
  // point a browser instead.
  std::string stop_hint = FLAGS_renderer == "web" ? "Ctrl+C to stop" : "close the window to stop";

  using bison::app::transport_kind;
  switch (bison::app::selected_transport()) {
    case transport_kind::pipe:
      std::cout << "[wish] listening on pipe " << FLAGS_name << " - " << stop_hint << "\n" << std::flush;
      break;
    case transport_kind::term:
      // Exiting the spawned terminal stops the server regardless of
      // --renderer (see run_with_transport()), so it takes priority over
      // stop_hint here.
      std::cout << "[wish] listening via --transport=term (spawned: " << FLAGS_cmd
                << ") - exit the spawned terminal to stop\n"
                << std::flush;
      break;
    case transport_kind::tcp:
      std::cout << "[wish] listening on " << FLAGS_host << ':' << FLAGS_port << " - " << stop_hint << "\n"
                << std::flush;
      break;
  }

  configured_server::print_renderer_hint();
}

void wish_server_app::on_verbose_trace(bison::key_t /*session_id*/, const std::string& line) const {
  if (server_log_)
    server_log_->info(line);
}

std::unique_ptr<bison::rmi::server> wish_server_app::make_server(
    bison::rmi::transport::server_transport_iface& transport) {
  return std::make_unique<server>(transport, make_renderer());
}

int wish_server_app::run_with_transport(bison::rmi::transport::server_transport_iface& transport) {
  // Populates cert_file/key_file/etc. from FLAGS_* when --transport=tls
  // selected it (see bison::app::server_app::on_listen_params()); a no-op
  // for every other transport.
  bison::dynamic listen_params;
  on_listen_params(listen_params);
  {
    configured_server srv{transport, std::move(listen_params), server_endpoint_description()};
    server_log_ = srv.log();
    on_listening();
    // Stops on whichever comes first: the renderer's own close signal (sdl3
    // window close; web has none), or -- when --transport=term spawned a
    // terminal -- that terminal process exiting. This is what lets --renderer
    // web be stopped at all: it has no window and nothing installs a SIGINT
    // handler for this process.
    while (!srv.should_quit() && !is_shutdown_requested())
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  server_log_.reset();
  return 0;
}

// ── configured_server ─────────────────────────────────────────────────────────

configured_server::configured_server(
    bison::rmi::transport::server_transport_iface& transport,
    bison::dynamic listen_params,
    std::string endpoint)
    : log_(make_server_logger()) {
  server_overlay_host* overlay_host = nullptr;
  server_ = std::make_unique<server>(transport, make_renderer(&overlay_host));
  auto& srv = *server_;
  srv.set_logger(log_);
  srv.set_default_theme(FLAGS_theme);
  srv.set_default_language(FLAGS_lang);
  srv.set_allow_absolute_paths(FLAGS_allow_absolute_paths);
  {
    std::set<std::string> admins;
    std::stringstream ss(FLAGS_settings_admins);
    for (std::string item; std::getline(ss, item, ',');) {
      item.erase(0, item.find_first_not_of(" \t"));
      item.erase(item.find_last_not_of(" \t") + 1);
      if (!item.empty())
        admins.insert(item);
    }
    srv.set_settings_admins(std::move(admins));
  }
  if (!FLAGS_profiling_dir.empty())
    srv.enable_profiling(FLAGS_profiling_dir);
  if (!FLAGS_store_dir.empty())
    srv.set_store_dir(FLAGS_store_dir);
  log_->info("persistent stores under " + srv.store_dir().string());
  // Always trust the client-supplied --username as the session identity, so
  // identified clients get a user store; a client without one stays
  // anonymous -- unless --sandbox_root is set, which keeps its 'default'
  // fallback identity so every client gets a persistent sandbox.
  bison::rmi::auth_module_ptr auth;
  if (!FLAGS_sandbox_root.empty()) {
    srv.set_persistent_sandbox_root(FLAGS_sandbox_root);
    auth = std::make_shared<local_auth_module>("default");
    log_->info("persistent sandboxes under " + FLAGS_sandbox_root);
  } else {
    auth = std::make_shared<local_auth_module>();
  }
  if (overlay_host && !FLAGS_overlay_hotkey.empty()) {
    if (parse_key_chord(FLAGS_overlay_hotkey) == 0)
      std::cerr << "[wish] --overlay_hotkey: '" << FLAGS_overlay_hotkey
                << "' is not a key combination; the overlay is disabled\n";
    server_overlay_info info;
    info.version = WISH_VERSION_STRING;
    info.hotkey = FLAGS_overlay_hotkey;
    info.endpoint = std::move(endpoint);
    info.renderer = FLAGS_renderer == "web"
        ? "web (http://" + FLAGS_web_bind + ':' + std::to_string(FLAGS_web_port) + ")"
        : FLAGS_renderer;
    std::error_code ec;
    info.log_file = std::filesystem::absolute(log_->path(), ec);
    if (ec)
      info.log_file = log_->path();
    info.sandbox_root = srv.persistent_sandbox_root();
    info.stores = srv.stores();
    info.settings_admins = srv.settings_admins();
    info.screenshot_dir = std::filesystem::absolute(FLAGS_screenshot_dir, ec);
    if (ec)
      info.screenshot_dir = FLAGS_screenshot_dir;
    if (!FLAGS_profiling_dir.empty()) {
      info.profiler.dir = std::filesystem::absolute(FLAGS_profiling_dir, ec);
      if (ec)
        info.profiler.dir = FLAGS_profiling_dir;
      info.profiler.active = [&srv] { return srv.is_capture_active_now(); };
      info.profiler.start = [&srv] { return srv.start_capture_now("overlay"); };
      info.profiler.stop = [&srv] { srv.stop_capture_now(); };
    }
    overlay_host->enable_server_overlay(std::move(info));
  }
  srv.start(auth, std::move(listen_params));
  if (FLAGS_profiling_autostart) {
    if (FLAGS_profiling_dir.empty())
      log_->info("--profiling_autostart requires --profiling_dir; ignoring");
    else
      srv.start_capture_now("startup");
  }
  log_->info("server started");
}

configured_server::~configured_server() {
  server_->stop_capture_now();
  server_->stop();
}

bool configured_server::should_quit() const {
  return server_->should_quit();
}

void configured_server::print_renderer_hint() {
  if (FLAGS_renderer == "web") {
    std::cout << "[wish] open http://" << FLAGS_web_bind << ':' << FLAGS_web_port << " in a browser\n" << std::flush;
  }
}

} // namespace bdg::wish
