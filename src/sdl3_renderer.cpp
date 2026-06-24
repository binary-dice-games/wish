// MIT License © 2025 Binary Dice Games
/// @file sdl3_renderer.cpp
/// @brief Implementation of wish::sdl3_renderer.
#include <wish/sdl3_renderer.hpp>

#ifdef WISH_SDL3_ENABLED

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <implot.h>
#include <implot3d.h>
#include <SDL3_image/SDL_image.h>

#include <stdexcept>

namespace bdg::wish {

// ── construction ──────────────────────────────────────────────────────────────

sdl3_renderer::sdl3_renderer(const char* title, int width, int height)
    : title_(title), width_(width), height_(height) {}

sdl3_renderer::~sdl3_renderer() {
  // teardown() is called from the render thread before the render loop exits.
  // If it was never called (e.g. setup() threw), SDL objects are still null
  // and nothing needs freeing here.
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

void sdl3_renderer::setup() {
  if (!SDL_Init(SDL_INIT_VIDEO))
    throw std::runtime_error(
        std::string("SDL_Init failed: ") + SDL_GetError());

  window_ = SDL_CreateWindow(
      title_, width_, height_, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) {
    SDL_Quit();
    throw std::runtime_error(
        std::string("SDL_CreateWindow failed: ") + SDL_GetError());
  }

  sdl_renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!sdl_renderer_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
    throw std::runtime_error(
        std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
  }

  SDL_SetRenderVSync(sdl_renderer_, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImPlot3D::CreateContext();
  ImGui::StyleColorsDark();

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

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

void sdl3_renderer::begin_frame() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_QUIT)
      quit_.store(true, std::memory_order_release);
  }

  if (fonts_dirty_) rebuild_font_atlas();

  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  imgui_renderer::begin_frame();  // → ImGui::NewFrame()
}

void sdl3_renderer::end_frame() {
  // ImGui::Render() subsumes EndFrame() — do NOT call imgui_renderer::end_frame().
  ImGui::Render();

  SDL_SetRenderDrawColor(sdl_renderer_, 30, 30, 30, 255);
  SDL_RenderClear(sdl_renderer_);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), sdl_renderer_);
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
  if (it != font_cache_.end()) return it->second;
  // Cache miss: record the key (even for missing files) so we don't keep
  // scheduling rebuilds every frame for the same bad path.
  font_cache_[key] = nullptr;
  if (std::filesystem::exists(path)) {
    pending_fonts_.insert(key);
    fonts_dirty_ = true;
  }
  return nullptr;  // default font used this frame
}

void sdl3_renderer::rebuild_font_atlas() {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  io.Fonts->AddFontDefault();

  // Re-add all previously loaded fonts (ImFont* pointers must be refreshed).
  for (auto& [key, ptr] : font_cache_) {
    ptr = std::filesystem::exists(key.path)
        ? io.Fonts->AddFontFromFileTTF(key.path.c_str(), key.size)
        : nullptr;
  }

  // Add newly requested fonts, skipping any whose file cannot be found.
  for (const auto& key : pending_fonts_) {
    font_cache_[key] = std::filesystem::exists(key.path)
        ? io.Fonts->AddFontFromFileTTF(key.path.c_str(), key.size)
        : nullptr;
  }
  pending_fonts_.clear();

  io.Fonts->Build();
  ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
  ImGui_ImplSDLRenderer3_CreateDeviceObjects();
  fonts_dirty_ = false;
}

// ── texture loading ───────────────────────────────────────────────────────────

ImTextureID sdl3_renderer::get_or_load_texture(
    const std::string&           src,
    const std::filesystem::path& resource_dir) {
  auto it = texture_cache_.find(src);
  if (it != texture_cache_.end()) return it->second;

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

}  // namespace bdg::wish

#endif  // WISH_SDL3_ENABLED
