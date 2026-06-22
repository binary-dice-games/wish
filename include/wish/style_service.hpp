// MIT License © 2025 Binary Dice Games
/// @file style_service.hpp
/// @brief Per-session RMI service for configuring the ImGui visual style.
#pragma once

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_common.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace bdg::wish {

class style_service;
using style_service_ptr = std::shared_ptr<style_service>;

/**
 * @brief Per-session service that lets clients configure the renderer style.
 *
 * Registered in the `"wish"` bison namespace as `"__WishStyle"`.  The server
 * creates one instance per connected client in `on_session_created`; the client
 * retrieves it via `bison::dynamic::instantiate("wish"_key, "__WishStyle"_key)`.
 *
 * ## RMI methods exposed to clients
 *
 * | Method   | Params                        | Effect                                        |
 * |----------|-------------------------------|-----------------------------------------------|
 * | `set`    | any flat dynamic field map    | Merges fields into the active style           |
 * | `get`    | —                             | Returns current style as a flat dynamic       |
 * | `preset` | `"name"`: string              | Resets to a built-in preset and clears overrides |
 *
 * ## Supported `preset` names
 *
 * `"dark"`, `"light"`, `"classic"` — map to ImGui::StyleColors{Dark,Light,Classic}.
 *
 * ## Field key naming (for `set` / `get`)
 *
 * Scalar floats: `alpha`, `disabled_alpha`, `window_rounding`, `window_border_size`,
 * `child_rounding`, `child_border_size`, `popup_rounding`, `popup_border_size`,
 * `frame_rounding`, `frame_border_size`, `indent_spacing`, `scrollbar_size`,
 * `scrollbar_rounding`, `grab_min_size`, `grab_rounding`, `tab_rounding`,
 * `tab_border_size`, `separator_text_border_size`.
 *
 * Vec2 fields (two floats, `_x` / `_y` suffix): `window_padding`, `frame_padding`,
 * `item_spacing`, `item_inner_spacing`, `cell_padding`, `button_text_align`.
 *
 * Colors (hex string `"#RRGGBBAA"`): `color_text`, `color_text_disabled`,
 * `color_window_bg`, `color_child_bg`, `color_popup_bg`, `color_border`,
 * `color_border_shadow`, `color_frame_bg`, `color_frame_bg_hovered`,
 * `color_frame_bg_active`, `color_title_bg`, `color_title_bg_active`,
 * `color_title_bg_collapsed`, `color_menu_bar_bg`, `color_scrollbar_bg`,
 * `color_scrollbar_grab`, `color_scrollbar_grab_hovered`,
 * `color_scrollbar_grab_active`, `color_check_mark`, `color_slider_grab`,
 * `color_slider_grab_active`, `color_button`, `color_button_hovered`,
 * `color_button_active`, `color_header`, `color_header_hovered`,
 * `color_header_active`, `color_separator`, `color_separator_hovered`,
 * `color_separator_active`, `color_resize_grip`, `color_resize_grip_hovered`,
 * `color_resize_grip_active`, `color_plot_lines`, `color_plot_lines_hovered`,
 * `color_plot_histogram`, `color_plot_histogram_hovered`,
 * `color_text_selected_bg`, `color_modal_window_dim_bg`.
 */
class style_service : public bison::dynamic {
 public:
  /// @brief Construct and register RMI methods.
  /// @param base  Prototype-initialised dynamic base (from `dynamic::instantiate`).
  explicit style_service(bison::dynamic&& base);

  // ── Public C++ API (also exposed via RMI) ──────────────────────────────────

  /// @brief Merge flat style fields from @p params into the active style.
  /// @param params  A flat dynamic where each key maps to a float (scalar /
  ///                vec2 component) or a string (preset or `"#RRGGBBAA"` color).
  void set_fields(const bison::dynamic& params);

  /// @brief Return the current style as a flat dynamic (copy).
  bison::dynamic get_fields() const;

  /// @brief Replace the style with a named built-in preset.
  /// @param name  One of `"dark"`, `"light"`, `"classic"`.
  /// @throws std::runtime_error for unknown preset names.
  void set_preset(const std::string& name);

  /// @brief Read-only view of the current style field map.
  const bison::dynamic& current_style() const { return style_; }

  // ── Renderer cache (render-thread only) ────────────────────────────────────

  /// @brief True when the field map has changed since the last compiled cache.
  ///
  /// The render thread checks this flag before each session draw.  When true
  /// it recompiles the bison fields into a backend-specific representation and
  /// calls `set_renderer_cache` to store it and clear the flag.
  bool is_dirty() const noexcept {
    return dirty_.load(std::memory_order_acquire);
  }

  /// @brief Opaque compiled-style slot, written and read only on the render thread.
  const std::shared_ptr<void>& renderer_cache() const { return renderer_cache_; }

  /// @brief Store a compiled cache and clear the dirty flag.
  /// @param c  Renderer-specific compiled representation (e.g. a heap ImGuiStyle).
  void set_renderer_cache(std::shared_ptr<void> c) {
    renderer_cache_ = std::move(c);
    dirty_.store(false, std::memory_order_release);
  }

 private:
  bison::dynamic style_;
  std::atomic<bool> dirty_{true};
  std::shared_ptr<void> renderer_cache_;
};

/// @brief Register `"__WishStyle"` in the `"wish"` bison class namespace.
void register_style_service();

}  // namespace bdg::wish
