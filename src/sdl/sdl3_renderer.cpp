// MIT License © 2025 Binary Dice Games
/// @file sdl3_renderer.cpp
/// @brief Implementation of wish::sdl3_renderer.
#include <sdl/sdl3_renderer.hpp>

#ifdef WISH_SDL3_ENABLED

#include <SDL3_image/SDL_image.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <implot.h>
#include <implot3d.h>

#ifdef WISH_AUTOMATION_ENABLED
// stb_image_write is vendored solely for capture_frame_png() -- mirrors
// web_renderer.cpp's own STB_IMAGE_IMPLEMENTATION/<stb_image.h> pattern for
// the read side (see that file's comment on civetweb/stb_image isolation).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#endif

#include <stdexcept>
#include <utility>

namespace bdg::wish {

// ── construction ──────────────────────────────────────────────────────────────

sdl3_renderer::sdl3_renderer(const char* title, int width, int height, int font_size, render_fn_map extra_render_fns)
    : imgui_renderer(std::move(extra_render_fns)), title_(title), width_(width), height_(height),
      font_size_(font_size) {}

sdl3_renderer::~sdl3_renderer() {
  // teardown() is called from the render thread before the render loop exits.
  // If it was never called (e.g. setup() threw), SDL objects are still null
  // and nothing needs freeing here.
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

void sdl3_renderer::setup() {
  if (!SDL_Init(SDL_INIT_VIDEO))
    throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

  window_ = SDL_CreateWindow(title_, width_, height_, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) {
    SDL_Quit();
    throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
  }

  // See display_scale_'s doc comment in the header: SDL3 on Windows doesn't
  // separate window "points" from physical pixels, so this is the only
  // signal we get for the OS Display Scale setting.
  display_scale_ = SDL_GetWindowDisplayScale(window_);
  if (display_scale_ <= 0.0f)
    display_scale_ = 1.0f;

  sdl_renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!sdl_renderer_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
    throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
  }

  SDL_SetRenderVSync(sdl_renderer_, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImPlot3D::CreateContext();
  ImGui::StyleColorsDark();
  // Compensate for SDL3's lack of Windows content-scale normalization (see
  // display_scale_'s doc comment) by scaling padding/spacing/rounding etc.
  // to match what a DPI-normalized backend (like web_renderer) renders at
  // the same logical width/height. Font size is scaled separately in
  // rebuild_font_atlas(), since ScaleAllSizes() doesn't touch fonts.
  ImGui::GetStyle().ScaleAllSizes(display_scale_);

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigDockingWithShift = true;

  // Anchor imgui.ini next to the executable rather than letting ImGui use
  // its CWD-relative default -- launching the app from a different working
  // directory would otherwise silently start a fresh, empty layout file.
  if (const char* base_path = SDL_GetBasePath())
    ini_path_ = std::string(base_path) + "imgui.ini";
  else
    ini_path_ = "imgui.ini";
  io.IniFilename = ini_path_.c_str();

  ImGui_ImplSDL3_InitForSDLRenderer(window_, sdl_renderer_);
  ImGui_ImplSDLRenderer3_Init(sdl_renderer_);

  // Build font atlas — required before the first NewFrame.
  unsigned char* pixels;
  int fw, fh;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
}

void sdl3_renderer::teardown() {
  // Free any textures loaded by get_or_load_texture.
  for (auto& [key, id] : texture_cache_) {
    if (id)
      SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(id));
  }
  texture_cache_.clear();

  if (render_target_) {
    SDL_DestroyTexture(render_target_);
    render_target_ = nullptr;
    render_target_w_ = 0;
    render_target_h_ = 0;
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImPlot3D::DestroyContext();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();

  if (sdl_renderer_) {
    SDL_DestroyRenderer(sdl_renderer_);
    sdl_renderer_ = nullptr;
  }
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  SDL_Quit();
}

// ── per-frame ─────────────────────────────────────────────────────────────────

bool sdl3_renderer::mouse_motion_significant(float x, float y) {
  static constexpr float kMotionPixelThreshold = 4.0f;
  static constexpr auto kMotionTimeThreshold = std::chrono::milliseconds{100};

  auto now = std::chrono::steady_clock::now();
  float dx = x - last_motion_x_;
  float dy = y - last_motion_y_;
  bool far_enough = dx * dx + dy * dy >= kMotionPixelThreshold * kMotionPixelThreshold;
  bool long_enough = now - last_motion_time_ >= kMotionTimeThreshold;

  if (has_motion_baseline_ && !far_enough && !long_enough)
    return false;

  last_motion_x_ = x;
  last_motion_y_ = y;
  last_motion_time_ = now;
  has_motion_baseline_ = true;
  return true;
}

bool sdl3_renderer::poll_events() {
  bool activity = false;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_QUIT)
      quit_.store(true, std::memory_order_release);

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      if (mouse_motion_significant(event.motion.x, event.motion.y))
        activity = true;
      continue;
    }

    activity = true;
  }
  return activity || fonts_dirty_ || quit_.load(std::memory_order_acquire);
}

void sdl3_renderer::begin_frame() {
  if (fonts_dirty_)
    rebuild_font_atlas();

#ifdef WISH_AUTOMATION_ENABLED
  // hit_test_map_ is cleared here (not in end_frame()) so a query answers
  // against a complete frame -- mirrors web_renderer::begin_frame().
  hit_test_map_.clear();

  // Drain synthetic text input queued by inject_text() and feed it to ImGui
  // directly, before NewFrame() -- see pending_text_inputs_'s doc comment.
  {
    std::deque<std::string> texts;
    {
      auto lock = pending_text_inputs_.wlock();
      texts = std::move(*lock);
      lock->clear();
    }
    if (!texts.empty()) {
      ImGuiIO& io = ImGui::GetIO();
      for (const auto& t : texts)
        io.AddInputCharactersUTF8(t.c_str());
    }
  }
#endif

  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  imgui_renderer::begin_frame(); // → ImGui::NewFrame()
}

void sdl3_renderer::end_frame() {
  // ImGui::Render() subsumes EndFrame() — do NOT call imgui_renderer::end_frame().
  ImGui::Render();

  SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 30, 255);
  SDL_RenderClear(sdl_renderer_);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), sdl_renderer_);

#ifdef WISH_AUTOMATION_ENABLED
  // Service any pending screenshot requests here -- after draw data has been
  // submitted to sdl_renderer_ but before SDL_RenderPresent(), the last
  // point at which SDL_RenderReadPixels() still sees this frame's pixels.
  {
    std::deque<std::promise<std::vector<uint8_t>>> pending;
    {
      auto lock = pending_screenshots_.wlock();
      pending = std::move(*lock);
      lock->clear();
    }
    if (!pending.empty()) {
      auto png = capture_frame_png();
      for (auto& p : pending)
        p.set_value(png);
    }
  }
#endif

  SDL_RenderPresent(sdl_renderer_);
}

bool sdl3_renderer::should_quit() const {
  return quit_.load(std::memory_order_acquire);
}

void sdl3_renderer::request_quit() {
  quit_.store(true, std::memory_order_release);
}

// ── font loading ──────────────────────────────────────────────────────────────

ImFont* sdl3_renderer::get_or_load_font(const std::string& path, float size) {
  FontKey key{path, size};
  auto it = font_cache_.find(key);
  if (it != font_cache_.end())
    return it->second;
  // Cache miss: record the key (even for missing files) so we don't keep
  // scheduling rebuilds every frame for the same bad path.
  font_cache_[key] = nullptr;
  if (std::filesystem::exists(path)) {
    pending_fonts_.insert(key);
    fonts_dirty_ = true;
  }
  return nullptr; // default font used this frame
}

void sdl3_renderer::rebuild_font_atlas() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.FontSizeBase = font_size_ * display_scale_;

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  io.Fonts->AddFontDefaultVector();

  // Re-add all previously loaded fonts (ImFont* pointers must be refreshed).
  // Cache keys stay in the caller's logical `size`; only the actual
  // rasterized size is scaled, so an explicit widget font size (e.g. from
  // ui_element::font_size()) matches the same display_scale_ compensation
  // as the default font above.
  for (auto& [key, ptr] : font_cache_) {
    ptr = std::filesystem::exists(key.path)
              ? io.Fonts->AddFontFromFileTTF(key.path.c_str(), key.size * display_scale_)
              : nullptr;
  }

  // Add newly requested fonts, skipping any whose file cannot be found.
  for (const auto& key : pending_fonts_) {
    font_cache_[key] = std::filesystem::exists(key.path)
                            ? io.Fonts->AddFontFromFileTTF(key.path.c_str(), key.size * display_scale_)
                            : nullptr;
  }
  pending_fonts_.clear();

  io.Fonts->Build();
  ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
  ImGui_ImplSDLRenderer3_CreateDeviceObjects();
  fonts_dirty_ = false;
}

// ── texture loading ───────────────────────────────────────────────────────────

ImTextureID sdl3_renderer::get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir,
    const std::unordered_map<std::string, uint32_t>* embedded_crc32s) {
  (void)embedded_crc32s; // no browser resource cache to version here
  auto it = texture_cache_.find(src);
  if (it != texture_cache_.end())
    return it->second;

  auto path = resource_dir / src;

  SDL_Surface* surf = IMG_Load(path.string().c_str());
  if (!surf) {
    texture_cache_[src] = ImTextureID{};
    return ImTextureID{};
  }

  SDL_Texture* tex = SDL_CreateTextureFromSurface(sdl_renderer_, surf);
  SDL_DestroySurface(surf);

  ImTextureID id = tex ? reinterpret_cast<ImTextureID>(tex) : ImTextureID{};
  texture_cache_[src] = id;
  return id;
}

// ── offscreen render target ────────────────────────────────────────────────────

ImTextureID sdl3_renderer::begin_render_target(int w, int h) {
  if (w <= 0 || h <= 0)
    return ImTextureID{};

  if (!render_target_ || render_target_w_ != w || render_target_h_ != h) {
    if (render_target_)
      SDL_DestroyTexture(render_target_);
    render_target_ = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    render_target_w_ = w;
    render_target_h_ = h;
    if (!render_target_) {
      render_target_w_ = 0;
      render_target_h_ = 0;
      return ImTextureID{};
    }
  }

  saved_render_target_ = SDL_GetRenderTarget(sdl_renderer_);
  SDL_SetRenderTarget(sdl_renderer_, render_target_);
  return reinterpret_cast<ImTextureID>(render_target_);
}

void sdl3_renderer::end_render_target() {
  SDL_SetRenderTarget(sdl_renderer_, saved_render_target_);
  saved_render_target_ = nullptr;
}

void sdl3_renderer::flush_draw_list(ImDrawList& draw_list, int w, int h) {
  if (w <= 0 || h <= 0)
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

  ImGui_ImplSDLRenderer3_RenderDrawData(&draw_data, sdl_renderer_);
}

// ── automation ───────────────────────────────────────────────────────────────

#ifdef WISH_AUTOMATION_ENABLED

void sdl3_renderer::render_node(const ui_element& node, const context& s) {
  imgui_renderer::render_node(node, s); // unchanged dispatch/recursion

  // imgui_renderer::render_node() itself early-returns (draws nothing) for
  // a node with visible=false, before reaching the class dispatch table --
  // capturing GetItemRect*() here would then read a stale, unrelated item's
  // state left over from whatever widget rendered before this one. Mirrors
  // web_renderer::render_node()'s identical guard.
  if (!node.get_as<bool>(bison::key_t{"visible"}, true))
    return;

  auto id = node.get_as<bison::key_t>(bison::key_t{"__wish_id"}, bison::key_t{});
  if (id.id == 0)
    return; // node was never assigned an id (e.g. a manually built test tree)

  // last_resolved_rect_min_/max_ is the same rect imgui_renderer::render_node()
  // just resolved for this node -- see that function's own comments for the
  // per-class resolution rules (Window/DockSpaceViewport self-report,
  // container classes are BeginGroup()/EndGroup()-wrapped, leaves read the
  // plain post-dispatch item rect).
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

void sdl3_renderer::capture_hit_test_for_last_item(const ui_element& node) {
  if (!node.get_as<bool>(bison::key_t{"visible"}, true))
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

void sdl3_renderer::service_automation_queries(const context& s) {
  std::deque<pending_tree_query> queries;
  {
    auto lock = pending_tree_queries_.wlock();
    queries = std::move(*lock);
    lock->clear();
  }
  for (auto& q : queries) {
    auto json = automation::build_tree_snapshot(q.request_id, q.root, s.ui_objects, hit_test_map_);
    q.reply.set_value(std::move(json));
  }
}

std::future<std::string> sdl3_renderer::query_tree(uint32_t request_id, const std::string& root) {
  pending_tree_query q{request_id, root, {}};
  auto fut = q.reply.get_future();
  pending_tree_queries_.wlock()->push_back(std::move(q));
  return fut;
}

std::future<std::vector<uint8_t>> sdl3_renderer::capture_screenshot() {
  std::promise<std::vector<uint8_t>> promise;
  auto fut = promise.get_future();
  pending_screenshots_.wlock()->push_back(std::move(promise));
  return fut;
}

std::vector<uint8_t> sdl3_renderer::capture_frame_png() {
  SDL_Surface* surf = SDL_RenderReadPixels(sdl_renderer_, nullptr);
  if (!surf)
    return {};

  // Normalize to a predictable, tightly-packed RGBA8888 layout regardless of
  // whatever pixel format SDL_RenderReadPixels happened to return, so
  // stbi_write_png_to_func can be called with a fixed comp/stride.
  SDL_Surface* converted = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(surf);
  if (!converted)
    return {};

  std::vector<uint8_t> png_bytes;
  auto write_cb = [](void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* bytes = static_cast<uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
  };
  stbi_write_png_to_func(write_cb, &png_bytes, converted->w, converted->h, 4, converted->pixels, converted->pitch);
  SDL_DestroySurface(converted);
  return png_bytes;
}

void sdl3_renderer::inject_mouse_move(float x, float y) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.windowID = window_ ? SDL_GetWindowID(window_) : 0;
  event.motion.x = x;
  event.motion.y = y;
  SDL_PushEvent(&event);
}

void sdl3_renderer::inject_mouse_button(int button, bool down) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.windowID = window_ ? SDL_GetWindowID(window_) : 0;
  // 0 = left, 1 = right, 2 = middle -- mirrors SDL_BUTTON_LEFT/RIGHT/MIDDLE's
  // own numbering (see automation::automation_backend::inject_mouse_button's
  // doc comment).
  event.button.button = static_cast<Uint8>(
      button == 0 ? SDL_BUTTON_LEFT : (button == 1 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE));
  event.button.down = down;
  event.button.clicks = 1;
  SDL_PushEvent(&event);
}

void sdl3_renderer::inject_key(int keycode, bool down) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.windowID = window_ ? SDL_GetWindowID(window_) : 0;
  event.key.key = static_cast<SDL_Keycode>(keycode);
  event.key.down = down;
  SDL_PushEvent(&event);
}

void sdl3_renderer::inject_text(const std::string& utf8) {
  pending_text_inputs_.wlock()->push_back(utf8);
}

#endif // WISH_AUTOMATION_ENABLED

} // namespace bdg::wish

#endif // WISH_SDL3_ENABLED
