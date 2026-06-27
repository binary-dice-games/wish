// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_app.cpp
 * @brief wish GUI server application implementation.
 */
#include "app/wish_cli/server/wish_server_app.hpp"

#include <wish/logger.hpp>
#include <wish/registry.hpp>
#include <wish/sdl3_renderer.hpp>
#include <wish/server.hpp>

#include <imgui.h>

#if defined(__linux__) || defined(_WIN32)
#  include "src/rmi/transport/pty_server_transport.hpp"
#endif
#if defined(_WIN32)
#  include "src/rmi/transport/named_pipe_transport.hpp"
#endif

#include <gflags/gflags.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

DECLARE_bool  (verbose);
DECLARE_string(host);
DECLARE_int32 (port);
DECLARE_string(pipe);

DEFINE_string(title,  "wish", "Window title");
DEFINE_int32 (width,  1280,   "Window width in pixels");
DEFINE_int32 (height, 720,    "Window height in pixels");

#if defined(__linux__) || defined(_WIN32)
DECLARE_bool  (pty);
#  if defined(_WIN32)
DEFINE_string (cmd, "cmd.exe", "Shell command spawned for PTY transport");
#  else
DEFINE_string (cmd, "bash",    "Shell command spawned for PTY transport");
#  endif
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
    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
  }
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::unique_ptr<sdl3_renderer> make_renderer() {
  return std::make_unique<server_renderer>(
      FLAGS_title.c_str(), FLAGS_width, FLAGS_height);
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
         "Opens an SDL3 window and renders UI pushed by connected clients.\n"
         "Close the window to stop.";
}

void wish_server_app::register_classes() {
  register_all();
}

void wish_server_app::on_listening() const {
  if (!FLAGS_pipe.empty()) {
    std::cout << "[wish] listening on pipe " << FLAGS_pipe
              << " - close the window to stop\n" << std::flush;
  } else {
    std::cout << "[wish] listening on " << FLAGS_host << ':' << FLAGS_port
              << " - close the window to stop\n" << std::flush;
  }
}

void wish_server_app::on_verbose_trace(bison::key_t /*session_id*/,
                                       const std::string& line) const {
  if (server_log_) server_log_->info(line);
}

int wish_server_app::run_with_transport(
    bison::rmi::transport::server_transport_iface& transport) {
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

// ── PTY support (Linux and Windows) ──────────────────────────────────────────

#if defined(__linux__) || defined(_WIN32)

void wish_server_app::on_listening_pty() const {
  std::cout << "[wish] PTY server started (cmd: " << FLAGS_cmd
            << ") - close the window to stop\n"
            << std::flush;
}

#if defined(_WIN32)

// On Windows, ConPTY is not a transparent byte pipe — it interprets DCS
// frames as VT terminal sequences and swallows them before they reach the
// server.  We therefore use a Windows named pipe as the actual RMI channel
// and keep ConPTY purely for interactive shell I/O.
//
// Setting BISON_PTY_PIPE in the environment before spawning the shell means
// cmd.exe (and any child wish-client process) inherits it automatically.
// wish-client --pty on Windows checks that variable and connects via named
// pipe instead of DCS-over-stdin/stdout.
int wish_server_app::run_pty() {
  using bison::app::pty_server_transport;
  using rmi::transport::named_pipe_server_transport;

  const std::string pipe_name =
      R"(\\.\pipe\wish-pty-)" + std::to_string(GetCurrentProcessId());

  // Expose pipe name before spawning the shell so cmd.exe inherits it.
  SetEnvironmentVariableA("BISON_PTY_PIPE", pipe_name.c_str());
  pty_server_transport shell{FLAGS_cmd};
  shell.start({});
  // Clear from our own environment; cmd.exe already has the value.
  SetEnvironmentVariableA("BISON_PTY_PIPE", nullptr);

  on_listening_pty();

  named_pipe_server_transport pipe_srv{pipe_name};

  server_log_ = make_server_logger();
  server srv{pipe_srv, make_renderer()};
  srv.set_logger(server_log_);
  srv.start();
  server_log_->info("server started (PTY/pipe)");
  while (!srv.should_quit())
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  srv.stop();
  shell.stop();
  server_log_.reset();
  return 0;
}

#else // Linux: DCS frames pass through PTY transparently.

int wish_server_app::run_pty() {
  using bison::app::pty_server_transport;

  pty_server_transport pty{FLAGS_cmd};
  bison::dynamic params;
  params[bdg::bison::key_t{"mode"}] = std::string{"dcs"};
  pty.start(std::move(params));
  on_listening_pty();

  struct pty_server_impl : public server {
    using server::server;
    pty_server_transport* pty = nullptr;
   protected:
    void on_session_destroyed(session&) override {
      if (pty && pty->is_shell_running())
        pty->restart_session();
    }
  };

  server_log_ = make_server_logger();
  pty_server_impl srv{pty, make_renderer()};
  srv.pty = &pty;
  srv.set_logger(server_log_);
  srv.start();
  server_log_->info("server started (PTY)");
  while (!srv.should_quit())
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  srv.stop();
  pty.stop();
  server_log_.reset();
  return 0;
}

#endif

#endif // defined(__linux__) || defined(_WIN32)

} // namespace bdg::wish
