// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.hpp
/// @brief Dear ImGui concrete renderer backend for wish.
#pragma once

#include <wish/renderer.hpp>

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
  void render_node(const ui_element& node, session& s) override;

  /// @brief Ends the ImGui frame (`ImGui::EndFrame`).
  void end_frame() override;

  /// @brief Fetch a cached texture or attempt to load it from @p resource_dir.
  /// Returns a zero/null ID in headless / no-backend contexts.
  virtual ImTextureID get_or_load_texture(
      const std::string&            src,
      const std::filesystem::path&  resource_dir);

 protected:
  /// Loaded texture cache: maps resource path → ImTextureID.
  std::unordered_map<std::string, ImTextureID> texture_cache_;
};

}  // namespace bdg::wish
