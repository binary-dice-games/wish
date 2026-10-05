// MIT License © 2026 Binary Dice Games
/// @file nymph_figure_renderer.cpp
/// @brief Implementation of nymph's offscreen rasterizer.
#include "nymph_figure_renderer.hpp"

#include <stdexcept>

#if defined(WISH_SDL3_ENABLED)

#include <imgui/imgui_renderer.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>
#include <implot.h>
#include <implot3d.h>

#include <cfloat>
#include <cstring>

namespace bdg::wish::nymph {

namespace {

// ImPlot fits its axes, and wish's measure/arrange pass settles, one frame
// after the content first appears; the third frame is the stable one.
constexpr int kFrames = 3;
constexpr float kFontSize = 16.0f; // the wish renderers' default UI font size

/// Owns everything one render needs and puts the calling thread's ImGui,
/// ImPlot and ImPlot3D contexts back when it goes out of scope.
class offscreen_scope {
 public:
  offscreen_scope(int pixel_width, int pixel_height)
      : prev_imgui_(ImGui::GetCurrentContext()), prev_implot_(ImPlot::GetCurrentContext()),
        prev_implot3d_(ImPlot3D::GetCurrentContext()) {
    surface_ = SDL_CreateSurface(pixel_width, pixel_height, SDL_PIXELFORMAT_RGBA32);
    if (!surface_)
      fail("SDL_CreateSurface");
    sdl_ = SDL_CreateSoftwareRenderer(surface_);
    if (!sdl_)
      fail("SDL_CreateSoftwareRenderer");

    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    implot_ = ImPlot::CreateContext();
    ImPlot::SetCurrentContext(implot_);
    implot3d_ = ImPlot3D::CreateContext();
    ImPlot3D::SetCurrentContext(implot3d_);
    backend_ = ImGui_ImplSDLRenderer3_Init(sdl_);
    if (!backend_)
      fail("ImGui_ImplSDLRenderer3_Init");
  }

  ~offscreen_scope() { release(); }

  offscreen_scope(const offscreen_scope&) = delete;
  offscreen_scope& operator=(const offscreen_scope&) = delete;

  SDL_Renderer* sdl() const { return sdl_; }
  SDL_Surface* surface() const { return surface_; }

 private:
  [[noreturn]] void fail(const char* what) {
    std::string message = std::string("nymph: ") + what + " failed: " + SDL_GetError();
    release();
    throw std::runtime_error(message);
  }

  void release() {
    if (imgui_) {
      ImGui::SetCurrentContext(imgui_);
      if (backend_)
        ImGui_ImplSDLRenderer3_Shutdown();
      if (implot3d_)
        ImPlot3D::DestroyContext(implot3d_);
      if (implot_)
        ImPlot::DestroyContext(implot_);
      ImGui::DestroyContext(imgui_);
      imgui_ = nullptr;
      implot_ = nullptr;
      implot3d_ = nullptr;
      backend_ = false;
    }
    if (sdl_) {
      SDL_DestroyRenderer(sdl_);
      sdl_ = nullptr;
    }
    if (surface_) {
      SDL_DestroySurface(surface_);
      surface_ = nullptr;
    }
    ImGui::SetCurrentContext(prev_imgui_);
    ImPlot::SetCurrentContext(prev_implot_);
    ImPlot3D::SetCurrentContext(prev_implot3d_);
  }

  ImGuiContext* prev_imgui_;
  ImPlotContext* prev_implot_;
  ImPlot3DContext* prev_implot3d_;
  SDL_Surface* surface_{nullptr};
  SDL_Renderer* sdl_{nullptr};
  ImGuiContext* imgui_{nullptr};
  ImPlotContext* implot_{nullptr};
  ImPlot3DContext* implot3d_{nullptr};
  bool backend_{false};
};

void check_options(const image_options& o) {
  if (o.width < 16 || o.width > 4096 || o.height < 16 || o.height > 4096)
    throw std::runtime_error("nymph: image width and height must be between 16 and 4096");
  if (o.scale < 1 || o.scale > 4)
    throw std::runtime_error("nymph: image scale must be between 1 and 4");
  if (o.width * o.scale > 8192 || o.height * o.scale > 8192)
    throw std::runtime_error("nymph: image width and height times scale must not exceed 8192");
  if (o.padding < 0 || o.padding * 2 >= o.width || o.padding * 2 >= o.height)
    throw std::runtime_error("nymph: image padding does not fit the image size");
}

} // namespace

image render_figure(const ui_element& root, const context& s, const image_options& options) {
  check_options(options);
  theme_fn theme = find_theme(options.theme);
  if (!theme)
    throw std::runtime_error("nymph: unknown theme '" + options.theme + "'");

  const int pixel_width = options.width * options.scale;
  const int pixel_height = options.height * options.scale;
  offscreen_scope scope(pixel_width, pixel_height);
  // The backend leaves vertex positions in logical units and expects the
  // SDL renderer itself to carry the pixel scale (as imgui's own SDL3
  // example does); setting only DisplayFramebufferScale draws the image at
  // 1x into a corner of the larger surface.
  SDL_SetRenderScale(scope.sdl(), static_cast<float>(options.scale), static_cast<float>(options.scale));

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  theme(&ImGui::GetStyle());
  ImGui::GetStyle().FontSizeBase = kFontSize;
  // ImGui draws anti-aliased lines by sampling a small texture, which only
  // looks right with the bilinear filtering a GPU does. The software
  // renderer samples it unevenly (lines come out with gaps and uneven
  // widths), so use ImGui's geometry-only anti-aliasing instead.
  ImGui::GetStyle().AntiAliasedLinesUseTex = false;

  const ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
  imgui_renderer renderer;
  const float pad = static_cast<float>(options.padding);
  const ImVec2 size(static_cast<float>(options.width), static_cast<float>(options.height));

  for (int frame = 0; frame < kFrames; ++frame) {
    // The same inputs every frame and every call: no clock, no mouse.
    io.DisplaySize = size;
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(options.scale), static_cast<float>(options.scale));
    io.DeltaTime = 1.0f / 60.0f;
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

    ImGui_ImplSDLRenderer3_NewFrame();
    renderer.begin_frame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##nymph_figure", nullptr, flags);
    renderer.render_node(root, s);
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::Render();

    // Every frame is submitted, not only the last: the backend creates and
    // updates the font atlas texture while it draws.
    SDL_SetRenderDrawColorFloat(scope.sdl(), background.x, background.y, background.z, 1.0f);
    SDL_RenderClear(scope.sdl());
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), scope.sdl());
    SDL_FlushRenderer(scope.sdl());
  }

  image out;
  out.width = pixel_width;
  out.height = pixel_height;
  out.rgba.resize(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height) * 4);
  SDL_Surface* surface = scope.surface();
  const size_t row_bytes = static_cast<size_t>(pixel_width) * 4;
  for (int y = 0; y < pixel_height; ++y) {
    const auto* src = static_cast<const uint8_t*>(surface->pixels) + static_cast<size_t>(y) * surface->pitch;
    uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * row_bytes;
    std::memcpy(dst, src, row_bytes);
    for (size_t x = 3; x < row_bytes; x += 4)
      dst[x] = 255; // the image is opaque; blending leaves arbitrary alpha behind
  }
  return out;
}

} // namespace bdg::wish::nymph

#else // !WISH_SDL3_ENABLED

namespace bdg::wish::nymph {

image render_figure(const ui_element&, const context&, const image_options&) {
  throw std::runtime_error("nymph: this build cannot render images (wish was built with WISH_ENABLE_SDL3=OFF)");
}

} // namespace bdg::wish::nymph

#endif
