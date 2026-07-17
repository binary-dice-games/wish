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

 protected:
  /// Loaded texture cache: maps resource path → ImTextureID.
  std::unordered_map<std::string, ImTextureID> texture_cache_;

 private:
  /// This instance's full class-id -> render function dispatch table: wish's
  /// built-ins seeded at construction, overridden/extended by whatever was
  /// passed as `extra_render_fns`. See `render_node()`.
  render_fn_map render_fns_;
};

} // namespace bdg::wish
