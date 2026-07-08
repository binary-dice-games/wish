// MIT License © 2025 Binary Dice Games
/// @file web_renderer.cpp
/// @brief Implementation of wish::web_renderer.
#include <web/web_renderer.hpp>

#ifdef WISH_WEB_ENABLED

#include <web/draw_protocol.hpp>

#include <resource_store.hpp>

#include <imgui.h>
#include <implot.h>
#include <implot3d.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bdg::wish {

// ── construction ──────────────────────────────────────────────────────────────

web_renderer::web_renderer(std::string bind_addr, int port, int font_size)
    : bind_addr_(std::move(bind_addr)), port_(port), font_size_(font_size) {}

web_renderer::~web_renderer() {
  // teardown() is called from the render thread before the render loop
  // exits. If it was never called (e.g. setup() threw), nothing needs
  // freeing here.
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

void web_renderer::setup() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImPlot3D::CreateContext();
  ImGui::StyleColorsDark();

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigDockingWithShift = true;
  // No GPU backend exists to eagerly build/upload the font atlas, so opt
  // into ImGui's incremental texture-management path: end_frame() walks
  // ImDrawData::Textures each frame and reacts to ImTextureData::Status
  // (WantCreate/WantUpdates/WantDestroy) instead.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  // Deliberately NOT setting ImGuiBackendFlags_RendererHasVtxOffset: the
  // browser client renders with core WebGL2 (GLES 3.0), which has no
  // "draw with base vertex" call. Leaving this unset keeps ImGui splitting
  // draw lists so ImDrawCmd::VtxOffset is always 0 (see client.js).
  // No platform window to query a size from; default until the browser
  // sends a RESIZE message. imgui_renderer::begin_frame() also defends
  // against an unset DisplaySize.
  io.DisplaySize = ImVec2(1280.0f, 720.0f);

  ImGuiStyle& style = ImGui::GetStyle();
  style.FontSizeBase = static_cast<float>(font_size_);

  // civetweb's document_root is one static path shared by every browser
  // connection -- unlike the per-session resource_dir extracted by
  // context::context(), this is a process-global directory extracted once
  // here. The pid suffix keeps concurrent web_renderer instances (e.g. in
  // tests) from colliding on the same path.
#if defined(_WIN32)
  long pid = static_cast<long>(::_getpid());
#else
  long pid = static_cast<long>(::getpid());
#endif
  web_assets_dir_ = std::filesystem::temp_directory_path() /
                    ("wish_web_assets_" + std::to_string(pid));
  std::filesystem::create_directories(web_assets_dir_);
  resource_store::extract_to(web_assets_dir_);

  server_ = std::make_unique<civetweb_server>(
      bind_addr_, port_, web_assets_dir_ / "web",
      /*on_connect=*/
      [this](ws_connection_id id) {
        pending_sync_.wlock()->push_back(id);
        activity_.store(true, std::memory_order_relaxed);
      },
      /*on_disconnect=*/[this](ws_connection_id) { activity_.store(true, std::memory_order_relaxed); },
      /*on_message=*/
      [this](ws_connection_id, std::span<const std::byte> message) {
        if (auto ev = draw_protocol::decode_input_message(message)) {
          input_queue_.wlock()->push_back(*ev);
          activity_.store(true, std::memory_order_relaxed);
        } else if (auto resize = draw_protocol::decode_resize_message(message)) {
          *pending_resize_.wlock() = *resize;
          activity_.store(true, std::memory_order_relaxed);
        }
        // Unrecognized messages are silently dropped -- decode_*_message()
        // already rejects malformed input; nothing else to do here.
      });
  server_->start();
}

void web_renderer::teardown() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
  if (!web_assets_dir_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(web_assets_dir_, ec);
    // Ignore errors on cleanup (e.g. already removed externally).
  }

  ImPlot3D::DestroyContext();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
}

// ── per-frame ─────────────────────────────────────────────────────────────────

bool web_renderer::poll_events() {
  return activity_.exchange(false, std::memory_order_relaxed);
}

void web_renderer::begin_frame() {
  ImGuiIO& io = ImGui::GetIO();

  std::optional<web_resize_event> resize;
  {
    auto lock = pending_resize_.wlock();
    resize = *lock;
    lock->reset();
  }
  if (resize) {
    io.DisplaySize = ImVec2(resize->width, resize->height);
    io.DisplayFramebufferScale = ImVec2(resize->dpr, resize->dpr);
  }

  std::deque<web_input_event> events;
  {
    auto lock = input_queue_.wlock();
    events = std::move(*lock);
    lock->clear();
  }
  for (const web_input_event& ev : events) {
    switch (ev.kind) {
      case web_input_kind::mouse_move:
        io.AddMousePosEvent(ev.x, ev.y);
        break;
      case web_input_kind::mouse_button:
        io.AddMouseButtonEvent(ev.button, ev.down);
        break;
      case web_input_kind::mouse_wheel:
        io.AddMouseWheelEvent(ev.wheel_x, ev.wheel_y);
        break;
      case web_input_kind::key:
        io.AddKeyEvent(static_cast<ImGuiKey>(ev.key_code), ev.down);
        break;
      case web_input_kind::char_input:
        io.AddInputCharacter(ev.codepoint);
        break;
    }
  }

  imgui_renderer::begin_frame(); // → ImGui::NewFrame()
}

void web_renderer::end_frame() {
  // ImGui::Render() subsumes EndFrame() — do NOT call
  // imgui_renderer::end_frame().
  ImGui::Render();

  const ImDrawData* draw_data = ImGui::GetDrawData();
  if (!draw_data || !draw_data->Valid || !server_ || !draw_data->Textures)
    return;

  // Texture (re)uploads must be broadcast before the FRAME that references
  // their ids, and before any draw command's GetTexID() is called (which
  // asserts a valid TexID) -- draw_protocol::encode_frame() calls GetTexID()
  // per command, so this must run first.
  for (ImTextureData* tex : *draw_data->Textures) {
    switch (tex->Status) {
      case ImTextureStatus_WantCreate: {
        uint32_t id = next_texture_id_++;
        texture_ids_[tex] = id;
        tex->SetTexID(static_cast<ImTextureID>(id));
        server_->broadcast(draw_protocol::encode_texture_update(id, *tex));
        tex->SetStatus(ImTextureStatus_OK);
        break;
      }
      case ImTextureStatus_WantUpdates: {
        auto it = texture_ids_.find(tex);
        uint32_t id = it != texture_ids_.end() ? it->second : next_texture_id_++;
        texture_ids_[tex] = id;
        server_->broadcast(draw_protocol::encode_texture_update(id, *tex));
        tex->SetStatus(ImTextureStatus_OK);
        break;
      }
      case ImTextureStatus_WantDestroy: {
        auto it = texture_ids_.find(tex);
        if (it != texture_ids_.end()) {
          server_->broadcast(draw_protocol::encode_texture_destroy(it->second));
          texture_ids_.erase(it);
        }
        tex->SetStatus(ImTextureStatus_Destroyed);
        break;
      }
      default:
        break; // OK / Destroyed: nothing to do this frame.
    }
  }

  // A newly-connected browser needs the *current* texture state even if
  // nothing changed this frame (Status only transitions once per upload) --
  // resend every live texture as a full upload, to that connection only.
  std::vector<ws_connection_id> pending;
  {
    auto lock = pending_sync_.wlock();
    pending = std::move(*lock);
    lock->clear();
  }
  if (!pending.empty()) {
    for (ImTextureData* tex : *draw_data->Textures) {
      if (tex->Status != ImTextureStatus_OK)
        continue;
      auto it = texture_ids_.find(tex);
      if (it == texture_ids_.end())
        continue;
      // Temporarily present as WantCreate so encode_texture_update() emits
      // a whole-texture payload; restored immediately after. Safe because
      // no ImGui core logic runs between the two assignments (Status is
      // only inspected again at the next NewFrame()).
      ImTextureStatus saved = tex->Status;
      tex->Status = ImTextureStatus_WantCreate;
      auto bytes = draw_protocol::encode_texture_update(it->second, *tex);
      tex->Status = saved;
      for (ws_connection_id id : pending)
        server_->send_to(id, bytes);
    }
  }

  server_->broadcast(draw_protocol::encode_frame(*draw_data));
}

bool web_renderer::should_quit() const {
  return quit_.load(std::memory_order_acquire);
}

void web_renderer::request_quit() {
  quit_.store(true, std::memory_order_release);
}

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
