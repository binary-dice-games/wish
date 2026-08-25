// MIT License © 2025 Binary Dice Games
/// @file web_renderer.cpp
/// @brief Implementation of wish::web_renderer.
#include <web/web_renderer.hpp>

#ifdef WISH_WEB_ENABLED

#include <web/draw_protocol.hpp>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_query.hpp>
#endif

#include <resource_store.hpp>

#include <imgui.h>
#include <imgui_internal.h> // ImGui::RegisterUserTexture/UnregisterUserTexture
#include <implot.h>
#include <implot3d.h>
#include <miniz.h> // mz_crc32 -- on-the-fly content versioning for uploaded files

// stb_image is vendored solely for get_or_load_texture() -- WISH_ENABLE_SDL3
// (and its transitive SDL3_image dependency) is not guaranteed to be on in a
// web-only build, so this backend needs its own decoder. Implementation is
// compiled into this translation unit only; never included from the header
// (see src/web/DESIGN.md's isolation note for civetweb/stb_image).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <cstring>
#include <fstream>
#include <iterator>
namespace bdg::wish {

// ── construction ──────────────────────────────────────────────────────────────

web_renderer::web_renderer(
    std::string bind_addr, int port, int font_size, render_fn_map extra_render_fns, bool render_on_demand)
    : imgui_renderer(std::move(extra_render_fns)), bind_addr_(std::move(bind_addr)), port_(port),
      font_size_(font_size), render_on_demand_(render_on_demand) {}

web_renderer::~web_renderer() {
  // teardown() is called from the render thread before the render loop
  // exits. If it was never called (e.g. setup() threw), nothing needs
  // freeing here.
}

// ── clipboard ─────────────────────────────────────────────────────────────────

namespace {
// The web_renderer currently owning the active ImGui context -- see the
// doc comment on get_clipboard_text()/set_clipboard_text() in the header
// for why this can't just be a PlatformIO user-data pointer.
web_renderer* g_clipboard_renderer = nullptr;
} // namespace

const char* web_renderer::get_clipboard_text(ImGuiContext* /*ctx*/) {
  if (!g_clipboard_renderer)
    return "";
  g_clipboard_renderer->clipboard_text_scratch_ = *g_clipboard_renderer->pending_clipboard_text_.rlock();
  return g_clipboard_renderer->clipboard_text_scratch_.c_str();
}

void web_renderer::set_clipboard_text(ImGuiContext* /*ctx*/, const char* text) {
  if (g_clipboard_renderer && g_clipboard_renderer->server_)
    g_clipboard_renderer->server_->broadcast(draw_protocol::encode_clipboard_write(text ? text : ""));
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
  // The render loop only calls NewFrame() when activity/dirty triggers a
  // frame (see server::render_loop()), not once per hardware tick -- so a
  // fast click's down+up can both land in one input_queue_ drain before the
  // next frame runs. ImGui's default event trickling would then apply only
  // the down event this frame and defer the up event to a NewFrame() that
  // may never come without unrelated further activity, silently stalling
  // clicks (e.g. a window's collapse arrow) until an incidental mouse move.
  // Flatten same-frame button transitions instead of trickling them.
  io.ConfigInputTrickleEventQueue = false;

  // Anchor imgui.ini to an absolute path snapshotted now, rather than
  // ImGui's CWD-relative default -- see ini_path_'s doc comment.
  std::error_code ec;
  auto abs_cwd = std::filesystem::current_path(ec);
  ini_path_ = (ec ? std::filesystem::path("imgui.ini") : abs_cwd / "imgui.ini").string();
  io.IniFilename = ini_path_.c_str();

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

  // No platform backend exists to wire up OS clipboard access for us (see
  // "Clipboard bridging" in src/web/DESIGN.md) -- without this, ImGui falls
  // back to its own internal, session-local clipboard buffer, so copy/paste
  // works *within* one running app but never interops with the browser's
  // real OS clipboard (e.g. pasting JSON copied from an external editor).
  g_clipboard_renderer = this;
  ImGui::GetPlatformIO().Platform_GetClipboardTextFn = &web_renderer::get_clipboard_text;
  ImGui::GetPlatformIO().Platform_SetClipboardTextFn = &web_renderer::set_clipboard_text;

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
        connected_ids_.wlock()->insert(id);
        pending_sync_.wlock()->push_back(id);
        activity_.store(true, std::memory_order_relaxed);
      },
      /*on_disconnect=*/
      [this](ws_connection_id id) {
        connected_ids_.wlock()->erase(id);
        awaiting_cache_response_.wlock()->erase(id);
        activity_.store(true, std::memory_order_relaxed);
      },
      /*on_message=*/
      [this](ws_connection_id id, std::span<const std::byte> message) {
        if (auto ev = draw_protocol::decode_input_message(message)) {
          input_queue_.wlock()->push_back(*ev);
          activity_.store(true, std::memory_order_relaxed);
        } else if (auto resize = draw_protocol::decode_resize_message(message)) {
          *pending_resize_.wlock() = *resize;
          activity_.store(true, std::memory_order_relaxed);
        } else if (auto resp = draw_protocol::decode_cache_response_message(message)) {
          cache_response_queue_.wlock()->push_back({id, *resp});
          activity_.store(true, std::memory_order_relaxed);
        } else if (auto clip = draw_protocol::decode_clipboard_text_message(message)) {
          *pending_clipboard_text_.wlock() = std::move(*clip);
          // No activity_ bump: this alone never needs a frame redrawn, and
          // it always arrives immediately before the paste keystroke's own
          // INPUT message, which does set it.
#ifdef WISH_AUTOMATION_ENABLED
        } else if (auto query_json = draw_protocol::decode_query_tree_message(message)) {
          pending_tree_queries_.wlock()->push_back({id, std::move(*query_json)});
          activity_.store(true, std::memory_order_relaxed);
          // A tree query wants this frame's *current* state -- request one
          // explicitly so it's answered against something fresh even under
          // render_on_demand() (where activity_ above doesn't by itself
          // trigger a draw). Harmless, cheap no-op when not render_on_demand().
          request_render();
        } else if (draw_protocol::decode_request_render_message(message)) {
          request_render();
#endif
        }
        // Unrecognized messages are silently dropped -- decode_*_message()
        // already rejects malformed input; nothing else to do here.
      });
  server_->start();
}

void web_renderer::teardown() {
  if (g_clipboard_renderer == this)
    g_clipboard_renderer = nullptr;
  if (server_) {
    server_->stop();
    server_.reset();
  }
  if (!web_assets_dir_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(web_assets_dir_, ec);
    // Ignore errors on cleanup (e.g. already removed externally).
  }

  for (auto& tex : loaded_textures_)
    ImGui::UnregisterUserTexture(tex.get());
  loaded_textures_.clear();
  loaded_by_src_.clear();
  texture_cache_.clear();
  texture_ids_.clear();
  textures_by_id_.clear();
  texture_meta_.clear();
  connected_ids_.wlock()->clear();
  awaiting_cache_response_.wlock()->clear();
  cache_response_queue_.wlock()->clear();
  render_target_id_ = 0;
  render_target_w_ = 0;
  render_target_h_ = 0;
  current_target_id_ = 0;
  saved_render_target_id_ = 0;
#ifdef WISH_AUTOMATION_ENABLED
  hit_test_map_.clear();
  pending_tree_queries_.wlock()->clear();
  last_broadcast_log_seq_ = 0;
#endif

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

#ifdef WISH_AUTOMATION_ENABLED
  // Rebuilt fresh every frame by render_node() below, so a query answered
  // from service_automation_queries() always reflects the most recently
  // completed frame, never a stale or partially-rendered one.
  hit_test_map_.clear();
#endif

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
      case web_input_kind::key: {
        const auto key = static_cast<ImGuiKey>(ev.key_code);
        io.AddKeyEvent(key, ev.down);

        // ImGui derives io.KeyShift/io.KeyMods (and thus
        // ConfigDockingWithShift) -- and every Ctrl/Shift/Alt/Super-modified
        // shortcut check, e.g. TextEditor's Ctrl+A/C/V/X -- from a separate
        // merged ImGuiMod_Shift/Ctrl/Alt/Super key, not from LeftShift/
        // RightShift directly -- mirror ImGui_ImplSDL3_UpdateKeyModifiers()
        // (imgui_impl_sdl3.cpp) and emit that merged event too whenever a
        // left/right modifier key changes.
        auto update_mod = [&](modifier_state& state, ImGuiKey left,
                               ImGuiKey right, ImGuiKey mod) {
          if (key == left)
            state.left = ev.down;
          else if (key == right)
            state.right = ev.down;
          else
            return;
          io.AddKeyEvent(mod, state.any());
        };
        update_mod(mod_ctrl_, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, ImGuiMod_Ctrl);
        update_mod(mod_shift_, ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiMod_Shift);
        update_mod(mod_alt_, ImGuiKey_LeftAlt, ImGuiKey_RightAlt, ImGuiMod_Alt);
        update_mod(mod_super_, ImGuiKey_LeftSuper, ImGuiKey_RightSuper, ImGuiMod_Super);
        break;
      }
      case web_input_kind::char_input:
        io.AddInputCharacter(ev.codepoint);
        break;
      case web_input_kind::touch_down:
      case web_input_kind::touch_move:
      case web_input_kind::touch_up:
      case web_input_kind::touch_cancel:
      case web_input_kind::motion:
        // Not modeled as ImGui io events -- wish's own generic UI has no
        // per-session input-action consumer to hand these to (unlike
        // genie's web_deferred_renderer, which queues them separately for
        // genie::input_manager). Ignored here, not an error.
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

  // Texture ids assigned a fresh WantCreate this frame, and thus already
  // offered (TEX_CHECK or full TEX_CREATE, per cacheability) to every
  // currently-connected client below -- the pending_sync_ drain further
  // down must skip these so a client that just connected this same frame
  // doesn't get sent a second, redundant TEX_CHECK for the same texture.
  std::unordered_set<ImTextureData*> created_this_frame;

  // Texture (re)uploads must be broadcast before the FRAME that references
  // their ids, and before any draw command's GetTexID() is called (which
  // asserts a valid TexID) -- draw_protocol::encode_frame() calls GetTexID()
  // per command, so this must run first.
  for (ImTextureData* tex : *draw_data->Textures) {
    switch (tex->Status) {
      case ImTextureStatus_WantCreate: {
        uint32_t id = next_texture_id_++;
        texture_ids_[tex] = id;
        textures_by_id_[id] = tex;
        tex->SetTexID(static_cast<ImTextureID>(id));
        created_this_frame.insert(tex);

        // A texture's very first upload is the common case a browser tab
        // could already have this exact (path, crc32) persisted from an
        // earlier run of the app -- offer the same TEX_CHECK handshake
        // used for reconnects (see pending_sync_ below) to every client
        // already connected, instead of unconditionally paying the full
        // pixel upload. See "Persistent Browser Resource Cache" in
        // src/web/DESIGN.md.
        auto meta_it = texture_meta_.find(tex);
        if (meta_it != texture_meta_.end() && meta_it->second.cacheable) {
          auto check_bytes =
              draw_protocol::encode_texture_check(id, meta_it->second.src, meta_it->second.crc32, *tex);
          auto conns = connected_ids_.rlock();
          auto lock = awaiting_cache_response_.wlock();
          for (ws_connection_id conn_id : *conns) {
            server_->send_to(conn_id, check_bytes);
            (*lock)[conn_id].insert(id);
          }
        } else {
          server_->broadcast(draw_protocol::encode_texture_update(id, *tex));
        }
        tex->SetStatus(ImTextureStatus_OK);
        break;
      }
      case ImTextureStatus_WantUpdates: {
        auto it = texture_ids_.find(tex);
        uint32_t id = it != texture_ids_.end() ? it->second : next_texture_id_++;
        texture_ids_[tex] = id;
        textures_by_id_[id] = tex;
        server_->broadcast(draw_protocol::encode_texture_update(id, *tex));
        tex->SetStatus(ImTextureStatus_OK);
        break;
      }
      case ImTextureStatus_WantDestroy: {
        auto it = texture_ids_.find(tex);
        if (it != texture_ids_.end()) {
          server_->broadcast(draw_protocol::encode_texture_destroy(it->second));
          textures_by_id_.erase(it->second);
          texture_ids_.erase(it);
        }
        tex->SetStatus(ImTextureStatus_Destroyed);
        break;
      }
      default:
        break; // OK / Destroyed: nothing to do this frame.
    }
  }

  // Resolve any CACHE_RESPONSE replies to a TEX_CHECK sent from a previous
  // end_frame()'s pending_sync_ drain (see below). On a miss, the browser
  // needs the full pixel payload it declined to reuse from its own cache;
  // on a hit, nothing further to send -- the browser already built the
  // texture from its cached bytes. Drained before pending_sync_ so a
  // response that arrived in the same tick as a fresh connection is handled
  // in a deterministic order relative to it.
  std::deque<std::pair<ws_connection_id, web_cache_response>> cache_responses;
  {
    auto lock = cache_response_queue_.wlock();
    cache_responses = std::move(*lock);
    lock->clear();
  }
  for (const auto& [conn_id, resp] : cache_responses) {
    {
      auto lock = awaiting_cache_response_.wlock();
      auto conn_it = lock->find(conn_id);
      if (conn_it != lock->end())
        conn_it->second.erase(resp.texture_id);
    }
    if (resp.hit)
      continue;
    auto tex_it = textures_by_id_.find(resp.texture_id);
    if (tex_it == textures_by_id_.end())
      continue; // texture destroyed since the TEX_CHECK was sent
    ImTextureData* tex = tex_it->second;
    // Temporarily present as WantCreate so encode_texture_update() emits a
    // whole-texture payload; restored immediately after -- same technique
    // the pending_sync_ loop below uses.
    ImTextureStatus saved = tex->Status;
    tex->Status = ImTextureStatus_WantCreate;
    auto bytes = draw_protocol::encode_texture_update(resp.texture_id, *tex);
    tex->Status = saved;
    server_->send_to(conn_id, bytes);
  }

  // A newly-connected browser needs the *current* texture state even if
  // nothing changed this frame (Status only transitions once per upload) --
  // resend every live texture as a full upload, to that connection only.
  // Cacheable textures get a TEX_CHECK instead: the browser may already
  // have this exact (path, crc32) persisted from an earlier session, saving
  // the pixel payload entirely.
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
      // Already offered (TEX_CHECK or full TEX_CREATE) to every connected
      // client -- including these -- by the WantCreate handling above.
      if (created_this_frame.contains(tex))
        continue;
      auto it = texture_ids_.find(tex);
      if (it == texture_ids_.end())
        continue;

      auto meta_it = texture_meta_.find(tex);
      if (meta_it != texture_meta_.end() && meta_it->second.cacheable) {
        auto check_bytes =
            draw_protocol::encode_texture_check(it->second, meta_it->second.src, meta_it->second.crc32, *tex);
        auto lock = awaiting_cache_response_.wlock();
        for (ws_connection_id id : pending) {
          server_->send_to(id, check_bytes);
          (*lock)[id].insert(it->second);
        }
        continue;
      }

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

// ── automation ───────────────────────────────────────────────────────────────

#ifdef WISH_AUTOMATION_ENABLED

void web_renderer::render_node(const ui_element& node, const context& s) {
  imgui_renderer::render_node(node, s); // unchanged dispatch/recursion

  // imgui_renderer::render_node() itself early-returns (draws nothing) for
  // a node with visible=false, before reaching the class dispatch table --
  // capturing GetItemRect*() here would then read a stale, unrelated
  // item's state left over from whatever widget rendered before this one.
  if (!node.visible())
    return;

  auto id = node.get_as<bison::key_t>(bison::key_t{"__wish_id"}, bison::key_t{});
  if (id.id == 0)
    return; // node was never assigned an id (e.g. a manually built test tree)

  // last_resolved_rect_min_/max_ is the same rect imgui_renderer::render_node()
  // just resolved for this node -- the group-wrapped bounding box for most
  // classes, or the self-reported Begin()/BeginPopupModal() window rect for
  // Window/DockSpaceViewport -- so a container's captured rect is now its
  // own bounds instead of whatever its last-rendered descendant drew.
  // IsItemHovered/Active/Visible() still read the plain post-dispatch ImGui
  // item state (forwarded correctly across the BeginGroup()/EndGroup() wrap
  // for the group-wrapped case; unaffected either way for Window/
  // DockSpaceViewport, which were never wrapped).
  hit_test_map_[id] = automation::hit_test_entry{
      last_resolved_rect_min_.x,
      last_resolved_rect_min_.y,
      last_resolved_rect_max_.x,
      last_resolved_rect_max_.y,
      ImGui::IsItemHovered(),
      ImGui::IsItemActive(),
      ImGui::IsItemVisible(),
  };
}

void web_renderer::capture_hit_test_for_last_item(const ui_element& node) {
  if (!node.visible())
    return;

  auto id = node.get_as<bison::key_t>(bison::key_t{"__wish_id"}, bison::key_t{});
  if (id.id == 0)
    return; // node was never assigned an id (e.g. a manually built test tree)

  // Unlike render_node()'s hit-test capture, which reads last_resolved_rect_min_/
  // max_ (resolved via render_node()'s own BeginGroup()/EndGroup()-wrap or
  // self-report logic), this reads ImGui's plain post-item state directly --
  // correct here because the caller (render_table(), for a TableRow's
  // row-spanning Selectable) guarantees this runs immediately after drawing
  // the one item that IS this node's whole rect, with no wrap needed.
  hit_test_map_[id] = automation::hit_test_entry{
      ImGui::GetItemRectMin().x,
      ImGui::GetItemRectMin().y,
      ImGui::GetItemRectMax().x,
      ImGui::GetItemRectMax().y,
      ImGui::IsItemHovered(),
      ImGui::IsItemActive(),
      ImGui::IsItemVisible(),
  };
}

namespace {
/// Drains `queries` (a `pending_tree_queries_` snapshot) and sends each a
/// `TREE_SNAPSHOT` reply built against @p ui_objects / @p hits. Shared by
/// both `service_automation_queries()` overloads below.
void answer_tree_queries(
    civetweb_server* server, std::deque<std::pair<ws_connection_id, std::string>>& queries,
    const wish::ui_tree& ui_objects, const automation::hit_test_map& hits) {
  for (const auto& [conn_id, json_payload] : queries) {
    auto req = automation::parse_query_tree_request(json_payload);
    if (!req)
      continue; // malformed QUERY_TREE payload -- drop silently, matches
                // draw_protocol's own "unrecognized messages" convention
    auto snapshot_json = automation::build_tree_snapshot(req->request_id, req->root, ui_objects, hits);
    server->send_to(conn_id, draw_protocol::encode_tree_snapshot(snapshot_json));
  }
}
} // namespace

void web_renderer::service_automation_queries(const context& s) {
  if (!server_)
    return;

  std::deque<std::pair<ws_connection_id, std::string>> tree_queries;
  {
    auto lock = pending_tree_queries_.wlock();
    tree_queries = std::move(*lock);
    lock->clear();
  }
  answer_tree_queries(server_.get(), tree_queries, s.ui_objects, hit_test_map_);

  // Push, not pull: broadcast whatever has been logged since the last call
  // to every connected browser, unprompted, so a script observes log
  // events interleaved with its own actions in the order they happened --
  // see service_automation_queries()'s doc comment.
  if (s.logger_service) {
    auto logs = s.logger_service->recent_logs();
    std::deque<logger::log_entry> new_entries;
    for (auto& e : logs)
      if (e.seq > last_broadcast_log_seq_)
        new_entries.push_back(std::move(e));
    if (!new_entries.empty()) {
      last_broadcast_log_seq_ = new_entries.back().seq;
      server_->broadcast(draw_protocol::encode_log_event(automation::build_log_event(new_entries)));
    }
  }
}

void web_renderer::service_automation_queries() {
  if (!server_)
    return;

  std::deque<std::pair<ws_connection_id, std::string>> tree_queries;
  {
    auto lock = pending_tree_queries_.wlock();
    tree_queries = std::move(*lock);
    lock->clear();
  }
  static const wish::ui_tree kEmptyUiObjects;
  answer_tree_queries(server_.get(), tree_queries, kEmptyUiObjects, hit_test_map_);
}

#endif // WISH_AUTOMATION_ENABLED

// ── texture loading ───────────────────────────────────────────────────────────

ImTextureID web_renderer::get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir,
    const std::unordered_map<std::string, uint32_t>* embedded_crc32s) {
  auto it = loaded_by_src_.find(src);
  if (it != loaded_by_src_.end())
    return it->second ? it->second->TexID : ImTextureID{};

  auto path = resource_dir / src;

  // Read the whole file into memory up front rather than letting stb_image
  // read from disk itself: the raw bytes are needed for the on-the-fly CRC32
  // fallback below, and decoding via stbi_load_from_memory() from the same
  // buffer avoids a second disk read.
  std::ifstream file(path, std::ios::binary);
  std::vector<unsigned char> file_bytes(
      (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (!file || file_bytes.empty()) {
    loaded_by_src_[src] = nullptr;
    return ImTextureID{};
  }

  int w = 0, h = 0, channels = 0;
  unsigned char* pixels =
      stbi_load_from_memory(file_bytes.data(), static_cast<int>(file_bytes.size()), &w, &h, &channels, 4);
  if (!pixels) {
    loaded_by_src_[src] = nullptr;
    return ImTextureID{};
  }

  auto tex = std::make_unique<ImTextureData>();
  tex->Create(ImTextureFormat_RGBA32, w, h);
  std::memcpy(tex->GetPixels(), pixels, static_cast<size_t>(tex->GetSizeInBytes()));
  tex->UseColors = true;
  stbi_image_free(pixels);

  // Content-version CRC32: reuse the embedded zip's own per-file CRC32 when
  // `src` is a known embedded asset (no need to hash bytes we already trust
  // miniz to have hashed); otherwise (session-uploaded files, which never
  // carry a precomputed checksum -- see file_service::upload()) compute it
  // from the bytes just read.
  auto rel = path.lexically_relative(resource_dir).generic_string();
  uint32_t crc32 = 0;
  bool has_precomputed_crc = false;
  if (embedded_crc32s) {
    if (auto crc_it = embedded_crc32s->find(rel); crc_it != embedded_crc32s->end()) {
      crc32 = crc_it->second;
      has_precomputed_crc = true;
    }
  }
  if (!has_precomputed_crc)
    crc32 = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, file_bytes.data(), file_bytes.size()));

  // Content under a "private/" prefix (relative to resource_dir) may hold
  // personal data (e.g. uploaded photos) and must never be offered to the
  // browser's persistent resource cache -- see context.hpp's resource_dir
  // doc comment and src/web/DESIGN.md.
  std::filesystem::path rel_path{rel};
  bool cacheable = rel_path.empty() || rel_path.begin()->string() != "private";

  ImTextureData* raw = tex.get();
  ImGui::RegisterUserTexture(raw);
  loaded_textures_.push_back(std::move(tex));
  loaded_by_src_[src] = raw;
  texture_meta_[raw] = texture_meta{rel, crc32, cacheable};

  // No id yet -- end_frame() assigns one this frame when it walks
  // ImDrawData::Textures and sees this texture's WantCreate status (see the
  // doc comment on get_or_load_texture() in web_renderer.hpp).
  return raw->TexID;
}

std::optional<web_renderer::texture_meta> web_renderer::texture_meta_for_test(const std::string& src) const {
  auto it = loaded_by_src_.find(src);
  if (it == loaded_by_src_.end() || !it->second)
    return std::nullopt;
  auto meta_it = texture_meta_.find(it->second);
  if (meta_it == texture_meta_.end())
    return std::nullopt;
  return meta_it->second;
}

// ── font loading ─────────────────────────────────────────────────────────────

ImFont* web_renderer::get_or_load_font(const std::string& path, float size) {
  FontKey key{path, size};
  auto it = font_cache_.find(key);
  if (it != font_cache_.end())
    return it->second;

  // Cache miss: record the key now (even for a missing file) so a bad path
  // isn't retried every frame.
  ImFont* loaded = std::filesystem::exists(path)
                        ? ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size)
                        : nullptr;
  font_cache_[key] = loaded;
  return nullptr; // default font used this frame; see doc comment in the header
}

// ── offscreen render target ───────────────────────────────────────────────────

ImTextureID web_renderer::begin_render_target(int w, int h) {
  if (w <= 0 || h <= 0)
    return ImTextureID{};

  if (render_target_id_ == 0 || render_target_w_ != w || render_target_h_ != h) {
    if (render_target_id_ != 0 && server_)
      server_->broadcast(draw_protocol::encode_texture_destroy(render_target_id_));
    render_target_id_ = next_texture_id_++;
    render_target_w_ = w;
    render_target_h_ = h;
  }

  saved_render_target_id_ = current_target_id_;
  current_target_id_ = render_target_id_;
  return static_cast<ImTextureID>(render_target_id_);
}

void web_renderer::end_render_target() {
  current_target_id_ = saved_render_target_id_;
  saved_render_target_id_ = 0;
}

void web_renderer::flush_draw_list(ImDrawList& draw_list, int w, int h) {
  if (w <= 0 || h <= 0 || !server_)
    return;

  ImDrawData draw_data;
  draw_data.DisplayPos = ImVec2(0.0f, 0.0f);
  draw_data.DisplaySize = ImVec2(float(w), float(h));
  draw_data.FramebufferScale = ImVec2(1.0f, 1.0f);
  draw_data.AddDrawList(&draw_list);
  // No dynamic texture updates to service for this one-off submission --
  // every texture drawn into `draw_list` (scene textures, fonts) was
  // already uploaded earlier this same frame via get_or_load_texture()/the
  // normal font atlas path.
  draw_data.Textures = nullptr;
  draw_data.Valid = true;

  server_->broadcast(draw_protocol::encode_frame(draw_data, current_target_id_));
}

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
