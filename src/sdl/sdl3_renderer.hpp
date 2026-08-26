// MIT License © 2025 Binary Dice Games
/// @file sdl3_renderer.hpp
/// @brief SDL3-backed concrete renderer: opens a real window and draws Dear
///        ImGui via the SDL3 renderer backend.
#pragma once

#ifdef WISH_SDL3_ENABLED

#include <imgui/imgui_renderer.hpp>

#include <SDL3/SDL.h>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_backend.hpp>
#include <automation/automation_query.hpp>
#include "src/bison/bison_sync.hpp"
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#ifdef WISH_AUTOMATION_ENABLED
#include <deque>
#include <future>
#include <vector>
#endif

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
class sdl3_renderer : public imgui_renderer
#ifdef WISH_AUTOMATION_ENABLED
    ,
                       public automation::automation_backend
#endif
{
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
   * @param font_size  Base font size in pixels.
   * @param extra_render_fns  Extra/override `render_*` dispatch entries, see
   *                          `imgui_renderer::imgui_renderer()`.
   */
  explicit sdl3_renderer(const char* title = "wish", int width = 1280, int height = 720, int font_size = 16,
      render_fn_map extra_render_fns = {});

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
   *
   * @p embedded_crc32s is accepted for interface compatibility but unused —
   * this backend draws to a native window, so there is no browser resource
   * cache to version by content hash.
   */
  ImTextureID get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir,
      const std::unordered_map<std::string, uint32_t>* embedded_crc32s = nullptr) override;

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

  // ── offscreen render target ──────────────────────────────────────────────

  /**
   * @brief Create (or reuse, if @p w x @p h matches the current one) an
   *        `SDL_TEXTUREACCESS_TARGET` texture and redirect rendering to it.
   *
   * The previously active render target (typically the window's own
   * backbuffer) is saved and restored by `end_render_target()`.
   */
  ImTextureID begin_render_target(int w, int h) override;

  /// @brief Restore the render target saved by `begin_render_target()`.
  void end_render_target() override;

  /**
   * @brief Immediately submits @p draw_list to whatever `SDL_Renderer`
   *        target is currently active, via `ImGui_ImplSDLRenderer3_RenderDrawData()`.
   *
   * Unlike the normal per-frame flush (one combined `ImGui_ImplSDLRenderer3_
   * RenderDrawData()` call over every window's accumulated draw list at
   * `end_frame()`), this wraps just @p draw_list in its own throwaway
   * `ImDrawData` and submits it right away -- so a caller sandwiching this
   * between `begin_render_target()`/`end_render_target()` gets @p draw_list
   * rendered onto the offscreen texture specifically, not the window
   * backbuffer everything else eventually lands on.
   */
  void flush_draw_list(ImDrawList& draw_list, int w, int h) override;

#ifdef WISH_AUTOMATION_ENABLED
  // ── automation ────────────────────────────────────────────────────────────

  /// @brief Draw @p node, then (in addition to the base dispatch) capture its
  ///        resolved screen rect and interaction flags into `hit_test_map_`.
  ///        Mirrors `web_renderer::render_node()` -- see
  ///        `src/automation/DESIGN.md`'s "Native (ABI-based) automation".
  void render_node(const ui_element& node, const context& s) override;

  /// @brief See `imgui_renderer::capture_hit_test_for_last_item()` -- used by
  ///        `render_table()` for `TableRow`, whose row-spanning `Selectable`
  ///        is drawn inline rather than via `render_node()`.
  void capture_hit_test_for_last_item(const ui_element& node) override;

  /// @brief Answer any tree/hit-test queries queued by `query_tree()` for @p s.
  void service_automation_queries(const context& s) override;

  /// @brief Returns `this` -- `sdl3_renderer` implements `automation::automation_backend`.
  automation::automation_backend* as_automation_backend() override {
    return this;
  }

  // ── automation::automation_backend ──────────────────────────────────────────

  std::future<std::string> query_tree(uint32_t request_id, const std::string& root) override;
  std::future<std::vector<uint8_t>> capture_screenshot() override;
  void inject_mouse_move(float x, float y) override;
  void inject_mouse_button(int button, bool down) override;
  void inject_key(int keycode, bool down) override;
  void inject_text(const std::string& utf8) override;
#endif

 protected:
  /// @brief The underlying SDL_Renderer handle, for tests/subclasses that
  ///        need to issue direct SDL draw/readback calls (e.g. verifying
  ///        begin_render_target()/end_render_target() actually swap the
  ///        active target) beyond what this class's own API exposes.
  SDL_Renderer* sdl_renderer() const {
    return sdl_renderer_;
  }

  /// @brief The Windows content-scale factor applied to fonts/style in
  ///        setup()/rebuild_font_atlas() to compensate for SDL3 not
  ///        separating window "points" from physical pixels there. See
  ///        display_scale_'s doc comment. Exposed for tests only.
  float display_scale() const {
    return display_scale_;
  }

 private:
  const char* title_;
  int width_;
  int height_;
  int font_size_;
  SDL_Window* window_ = nullptr;

  // Windows content-scale factor (e.g. 1.5 at 150% display scaling), read
  // once in setup() via SDL_GetWindowDisplayScale(). Unlike web_renderer
  // (where the browser reports canvas size in DPI-normalized CSS px and
  // devicePixelRatio separately), SDL3's window size on Windows *is* the
  // literal physical pixel count -- SDL_GetWindowSize() and
  // SDL_GetWindowSizeInPixels() return the same value, so
  // io.DisplayFramebufferScale computed by ImGui_ImplSDL3_NewFrame() is
  // always ~1.0 there regardless of the OS scale setting. Left uncorrected,
  // this makes the whole UI (fonts, padding, spacing) render at a fixed
  // physical pixel size that ignores Windows' Display Scale setting, so it
  // looks visibly smaller than the equivalent web UI at the same logical
  // width/height. rebuild_font_atlas() and setup() multiply font sizes and
  // ImGuiStyle by this factor to compensate. Not updated if the window is
  // later dragged to a monitor with a different scale.
  float display_scale_ = 1.0f;

  SDL_Renderer* sdl_renderer_ = nullptr;
  std::atomic<bool> quit_{false};

  // Backing storage for ImGuiIO::IniFilename -- ImGui keeps the pointer we
  // assign it, so the path string must outlive the ImGui context.
  std::string ini_path_;

  // ── Mouse-motion debounce ─────────────────────────────────────────────────
  //
  // Mouse motion is fed to ImGui unconditionally (position stays accurate),
  // but only counts as "activity" that forces a frame once it has moved far
  // enough or enough time has passed since the last motion-triggered frame.
  // This keeps small jitter / slow drags from redrawing on every SDL poll.

  /// @brief Returns true if motion to (x, y) is significant enough to force
  ///        a frame, and if so advances the debounce baseline to (x, y, now).
  bool mouse_motion_significant(float x, float y);

  float last_motion_x_{0.0f};
  float last_motion_y_{0.0f};
  std::chrono::steady_clock::time_point last_motion_time_{};
  bool has_motion_baseline_{false};

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
  bool fonts_dirty_{true};

  /// @brief Clear and rebuild the ImGui font atlas, then re-upload to the GPU.
  void rebuild_font_atlas();

  // ── Offscreen render target ────────────────────────────────────────────────

  /// Current offscreen render target texture, recreated by
  /// begin_render_target() only when the requested size changes; freed in
  /// teardown().
  SDL_Texture* render_target_ = nullptr;
  int render_target_w_ = 0;
  int render_target_h_ = 0;

  /// Render target active immediately before the last begin_render_target()
  /// call, restored by end_render_target(). Null means "the window backbuffer".
  SDL_Texture* saved_render_target_ = nullptr;

#ifdef WISH_AUTOMATION_ENABLED
  // ── automation ────────────────────────────────────────────────────────────
  //
  // Mirrors web_renderer's own hit_test_map_/pending_tree_queries_ (see
  // src/web/web_renderer.hpp), adapted for a promise/future hand-off instead
  // of a WebSocket reply -- automation_service's RMI dispatch thread blocks
  // on the future it gets back from query_tree()/capture_screenshot()
  // instead of a browser polling a Promise.

  /// Screen rect + interaction flags per widget, keyed by __wish_id, for the
  /// frame currently being drawn. Render-thread only (written by
  /// render_node(), read by service_automation_queries() -- both always
  /// called from the render thread). Cleared at the top of begin_frame() so
  /// a query always answers against a complete frame, never a
  /// partially-rendered one.
  automation::hit_test_map hit_test_map_;

  struct pending_tree_query {
    uint32_t request_id;
    std::string root;
    std::promise<std::string> reply;
  };

  /// query_tree() (called from an RMI dispatch thread) pushes here; drained
  /// on the render thread by service_automation_queries().
  bison::synchronized<std::deque<pending_tree_query>> pending_tree_queries_;

  /// capture_screenshot() (called from an RMI dispatch thread) pushes here;
  /// drained on the render thread by end_frame(), right after draw data is
  /// submitted to sdl_renderer_ but before SDL_RenderPresent() -- the last
  /// point at which SDL_RenderReadPixels() still sees this frame's pixels.
  bison::synchronized<std::deque<std::promise<std::vector<uint8_t>>>> pending_screenshots_;

  /// @brief Read back the currently-rendered frame from `sdl_renderer_` and
  ///        PNG-encode it. Render-thread only; called from `end_frame()`.
  std::vector<uint8_t> capture_frame_png();

  /// inject_text() (called from an RMI dispatch thread) pushes here; drained
  /// on the render thread at the top of begin_frame(), before ImGui::NewFrame(),
  /// via ImGuiIO::AddInputCharactersUTF8() -- bypassing SDL's own event queue
  /// entirely, since SDL_TextInputEvent::text is a `const char*` whose
  /// lifetime a synthetically-pushed SDL_Event cannot safely own (unlike the
  /// plain-POD mouse/key events, which SDL_PushEvent copies by value with no
  /// such lifetime issue).
  bison::synchronized<std::deque<std::string>> pending_text_inputs_;
#endif
};

} // namespace bdg::wish

#endif // WISH_SDL3_ENABLED
