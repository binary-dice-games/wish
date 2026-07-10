// MIT License © 2025 Binary Dice Games
/**
 * @file wish_desktop_app.cpp
 * @brief wish CLI desktop mode implementation.
 */
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include "src/bison/bison.hpp"
#include "src/term/terminal.hpp"
#include "src/ui/ui_descriptor.hpp"

#include <gflags/gflags.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <mutex>
#include <stdexcept>

// ── Shared flags (defined in main.cpp for wish-cli, or desktop/standalone_main.cpp
// for the standalone wish-desktop binary) ────────────────────────────────────
DECLARE_string(cmd);
DECLARE_bool(verbose);
DECLARE_bool(debugger);
// Shared with wish client's --timeout: both express "per-request timeout to
// the remote peer" (there, the wish server; here, the upstream server).
DECLARE_int32(timeout);

// ── Downstream (server side) flags ─────────────────────────────────────────────
DEFINE_string(downstream_transport, "tcp", "Downstream transport to use: tcp, pipe, or term");
DEFINE_string(downstream_host, "0.0.0.0", "Downstream bind host address (downstream_transport=tcp)");
DEFINE_int32(downstream_port, 7071, "Downstream listen port (downstream_transport=tcp)");
DEFINE_string(downstream_name, "", "Downstream named-pipe / Unix-socket path (downstream_transport=pipe)");

// ── Upstream (client side) flags ─────────────────────────────────────────────
DEFINE_string(upstream_transport, "term", "Upstream transport to use: tcp, pipe, or term");
DEFINE_string(upstream_host, "127.0.0.1", "Upstream host address (upstream_transport=tcp)");
DEFINE_int32(upstream_port, 7070, "Upstream port (upstream_transport=tcp)");
DEFINE_string(upstream_name, "", "Upstream named-pipe / Unix-socket path (upstream_transport=pipe)");

namespace bdg::wish {

using namespace bison;

namespace {

// Template name used to register/instantiate the desktop shell; namespaced
// with a leading "__" like wish's other internal protocol identifiers to
// avoid colliding with a downstream client's own template names.
constexpr const char* kChromeTemplateName = "__wish_desktop_chrome";

// Dot-paths (relative to the template root, see wish::client::proxy_map)
// of the two nodes wish_desktop needs a handle to after instantiation.
constexpr const char* kQuitPath = "main_menu.m_file.mi_quit";
constexpr const char* kClockPath = "main_menu.clock";

// Full-viewport dockspace host with a menu bar (File -> Quit) and a clock
// label docked to the right of the menu bar. Downstream clients' own Windows
// dock into the DockSpaceViewport automatically -- no reference to them is
// needed here.
constexpr const char* kChromeDescriptorJson = R"json({
  "type": "DockSpaceViewport", "id": "wish_desktop_dockspace", "passthru": true,
  "children": {
    "main_menu": { "type": "MenuBar",
      "children": {
        "m_file": { "type": "Menu", "label": "File",
          "children": {
            "mi_quit": { "type": "MenuItem", "label": "Quit" }
          }
        },
        "clock": { "type": "Label", "text": "" }
      }
    }
  }
})json";

} // namespace

// ── wish_desktop — desktop shell ────────────────────────────────────────────

wish_desktop::~wish_desktop() {
  stop_clock_.store(true);
  clock_cv_.notify_all();
  if (clock_thread_.joinable())
    clock_thread_.join();
}

void wish_desktop::build_chrome() {
  if (chrome_built_.exchange(true))
    return;

  try {
    auto tmpl = upstream().instantiate("wish"_key, "__WishTemplate"_key).get();

    dynamic reg_args;
    reg_args["name"_key] = key_t{kChromeTemplateName};
    reg_args["descriptor"_key] = dynamic_ptr{import_descriptor_json(kChromeDescriptorJson)};
    tmpl.call("register"_key, std::move(reg_args)).get();

    dynamic inst_args;
    inst_args["name"_key] = key_t{kChromeTemplateName};
    auto result = tmpl.call("instantiate"_key, std::move(inst_args)).get();

    key_t quit_id{0u};
    key_t clock_id{0u};
    result.forEach([&](key_t, const field& v) {
      if (!v.is<dynamic_ptr>())
        return;
      const auto& entry = v.as<dynamic_ptr>();
      if (!entry)
        return;
      const auto* name_field = entry->findField("name"_key);
      const auto* id_field = entry->findField("id"_key);
      if (!name_field || !id_field || !name_field->is<std::string>() || !id_field->is<key_t>())
        return;
      const std::string& path = name_field->as<std::string>();
      if (path == kQuitPath)
        quit_id = id_field->as<key_t>();
      else if (path == kClockPath)
        clock_id = id_field->as<key_t>();
    });

    if (quit_id.id != 0u) {
      quit_proxy_ = upstream().make_proxy(quit_id);
      quit_proxy_->onEvent("clicked"_key, [this](dynamic) { request_quit(); });
    }

    if (clock_id.id != 0u) {
      clock_proxy_ = upstream().make_proxy(clock_id);
      clock_thread_ = std::thread([this] { run_clock(); });
    }
  } catch (const std::exception& ex) {
    std::cerr << "[desktop] chrome build error: " << ex.what() << '\n';
  }
}

void wish_desktop::request_quit() {
  {
    std::lock_guard<std::mutex> lk(quit_mtx_);
    quit_requested_ = true;
  }
  quit_cv_.notify_all();
}

void wish_desktop::wait_for_quit() {
  std::unique_lock<std::mutex> lk(quit_mtx_);
  quit_cv_.wait(lk, [this] { return quit_requested_; });
}

bool wish_desktop::wait_for_quit_for(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(quit_mtx_);
  return quit_cv_.wait_for(lk, timeout, [this] { return quit_requested_; });
}

void wish_desktop::run_clock() {
  using namespace std::chrono_literals;
  std::unique_lock<std::mutex> lk(clock_mtx_);
  while (!stop_clock_.load()) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);

    dynamic fields;
    fields["text"_key] = std::string{buf};
    try {
      clock_proxy_->set(std::move(fields)).get();
    } catch (...) {
    }

    clock_cv_.wait_for(lk, 1s, [this] { return stop_clock_.load(); });
  }
}

// ── wish_desktop_app ─────────────────────────────────────────────────────────

std::string wish_desktop_app::bridge_description() const {
  return "wish desktop.\n"
         "Multiplexes downstream clients into one upstream wish server, "
         "rendering a menu bar and dockable area that downstream clients' "
         "windows attach into.";
}

std::unique_ptr<bison::rmi::bridge> wish_desktop_app::make_bridge(
    bison::rmi::transport::server_transport_iface& downstream_transport,
    std::unique_ptr<bison::rmi::transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params) {
  auto br =
      std::make_unique<wish_desktop>(downstream_transport, std::move(upstream_transport), std::move(upstream_params));
  desktop_ = br.get();
  return br;
}

void wish_desktop_app::on_listening() const {
  bison::app::bridge_app::on_listening();
  if (desktop_)
    desktop_->build_chrome();
}

void wish_desktop_app::wait_for_shutdown() {
  if (!desktop_) {
    bison::app::bridge_app::wait_for_shutdown();
    return;
  }

  using namespace std::chrono_literals;
  while (!desktop_->wait_for_quit_for(100ms)) {
    if (active_term_ && active_term_->has_exited())
      return;
  }
}

} // namespace bdg::wish
