// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server.cpp
 * @brief wish GUI server application implementation.
 */
#include "app/wish_server/wish_server.hpp"

#include <wish/registry.hpp>
#include <wish/sdl3_renderer.hpp>
#include <wish/server.hpp>

#include <imgui.h>

#if defined(__linux__)
#  include "src/app/pty/pty_server_transport.hpp"
#endif

#include <gflags/gflags.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

DECLARE_string(host);
DECLARE_int32 (port);
DECLARE_string(pipe);
DECLARE_string(title);
DECLARE_int32 (width);
DECLARE_int32 (height);


#if defined(__linux__)
DECLARE_string(cmd);
#endif

namespace bdg::wish {

// ── server_renderer ───────────────────────────────────────────────────────────
//
// Extends sdl3_renderer with a fullscreen host window that provides:
//   - A DockSpace so client windows can be docked anywhere in the server view.
//   - A menu bar with server-level actions (including Quit).

class server_renderer : public sdl3_renderer {
 public:
  using sdl3_renderer::sdl3_renderer;

  void render_server_frame() override {
    // Cover the entire main viewport with a borderless host window.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoDocking          |
        ImGuiWindowFlags_NoTitleBar         |
        ImGuiWindowFlags_NoCollapse         |
        ImGuiWindowFlags_NoResize           |
        ImGuiWindowFlags_NoMove             |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus         |
        ImGuiWindowFlags_MenuBar;

    ImGui::Begin("##wish_server_host", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    // ── Menu bar ──────────────────────────────────────────────────────────────
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

    // ── DockSpace ─────────────────────────────────────────────────────────────
    ImGuiID dock_id = ImGui::GetID("ServerDockSpace");
    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
    // Client session windows are rendered after this by server::render_loop
    // and will dock into the dockspace created above.
  }
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::unique_ptr<sdl3_renderer> make_renderer() {
  return std::make_unique<server_renderer>(
      FLAGS_title.c_str(), FLAGS_width, FLAGS_height);
}

// ── srv_app overrides ─────────────────────────────────────────────────────────

std::string wish_server::server_description() const {
  return "wish GUI server.\n"
         "Opens an SDL3 window and renders UI pushed by connected clients.\n"
         "Close the window to stop.";
}

void wish_server::register_classes() {
  register_all();
}

void wish_server::on_listening() const {
  if (!FLAGS_pipe.empty()) {
    std::cout << "[wish] listening on pipe " << FLAGS_pipe
              << " - close the window to stop\n" << std::flush;
  } else {
    std::cout << "[wish] listening on " << FLAGS_host << ':' << FLAGS_port
              << " - close the window to stop\n" << std::flush;
  }
}

int wish_server::run_with_transport(
    bison::rmi::transport::server_transport_iface& transport) {
  server srv{transport, make_renderer()};
  srv.start();
  on_listening();
  while (!srv.should_quit())
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  srv.stop();
  return 0;
}

// ── Linux PTY support ─────────────────────────────────────────────────────────

#if defined(__linux__)

void wish_server::on_listening_pty() const {
  std::cout << "[wish] PTY server started (cmd: " << FLAGS_cmd
            << ") - close the window to stop\n"
            << std::flush;
}

int wish_server::run_pty() {
  using bison::app::pty_server_transport;

  pty_server_transport pty{FLAGS_cmd};
  bison::dynamic params;
  params["mode"_key] = std::string{"dcs"};
  pty.start(std::move(params));
  on_listening_pty();

  // Local server subclass that restarts the PTY between client sessions.
  struct pty_server_impl : public server {
    using server::server;
    pty_server_transport* pty = nullptr;
   protected:
    void on_session_destroyed(session&) override {
      if (pty && pty->is_shell_running())
        pty->restart_session();
    }
  };

  pty_server_impl srv{pty, make_renderer()};
  srv.pty = &pty;
  srv.start();
  while (!srv.should_quit())
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  srv.stop();
  pty.stop();
  return 0;
}

#endif // defined(__linux__)

} // namespace bdg::wish
