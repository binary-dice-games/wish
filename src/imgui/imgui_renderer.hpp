// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.hpp
/// @brief Dear ImGui concrete renderer backend for wish.
#pragma once

#include <renderer.hpp>

#include <imgui.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace bdg::wish {

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
  imgui_renderer() = default;
  ~imgui_renderer() override = default;

  /// @brief Prepares a new ImGui frame.  Sets sensible IO defaults if the
  ///        caller has not done so (useful for headless/test contexts).
  void begin_frame() override;

  /// @brief Dispatches the element to its ImGui widget(s) and recurses into
  ///        children where required.
  void render_node(const ui_element& node, const session& s) override;

  /**
   * @brief Render a session's element tree with per-session style isolation.
   *
   * Saves the global ImGuiStyle, applies the session's style (from
   * `s.style_service`), renders via `render_node`, then restores the
   * original style — even if rendering throws.  When no style is configured
   * for the session, delegates directly to `render_node`.
   */
  void render_session(const ui_element& root, const session& s) override;

  /// @brief Ends the ImGui frame (`ImGui::EndFrame`).
  void end_frame() override;

  /// @brief True while an active ImGui text-input widget wants a blinking
  ///        caret drawn (`ImGuiIO::WantTextInput`). Custom widgets that draw
  ///        their own caret without going through ImGui::InputText (e.g. the
  ///        TextEditor wish element) mark the session dirty directly instead.
  bool wants_continuous_redraw() const override;

  /// @brief Fetch a cached texture or attempt to load it from @p resource_dir.
  /// Returns a zero/null ID in headless / no-backend contexts.
  virtual ImTextureID get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir);

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
};

} // namespace bdg::wish
