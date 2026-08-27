// MIT License © 2025 Binary Dice Games
/// @file wish_server_c.cpp
/// @brief C ABI implementation for the wish server shared library.
///
/// Wraps bdg::wish::server (the real server implementation the `wish
/// server` CLI uses -- see app/wish_cli/server/wish_server_app.cpp, whose
/// renderer-construction logic this mirrors) in a plain-C interface, so any
/// language with a C FFI can host and render a wish session.
#include <context/logger.hpp>
#include <include/wish_server_c.h>
#include <server/console_renderer.hpp>
#include <server/renderer.hpp>
#include <server/server.hpp>

#ifdef WISH_SDL3_ENABLED
#include <sdl/sdl3_renderer.hpp>
#endif
#ifdef WISH_WEB_ENABLED
#include <web/web_renderer.hpp>
#endif
#if defined(WISH_SDL3_ENABLED) || defined(WISH_WEB_ENABLED)
#include <imgui.h>
#endif

#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "src/term/terminal.hpp"

// See the identical comment in wish_client_c.cpp: #including bison_c.cpp/
// rmi_c.cpp directly gives this file access to their file-local handle
// helpers (bison_handle_to_dynamic, ...) so wish_server_start() can decode
// its bison_handle params argument without a second bison-side API
// surface. Neither file may also be compiled as a separate source of the
// wish_server_dll target (see CMakeLists.txt).
#include "src/bison/bison_c.cpp"
#include "src/rmi/rmi_c.cpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Server handle ────────────────────────────────────────────────────────────

struct wish_server_handle_ {
  // Only engaged by wish_server_term_create(). Declared before transport_ so
  // that member destruction order (reverse of declaration) tears transport_
  // down first: term_server_transport borrows term_proc_'s read/write fds
  // (see terminal.hpp), which must still be open while that happens.
  std::unique_ptr<term::terminal> term_proc_;

  // Declared before server_ so that member destruction order (reverse of
  // declaration) tears server_ down first -- wish::server only borrows the
  // transport (see server.hpp's constructor doc: "must outlive the
  // server"), so the transport must still be alive while the server (and
  // its destructor's stop()-adjacent teardown) runs.
  std::unique_ptr<server_transport_iface> transport_;
  std::unique_ptr<wish::server> server_;

  bool verbose_ = false;
  wish::logger_ptr logger_;

  std::string last_error_;
};

// ── Renderer construction ────────────────────────────────────────────────────

namespace {

// Distinguishes an unknown/unsupported renderer_kind from every other
// make_renderer()/server construction failure, so wish_server_start() can
// report WISH_SERVER_ERR_BAD_RENDERER (its documented contract) instead of
// the generic WISH_SERVER_ERR_EXCEPTION.
struct bad_renderer_error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

#if defined(WISH_SDL3_ENABLED) || defined(WISH_WEB_ENABLED)
// Wraps an imgui-based renderer backend (sdl3_renderer / web_renderer) with a
// single fullscreen, chrome-less host window whose only content is an
// ImGui::DockSpace. That dockspace becomes the frame's ambient_dockspace_id(),
// so every connected session's un-positioned, dockable top-level Window docks
// into it by default (see render_window) -- letting one server host several
// clients' windows in a coherent docked layout. This is the same role
// host_renderer plays for the `wish server` CLI (which also adds a menu bar);
// the bare imgui_renderer draws no server-level chrome of its own, so an app
// that drives sdl3/web directly and roots its UI in its own DockSpaceViewport
// is never given a second, competing fullscreen dockspace. Uses the same
// host-window/dockspace ids as host_renderer so an imgui.ini layout carries
// across.
template <typename Base>
class dockspace_renderer : public Base {
 public:
  using Base::Base;

  void render_server_frame(const std::vector<wish::sync_context_ptr>& /*sessions*/) override {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg]);

    constexpr ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##wish_host_chrome", nullptr, host_flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    ImGuiID dock_id = ImGui::GetID("HostDockSpace");
    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    this->set_ambient_dockspace_id(dock_id);

    ImGui::End();
  }
};
#endif

std::unique_ptr<wish::renderer> make_renderer(const char* renderer_kind, const dynamic& params) {
  std::string kind{renderer_kind ? renderer_kind : ""};

  if (kind == "console")
    return std::make_unique<wish::console_renderer>();

  // sdl3/web backends are wrapped in dockspace_renderer (above) so the
  // hosted server gets a fullscreen dockspace for its sessions' windows.
  if (kind == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    auto title = params.get_as<std::string>("title"_key, "wish");
    auto width = params.get_as<int32_t>("width"_key, 1280);
    auto height = params.get_as<int32_t>("height"_key, 720);
    auto font_size = params.get_as<int32_t>("font_size"_key, 16);
    return std::make_unique<dockspace_renderer<wish::sdl3_renderer>>(
        title.c_str(), width, height, font_size);
#else
    throw bad_renderer_error(
        "renderer_kind=\"sdl3\" requested but wish_server_dll was built with WISH_ENABLE_SDL3=OFF");
#endif
  }

  if (kind == "web") {
#ifdef WISH_WEB_ENABLED
    auto bind_addr = params.get_as<std::string>("web_bind"_key, "127.0.0.1");
    auto port = params.get_as<int32_t>("web_port"_key, 8080);
    auto font_size = params.get_as<int32_t>("font_size"_key, 16);
    return std::make_unique<dockspace_renderer<wish::web_renderer>>(
        bind_addr, port, font_size);
#else
    throw bad_renderer_error(
        "renderer_kind=\"web\" requested but wish_server_dll was built with WISH_ENABLE_WEB=OFF");
#endif
  }

  throw bad_renderer_error("unknown renderer_kind '" + kind + "' (expected \"sdl3\", \"web\", or \"console\")");
}

} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

extern "C" wish_server_handle wish_server_tcp_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    auto state = std::make_unique<wish_server_handle_>();
    state->transport_ = std::make_unique<socket_server_transport>(host, port);
    return state.release();
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_server_handle wish_server_pipe_create(const char* path) {
  if (!path)
    return nullptr;
  try {
    auto state = std::make_unique<wish_server_handle_>();
    state->transport_ = std::make_unique<named_pipe_server_transport>(path);
    return state.release();
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_server_handle wish_server_tls_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    auto state = std::make_unique<wish_server_handle_>();
    state->transport_ = std::make_unique<tls_socket_server_transport>(host, port);
    return state.release();
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_server_handle wish_server_term_create(const char* cmd) {
  try {
    auto state = std::make_unique<wish_server_handle_>();
    state->term_proc_ = std::make_unique<term::terminal>(cmd ? cmd : std::string{});
    state->term_proc_->start_pump();
    state->transport_ = std::make_unique<term_server_transport>(
        state->term_proc_->read_handle(), state->term_proc_->write_handle());
    return state.release();
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_server_error
wish_server_start(wish_server_handle s, const char* renderer_kind, bison_handle params) {
  if (!s || !renderer_kind)
    return WISH_SERVER_ERR_NULL;
  if (s->server_) {
    s->last_error_ = "server already started";
    return WISH_SERVER_ERR_EXCEPTION;
  }
  try {
    dynamic dyn_params = bison_handle_to_dynamic(params);
    auto renderer = make_renderer(renderer_kind, dyn_params);
    s->server_ = std::make_unique<wish::server>(*s->transport_, std::move(renderer));
    if (s->verbose_) {
      s->logger_ = std::make_shared<wish::logger>(
          dynamic::instantiate(bdg::bison::key_t{"wish"}, bdg::bison::key_t{"__WishLogger"}),
          /*verbose=*/true,
          std::filesystem::path{});
      s->server_->set_logger(s->logger_);
    }
    // Forwarded unchanged as listen params -- e.g. cert_file/key_file/etc.
    // for a wish_server_tls_create() transport; ignored by every other
    // transport's start().
    s->server_->start(nullptr, dyn_params);
    return WISH_SERVER_OK;
  } catch (const bad_renderer_error& e) {
    s->last_error_ = e.what();
    s->server_.reset();
    return WISH_SERVER_ERR_BAD_RENDERER;
  } catch (const std::exception& e) {
    s->last_error_ = e.what();
    s->server_.reset();
    return WISH_SERVER_ERR_EXCEPTION;
  } catch (...) {
    s->last_error_ = "unknown exception";
    s->server_.reset();
    return WISH_SERVER_ERR_EXCEPTION;
  }
}

extern "C" wish_server_error wish_server_stop(wish_server_handle s) {
  if (!s)
    return WISH_SERVER_ERR_NULL;
  if (!s->server_)
    return WISH_SERVER_OK;
  try {
    s->server_->stop();
    return WISH_SERVER_OK;
  } catch (const std::exception& e) {
    s->last_error_ = e.what();
    return WISH_SERVER_ERR_EXCEPTION;
  }
}

extern "C" int wish_server_should_quit(wish_server_handle s) {
  if (!s || !s->server_)
    return 0;
  if (s->term_proc_ && s->term_proc_->has_exited())
    return 1;
  return s->server_->should_quit() ? 1 : 0;
}

extern "C" wish_server_error wish_server_set_verbose(wish_server_handle s, int verbose) {
  if (!s)
    return WISH_SERVER_ERR_NULL;
  if (s->server_) {
    s->last_error_ = "wish_server_set_verbose() must be called before wish_server_start()";
    return WISH_SERVER_ERR_EXCEPTION;
  }
  s->verbose_ = verbose != 0;
  return WISH_SERVER_OK;
}

extern "C" void wish_server_destroy(wish_server_handle s) {
  if (!s)
    return;
  if (s->server_)
    s->server_->stop();
  delete s;
}

extern "C" const char* wish_server_last_error(wish_server_handle s) {
  if (!s)
    return "";
  return s->last_error_.c_str();
}
