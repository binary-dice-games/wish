// MIT License © 2025 Binary Dice Games
/// @file sdl3_renderer.hpp
/// @brief SDL3-backed concrete renderer: opens a real window and draws Dear
///        ImGui via the SDL3 renderer backend.
#pragma once

#ifdef WISH_SDL3_ENABLED

#include <imgui/imgui_renderer.hpp>

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace bdg::wish {

/**
 * @brief Windowed renderer built on SDL3 + Dear ImGui SDL3 backend.
 *
 * Lifecycle (all methods called from the render thread, never the
 * constructor/destructor thread):
 *
 *   setup()         — creates SDL window, SDL renderer, ImGui context + backends
 *   N × begin_frame / render_node / end_frame
 *   teardown()      — shuts down ImGui backends, destroys SDL objects
 *
 * The window close event sets the `should_quit()` flag, which causes
 * `wish::server::render_loop` to exit gracefully.
 *
 * Texture loading: BMP files are loaded via `SDL_LoadBMP`; PNG would require
 * SDL3_image (out of scope).  Loaded textures are cached by filename and
 * freed in `teardown()`.
 */
class sdl3_renderer : public imgui_renderer {
 public:
  /**
   * @brief Construct with window parameters.
   *
   * Does **not** initialize SDL or create a window — that happens in
   * `setup()`, which is called from the render thread.
   *
   * @param title   Window title string (must outlive this object).
   * @param width   Initial window width in pixels.
   * @param height  Initial window height in pixels.
   */
  explicit sdl3_renderer(const char* title = "wish", int width = 1280, int height = 720);

  ~sdl3_renderer() override;

  // ── renderer lifecycle ────────────────────────────────────────────────────

  /// @brief Initialize SDL3, create window + renderer, set up ImGui backends.
  void setup() override;

  /// @brief Shut down ImGui backends, destroy SDL objects, free textures.
  void teardown() override;

  // ── per-frame ─────────────────────────────────────────────────────────────

  /// @brief Drain pending SDL events into ImGui and report whether they (or
  ///        a pending font atlas rebuild) require a frame to be drawn.
  bool poll_events() override;

  /// @brief Call ImGui backend new-frame helpers, then forward to
  ///        `imgui_renderer::begin_frame()` (ImGui::NewFrame).
  void begin_frame() override;

  /// @brief Render ImGui draw data to the SDL renderer and present.
  ///        Calls `ImGui::Render()` instead of the parent's `EndFrame()`.
  void end_frame() override;

  /// @brief Returns true once the user closes the window or quit is requested.
  bool should_quit() const override;

  /// @brief Programmatically request shutdown (e.g. from a menu action).
  void request_quit();

  // ── texture loading ───────────────────────────────────────────────────────

  /**
   * @brief Load a BMP image from `resource_dir / src` and return its SDL
   *        texture handle as an `ImTextureID`.
   *
   * Results are cached by filename; the texture is freed in `teardown()`.
   * Returns a null handle (zero) if the file cannot be loaded.
   *
   * @param src           Filename relative to the session resource directory.
   * @param resource_dir  Session-scoped resource folder.
   */
  ImTextureID get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir) override;

  /**
   * @brief Return a cached ImFont* for (path, size), or schedule an atlas
   *        rebuild and return nullptr (default font) on the first request.
   *
   * The atlas is rebuilt at the start of the next frame; from that point on,
   * subsequent calls return the loaded font pointer.
   *
   * @param path  Absolute path to a TTF font file.
   * @param size  Font size in pixels.
   */
  ImFont* get_or_load_font(const std::string& path, float size) override;

 private:
  const char* title_;
  int width_;
  int height_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* sdl_renderer_ = nullptr;
  std::atomic<bool> quit_{false};

  // ── Font cache ─────────────────────────────────────────────────────────────

  struct FontKey {
    std::string path;
    float size{0.0f};
    bool operator<(const FontKey& o) const {
      if (path != o.path)
        return path < o.path;
      return size < o.size;
    }
  };

  std::map<FontKey, ImFont*> font_cache_;
  std::set<FontKey> pending_fonts_;
  bool fonts_dirty_{false};

  /// @brief Clear and rebuild the ImGui font atlas, then re-upload to the GPU.
  void rebuild_font_atlas();
};

} // namespace bdg::wish

#endif // WISH_SDL3_ENABLED
