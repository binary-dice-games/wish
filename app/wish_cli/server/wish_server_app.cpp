// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.cpp
 * @brief wish GUI server application implementation.
 */
#include "app/wish_cli/server/wish_server_app.hpp"

#include <logger.hpp>
#include <registry.hpp>
#include <sdl3_renderer.hpp>
#include <server.hpp>
#include <web/web_renderer.hpp>

#include <imgui.h>

#include "src/app/transport_flags.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/term/terminal.hpp"

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
DEFINE_string(renderer, "sdl3", "Rendering backend: sdl3 or web");
// Deliberately NOT named --port: that flag is already bison's TCP RMI
// transport port (see main.cpp), an unrelated concept from the web
// renderer's HTTP/WebSocket port.
DEFINE_int32(web_port, 8080, "HTTP/WebSocket port for --renderer web");
DEFINE_string(web_bind, "127.0.0.1", "Bind address for --renderer web (localhost-only by default)");

namespace bdg::wish {

#ifdef WISH_SDL3_ENABLED

// ── server_renderer ───────────────────────────────────────────────────────────
//
// Extends sdl3_renderer with a fullscreen host window that provides:
//   - A DockSpace so client windows can be docked anywhere in the server view.
//   - A menu bar with server-level actions (including Quit).

class server_renderer : public sdl3_renderer {
 public:
  using sdl3_renderer::sdl3_renderer;

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
          request_quit();
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

#endif // WISH_SDL3_ENABLED

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::unique_ptr<renderer> make_renderer() {
  if (FLAGS_renderer == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    return std::make_unique<server_renderer>(FLAGS_title.c_str(), FLAGS_width, FLAGS_height, FLAGS_font_size);
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

static std::shared_ptr<logger> make_server_logger() {
  return std::make_shared<logger>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishLogger"}),
      FLAGS_verbose,
      std::filesystem::path{"wish_logs"} / "server.log");
}

// ── server_app overrides ──────────────────────────────────────────────────────

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

  std::stringstream ss;
  using bison::app::transport_kind;
  switch (bison::app::selected_transport()) {
    case transport_kind::pipe:
      std::cout << "[wish] listening on pipe " << FLAGS_name << " - " << stop_hint << "\n" << std::flush;
      break;
    case transport_kind::term:
      std::cout << "[wish] listening via --transport=term (spawned: " << FLAGS_cmd << ") - " << stop_hint << "\n"
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

int wish_server_app::run_with_transport(
    bison::rmi::transport::server_transport_iface& transport,
    std::function<void()> wait_for_shutdown) {
  server_log_ = make_server_logger();
  server srv{transport, make_renderer()};
  srv.set_logger(server_log_);
  srv.start();
  server_log_->info("server started");
  on_listening();
  while (!srv.should_quit())
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  srv.stop();
  server_log_.reset();
  return 0;
}

} // namespace bdg::wish
