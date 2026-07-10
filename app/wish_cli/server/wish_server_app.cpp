// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.cpp
 * @brief wish GUI server application implementation.
 */
#include "app/wish_cli/server/wish_server_app.hpp"

#include "app/wish_cli/env_flags.hpp"

#include <context/logger.hpp>
#include <server/registry.hpp>
#include <sdl/sdl3_renderer.hpp>
#include <server/server.hpp>
#include <web/web_renderer.hpp>

#include <imgui.h>

#include "src/app/transport_flags.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"

#include <gflags/gflags.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

DECLARE_bool(verbose);
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_string(name);
DECLARE_string(cmd);

DEFINE_string(title, "wish", "Window title");
DEFINE_int32(width, 1280, "Window width in pixels");
DEFINE_int32(height, 720, "Window height in pixels");
DEFINE_int32(font_size, 16, "Font size in pixels");
DEFINE_string(renderer, "web", "Rendering backend: sdl3 or web");
// Deliberately NOT named --port: that flag is already bison's TCP RMI
// transport port (see main.cpp), an unrelated concept from the web
// renderer's HTTP/WebSocket port.
DEFINE_int32(web_port, 8080, "HTTP/WebSocket port for --renderer web");
DEFINE_string(web_bind, "127.0.0.1", "Bind address for --renderer web (localhost-only by default)");

namespace bdg::wish {

// ── server_renderer ───────────────────────────────────────────────────────────
//
// Extends a given imgui-based renderer backend (sdl3_renderer or
// web_renderer) with a fullscreen host window that provides:
//   - A DockSpace so client windows can be docked anywhere in the server view.
//   - A menu bar with server-level actions (including Quit).
//
// Templatized on the backend so the same host-window UI is shared by every
// renderer that draws through ImGui, rather than duplicating it per backend.

template <typename Base>
class server_renderer : public Base {
 public:
  using Base::Base;

  void render_server_frame() override {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

    ImGui::Begin("##wish_server_host", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("Server")) {
        if (ImGui::MenuItem("Quit", "Alt+F4"))
          this->request_quit();
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("(no options yet)", nullptr, false, false);
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    ImGuiID dock_id = ImGui::GetID("ServerDockSpace");
    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
  }
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::unique_ptr<renderer> make_renderer() {
  if (FLAGS_renderer == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    return std::make_unique<server_renderer<sdl3_renderer>>(
        FLAGS_title.c_str(), FLAGS_width, FLAGS_height, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=sdl3 requested but this binary was built with WISH_ENABLE_SDL3=OFF");
#endif
  }
  if (FLAGS_renderer == "web") {
#ifdef WISH_WEB_ENABLED
    return std::make_unique<server_renderer<web_renderer>>(FLAGS_web_bind, FLAGS_web_port, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=web requested but this binary was built with WISH_ENABLE_WEB=OFF");
#endif
  }
  throw std::runtime_error("unknown --renderer value '" + FLAGS_renderer + "' (expected sdl3 or web)");
}

static std::shared_ptr<logger> make_server_logger() {
  return std::make_shared<logger>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishLogger"}),
      FLAGS_verbose,
      std::filesystem::path{"wish_logs"} / "server.log");
}

// ── server_app overrides ──────────────────────────────────────────────────────

int wish_server_app::run(int argc, char** argv) {
  apply_env_flag_defaults();
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

  if (FLAGS_renderer == "web") {
    std::cout << "[wish] open http://" << FLAGS_web_bind << ':' << FLAGS_web_port << " in a browser\n" << std::flush;
  }
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
  server_log_ = make_server_logger();
  auto srv_owner = make_server(transport);
  auto& srv = static_cast<server&>(*srv_owner);
  srv.set_logger(server_log_);
  srv.start();
  server_log_->info("server started");
  on_listening();
  // Stops on whichever comes first: the renderer's own close signal (sdl3
  // window close; web has none), or -- when --transport=term spawned a
  // terminal -- that terminal process exiting. This is what lets --renderer
  // web be stopped at all: it has no window and nothing installs a SIGINT
  // handler for this process.
  while (!srv.should_quit() && !is_shutdown_requested())
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  srv.stop();
  server_log_.reset();
  return 0;
}

} // namespace bdg::wish
