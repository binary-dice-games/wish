// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.hpp
/// @brief Dear ImGui concrete renderer backend for wish.
#pragma once

#include <server/renderer.hpp>

#include "src/bison/bison_common.hpp"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace bdg::wish {

class imgui_renderer;

/// @brief Parses a "#RRGGBBAA" or "#RRGGBB" hex color string into an ImVec4
/// (each component in [0, 1]). Returns opaque white for anything malformed
/// (missing '#', wrong length, non-hex digits) -- the same fallback
/// apply_style_fields() (imgui_renderer.cpp) uses for a bad style color, so
/// a bad tint/color field degrades to "no visible tint" rather than a
/// thrown exception or garbage color.
ImVec4 parse_hex_color(const std::string& s);

/// @brief Renders one `ui_element` node's ImGui widget(s); the uniform
///        signature every `render_*` function in imgui_ui_renderer.hpp uses.
using render_fn = void (*)(imgui_renderer&, const ui_element&, const context&);

/// @brief Class-id -> render function map, used to extend an `imgui_renderer`
///        instance's dispatch table with project-specific `ui_element`
///        subclasses at construction time (see `imgui_renderer`'s
///        `extra_render_fns` constructor parameter).
using render_fn_map = std::unordered_map<bison::hash_t, render_fn>;

/// @brief Generic drag-and-drop: attaches to whatever ImGui item was drawn
///        last, so it is only meaningful called immediately after a leaf
///        element's own top-level item (Button, Image, Label, a Table row's
///        Selectable, ...) -- see docs/ui-elements.md's "Drag and drop"
///        section. Both `"drag_type"`/`"drop_type"` default to empty on
///        every element (element.cpp), so this is a no-op for the
///        overwhelming majority of calls. Shared by `imgui_renderer::
///        render_node()` (every element) and `render_table()`'s per-row loop
///        (imgui_ui_renderer.cpp) -- `TableRow` children are rendered
///        directly by `render_table()`, never via a recursive `render_node()`
///        call, so without this second call site a row's own `drag_type`/
///        `drop_type` fields would never be checked.
void handle_drag_drop(const ui_element& node, const context& s);

/// @brief Draws a gold highlight box around [@p rect_min, @p rect_max] if
///        @p node's `"__wish_highlight__"` field is set, into the
///        *current* window's own draw list (`ImGui::GetWindowDrawList()`)
///        rather than the global foreground layer -- so the box
///        participates in normal window z-ordering/clipping instead of
///        always drawing on top of every window. Only valid to call while
///        a real wish-tree window is current: for `Window`/
///        `DockSpaceViewport` (which self-report their own rect and have
///        already closed their own `Begin()`/`BeginPopupModal()` scope by
///        the time `imgui_renderer::render_node()` reaches its generic
///        post-dispatch code), call this from inside `render_window()`/
///        `render_dockspace_viewport()` themselves, before their own
///        `End()`/`EndPopup()` -- see those functions' call sites in
///        `imgui_ui_renderer.cpp`.
void draw_highlight_if_set(const ui_element& node, ImVec2 rect_min, ImVec2 rect_max);

/**
 * @brief Renderer backend that draws wish UI elements via Dear ImGui.
 *
 * Dispatch is on the element's `__class` field; every leaf class maps to the
 * corresponding ImGui widget call.  Layout elements (VerticalLayout,
 * HorizontalLayout) are handled in step 10.  Unknown classes are passed
 * through by calling `render_children` so subtrees are never silently dropped.
 *
 * The caller is responsible for creating and destroying the ImGui context.
 * `begin_frame` calls `ImGui::NewFrame`; `end_frame` calls `ImGui::EndFrame`.
 * No platform or renderer backend is required — attach one before calling
 * `begin_frame` when you need actual display output.
 */
class imgui_renderer : public renderer {
 public:
  /**
   * @brief Construct with an optional set of extra `render_*` functions.
   *
   * @param extra_render_fns  Class-id -> render function entries merged over
   *                           wish's built-in dispatch table for this
   *                           instance only (an entry here overrides a
   *                           built-in with the same class id). Lets a
   *                           project embedding wish (e.g. genie) add
   *                           dispatch for its own `ui_element` subclasses
   *                           without forking or patching this class. The
   *                           mapping is fixed for the lifetime of the
   *                           instance -- there is no post-construction
   *                           registration call, so it can never race with
   *                           `render_node`.
   */
  explicit imgui_renderer(render_fn_map extra_render_fns = {});
  ~imgui_renderer() override = default;

  /// @brief Prepares a new ImGui frame.  Sets sensible IO defaults if the
  ///        caller has not done so (useful for headless/test contexts).
  void begin_frame() override;

  /// @brief Dispatches the element to its ImGui widget(s) and recurses into
  ///        children where required.
  void render_node(const ui_element& node, const context& s) override;

  /**
   * @brief Render a session's element tree with per-session style isolation.
   *
   * Saves the global ImGuiStyle, applies the session's style (from
   * `s.style_service`), renders via `render_node`, then restores the
   * original style — even if rendering throws.  When no style is configured
   * for the session, delegates directly to `render_node`.
   */
  void render_session(const ui_element& root, const context& s) override;

  /// @brief Ends the ImGui frame (`ImGui::EndFrame`).
  void end_frame() override;

  /// @brief True while an active ImGui text-input widget wants a blinking
  ///        caret drawn (`ImGuiIO::WantTextInput`). Custom widgets that draw
  ///        their own caret without going through ImGui::InputText (e.g. the
  ///        TextEditor wish element) mark the session dirty directly instead.
  bool wants_continuous_redraw() const override;

  /**
   * @brief Fetch a cached texture or attempt to load it from @p resource_dir.
   * Returns a zero/null ID in headless / no-backend contexts.
   *
   * @param embedded_crc32s  Optional map of resource_dir-relative path ->
   *                          precomputed CRC-32 (see `context::embedded_crc32s`).
   *                          When @p src has an entry here, an override that
   *                          versions its resource cache by content hash
   *                          (e.g. `web_renderer`) may reuse it instead of
   *                          recomputing a CRC-32 over the file's bytes.
   *                          Ignored by backends that don't cache by content
   *                          version.
   */
  virtual ImTextureID get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir,
      const std::unordered_map<std::string, uint32_t>* embedded_crc32s = nullptr);

  /**
   * @brief Fetch a cached font or schedule loading the TTF at @p path with
   *        @p size pixels.
   *
   * On the first call for a given (path, size) pair, returns `nullptr` and
   * schedules an atlas rebuild for the next frame.  Subsequent calls return
   * the loaded `ImFont*`.  The base implementation always returns `nullptr`
   * (headless / no-GPU contexts).
   *
   * @param path  Fully-resolved absolute path to a TTF font file.
   * @param size  Font size in pixels (must be > 0).
   */
  virtual ImFont* get_or_load_font(const std::string& path, float size);

  /**
   * @brief Redirect subsequent draws into an offscreen render target of size
   *        @p w x @p h, returning its texture handle.
   *
   * Every draw call issued between this call and the matching
   * `end_render_target()` renders into the offscreen texture instead of the
   * window/canvas backbuffer; `end_render_target()` restores whatever target
   * was active before this call. Backends that cannot support an offscreen
   * target (no GPU backend attached) return a null handle and leave the
   * active target unchanged -- callers must treat a null return as
   * "unsupported" rather than assuming a texture was created.
   *
   * @param w  Target width in pixels (must be > 0 for a real render target).
   * @param h  Target height in pixels (must be > 0 for a real render target).
   * @return   The offscreen texture's handle, or a null `ImTextureID` if this
   *           backend has no GPU render target support.
   */
  virtual ImTextureID begin_render_target(int w, int h);

  /**
   * @brief Restore the render target active before the matching
   *        `begin_render_target()` call.
   *
   * Calling this without a preceding `begin_render_target()` (or after the
   * base no-op `begin_render_target()` returned null) is a safe no-op.
   */
  virtual void end_render_target();

  /**
   * @brief Immediately submits @p draw_list to whatever target is currently
   *        active -- the window backbuffer, or an offscreen target between
   *        a `begin_render_target()`/`end_render_target()` pair -- instead
   *        of going through ImGui's own once-per-frame deferred flush.
   *
   * ImGui accumulates one draw list per window and flushes all of them
   * together in a single backend call at `end_frame()` -- so a `render_*`
   * function that wants a *separate* draw pass landing on a *different*
   * target (e.g. a scene renderer drawing into an offscreen render target
   * that will later be composited elsewhere) needs its own manually-built
   * `ImDrawList` and its own immediate submission, bypassing the
   * once-per-frame path entirely. Base implementation is a no-op
   * (headless-safe, matching `begin_render_target()`'s "unsupported"
   * contract).
   *
   * @param draw_list  A caller-owned `ImDrawList`, typically constructed
   *                   with `ImGui::GetDrawListSharedData()` and initialized
   *                   via `_ResetForNewFrame()` before any `PushClipRect`/
   *                   `Add*` calls, populated with whatever draw commands
   *                   should land on the current target.
   * @param w  Target width in pixels, defining the coordinate space
   *           @p draw_list's vertices are expressed in (top-left origin).
   * @param h  Target height in pixels.
   */
  virtual void flush_draw_list(ImDrawList& draw_list, int w, int h);

 protected:
  /// Loaded texture cache: maps resource path → ImTextureID.
  std::unordered_map<std::string, ImTextureID> texture_cache_;

  /// The screen rect `render_node()` resolved for the most recently
  /// dispatched node -- for most classes, the bounding box of everything
  /// drawn by the dispatch call (via a `BeginGroup()`/`EndGroup()` wrap);
  /// for `Window`/`DockSpaceViewport`, the window's own
  /// `GetWindowPos()`/`GetWindowSize()`, since those open a new top-level
  /// window whose content a group can't see into. Set once per
  /// `render_node()` call, right before `handle_drag_drop()` -- read by the
  /// `"__wish_highlight__"` box draw, and by `web_renderer::render_node()`'s
  /// hit-test capture so both consumers share one resolution, instead of
  /// each re-deriving it (and disagreeing) independently.
  ImVec2 last_resolved_rect_min_{};
  ImVec2 last_resolved_rect_max_{};

 private:
  /// This instance's full class-id -> render function dispatch table: wish's
  /// built-ins seeded at construction, overridden/extended by whatever was
  /// passed as `extra_render_fns`. See `render_node()`.
  render_fn_map render_fns_;
};

} // namespace bdg::wish
