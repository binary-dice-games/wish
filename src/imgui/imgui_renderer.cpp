// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.cpp
/// @brief Dear ImGui concrete renderer — element dispatch.
#include <context/file_service.hpp>
#include <imgui/imgui_renderer.hpp>
#include <server/renderer.hpp>
#include <context/context.hpp>
#include <context/style_service.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

#include <imgui.h>

#include <imgui/imgui_plot3d_renderer.hpp>
#include <imgui/imgui_plot_renderer.hpp>
#include <imgui/imgui_ui_renderer.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── Dispatch table ────────────────────────────────────────────────────────────
//
// Maps class key hash → render function. Built once (process-wide, since it
// never varies) and copied into each imgui_renderer instance's render_fns_ at
// construction, where a caller-supplied extra_render_fns can add to or
// override it -- see imgui_renderer::imgui_renderer().

static const render_fn_map& built_in_render_fns() {
  static const render_fn_map tbl{
      // Docking
      {"DockSpaceViewport"_key.id, render_dockspace_viewport},
      {"DockSpace"_key.id, render_dockspace},
      // Table
      {"Table"_key.id, render_table},
      {"TableColumn"_key.id, render_table_column},
      {"TableRow"_key.id, render_table_row},
      // Core
      {"Window"_key.id, render_window},
      {"Label"_key.id, render_label},
      {"Button"_key.id, render_button},
      {"Checkbox"_key.id, render_checkbox},
      {"SliderFloat"_key.id, render_slider_float},
      {"SliderInt"_key.id, render_slider_int},
      {"InputText"_key.id, render_input_text},
      {"Image"_key.id, render_image},
      {"Separator"_key.id, render_separator},
      {"SeparatorText"_key.id, render_separator_text},
      {"VerticalLayout"_key.id, render_vertical_layout},
      {"HorizontalLayout"_key.id, render_horizontal_layout},
      // Menu
      {"MenuBar"_key.id, render_menu_bar},
      {"Menu"_key.id, render_menu},
      {"MenuItem"_key.id, render_menu_item},
      // Tabs
      {"TabBar"_key.id, render_tab_bar},
      {"TabItem"_key.id, render_tab_item},
      // Tree
      {"TreeNode"_key.id, render_tree_node},
      {"CollapsingHeader"_key.id, render_collapsing_header},
      // Selection
      {"Combo"_key.id, render_combo},
      {"RadioButton"_key.id, render_radio_button},
      {"Selectable"_key.id, render_selectable},
      // Numeric inputs
      {"InputInt"_key.id, render_input_int},
      {"InputFloat"_key.id, render_input_float},
      {"DragFloat"_key.id, render_drag_float},
      {"DragInt"_key.id, render_drag_int},
      // Status
      {"ProgressBar"_key.id, render_progress_bar},
      // Text editor
      {"TextEditor"_key.id, render_text_editor},
      // Plot elements (require ImPlot context — must be inside a Plot)
      {"Plot"_key.id, render_plot},
      {"PlotLine"_key.id, render_plot_line},
      {"PlotScatter"_key.id, render_plot_scatter},
      {"PlotStairs"_key.id, render_plot_stairs},
      {"PlotStems"_key.id, render_plot_stems},
      {"PlotShaded"_key.id, render_plot_shaded},
      {"PlotDigital"_key.id, render_plot_digital},
      {"PlotBars"_key.id, render_plot_bars},
      {"PlotBarsH"_key.id, render_plot_bars_h},
      {"PlotHistogram"_key.id, render_plot_histogram},
      {"PlotHistogram2D"_key.id, render_plot_histogram2d},
      {"PlotHeatmap"_key.id, render_plot_heatmap},
      {"PlotPieChart"_key.id, render_plot_pie_chart},
      {"PlotText"_key.id, render_plot_text},
      {"PlotInfLines"_key.id, render_plot_inf_lines},
      // 3-D plot elements (require ImPlot3D context — must be inside a Plot3D)
      {"Plot3D"_key.id, render_plot3d},
      {"Plot3DLine"_key.id, render_plot3d_line},
      {"Plot3DScatter"_key.id, render_plot3d_scatter},
      {"Plot3DSurface"_key.id, render_plot3d_surface},
      {"Plot3DTriangle"_key.id, render_plot3d_triangle},
      {"Plot3DQuad"_key.id, render_plot3d_quad},
      {"Plot3DMesh"_key.id, render_plot3d_mesh},
      {"Plot3DText"_key.id, render_plot3d_text},
  };
  return tbl;
}

// ── Per-session style helpers ─────────────────────────────────────────────────

// Parse "#RRGGBBAA" or "#RRGGBB" hex color string into an ImVec4. Declared in
// imgui_renderer.hpp so imgui_ui_renderer.cpp can reuse it (e.g. Image's
// "tint" field).
ImVec4 parse_hex_color(const std::string& s) {
  if (s.size() < 7 || s[0] != '#')
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  try {
    std::string hex = s.substr(1);
    if (hex.size() == 6)
      hex += "FF";
    if (hex.size() != 8)
      return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    unsigned long val = std::stoul(hex, nullptr, 16);
    return ImVec4(
        static_cast<float>((val >> 24) & 0xFF) / 255.0f,
        static_cast<float>((val >> 16) & 0xFF) / 255.0f,
        static_cast<float>((val >> 8) & 0xFF) / 255.0f,
        static_cast<float>((val) & 0xFF) / 255.0f);
  } catch (...) {
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  }
}

// Apply all fields from sd into style.
static void apply_style_fields(const bison::dynamic& sd, ImGuiStyle& style) {
  // Apply named preset first so per-field overrides can refine it.
  const auto* preset_f = sd.findField("preset"_key);
  if (preset_f && preset_f->is<std::string>()) {
    const std::string& p = preset_f->as<std::string>();
    if (p == "dark")
      ImGui::StyleColorsDark(&style);
    else if (p == "light")
      ImGui::StyleColorsLight(&style);
    else if (p == "classic")
      ImGui::StyleColorsClassic(&style);
  }

  // Scalar float overrides.
  auto fset = [&](key_t k, float& target) {
    const auto* f = sd.findField(k);
    if (f && f->is<float>())
      target = f->as<float>();
  };
  fset("alpha"_key, style.Alpha);
  fset("disabled_alpha"_key, style.DisabledAlpha);
  fset("window_rounding"_key, style.WindowRounding);
  fset("window_border_size"_key, style.WindowBorderSize);
  fset("child_rounding"_key, style.ChildRounding);
  fset("child_border_size"_key, style.ChildBorderSize);
  fset("popup_rounding"_key, style.PopupRounding);
  fset("popup_border_size"_key, style.PopupBorderSize);
  fset("frame_rounding"_key, style.FrameRounding);
  fset("frame_border_size"_key, style.FrameBorderSize);
  fset("indent_spacing"_key, style.IndentSpacing);
  fset("scrollbar_size"_key, style.ScrollbarSize);
  fset("scrollbar_rounding"_key, style.ScrollbarRounding);
  fset("grab_min_size"_key, style.GrabMinSize);
  fset("grab_rounding"_key, style.GrabRounding);
  fset("tab_rounding"_key, style.TabRounding);
  fset("tab_border_size"_key, style.TabBorderSize);
  fset("separator_text_border_size"_key, style.SeparatorTextBorderSize);

  // Vec2 overrides (stored as _x / _y float pairs).
  fset("window_padding_x"_key, style.WindowPadding.x);
  fset("window_padding_y"_key, style.WindowPadding.y);
  fset("frame_padding_x"_key, style.FramePadding.x);
  fset("frame_padding_y"_key, style.FramePadding.y);
  fset("item_spacing_x"_key, style.ItemSpacing.x);
  fset("item_spacing_y"_key, style.ItemSpacing.y);
  fset("item_inner_spacing_x"_key, style.ItemInnerSpacing.x);
  fset("item_inner_spacing_y"_key, style.ItemInnerSpacing.y);
  fset("cell_padding_x"_key, style.CellPadding.x);
  fset("cell_padding_y"_key, style.CellPadding.y);
  fset("button_text_align_x"_key, style.ButtonTextAlign.x);
  fset("button_text_align_y"_key, style.ButtonTextAlign.y);

  // Color overrides (#RRGGBBAA hex strings).
  auto cset = [&](key_t k, ImVec4& target) {
    const auto* f = sd.findField(k);
    if (f && f->is<std::string>())
      target = parse_hex_color(f->as<std::string>());
  };
  cset("color_text"_key, style.Colors[ImGuiCol_Text]);
  cset("color_text_disabled"_key, style.Colors[ImGuiCol_TextDisabled]);
  cset("color_window_bg"_key, style.Colors[ImGuiCol_WindowBg]);
  cset("color_child_bg"_key, style.Colors[ImGuiCol_ChildBg]);
  cset("color_popup_bg"_key, style.Colors[ImGuiCol_PopupBg]);
  cset("color_border"_key, style.Colors[ImGuiCol_Border]);
  cset("color_border_shadow"_key, style.Colors[ImGuiCol_BorderShadow]);
  cset("color_frame_bg"_key, style.Colors[ImGuiCol_FrameBg]);
  cset("color_frame_bg_hovered"_key, style.Colors[ImGuiCol_FrameBgHovered]);
  cset("color_frame_bg_active"_key, style.Colors[ImGuiCol_FrameBgActive]);
  cset("color_title_bg"_key, style.Colors[ImGuiCol_TitleBg]);
  cset("color_title_bg_active"_key, style.Colors[ImGuiCol_TitleBgActive]);
  cset("color_title_bg_collapsed"_key, style.Colors[ImGuiCol_TitleBgCollapsed]);
  cset("color_menu_bar_bg"_key, style.Colors[ImGuiCol_MenuBarBg]);
  cset("color_scrollbar_bg"_key, style.Colors[ImGuiCol_ScrollbarBg]);
  cset("color_scrollbar_grab"_key, style.Colors[ImGuiCol_ScrollbarGrab]);
  cset("color_scrollbar_grab_hovered"_key, style.Colors[ImGuiCol_ScrollbarGrabHovered]);
  cset("color_scrollbar_grab_active"_key, style.Colors[ImGuiCol_ScrollbarGrabActive]);
  cset("color_check_mark"_key, style.Colors[ImGuiCol_CheckMark]);
  cset("color_slider_grab"_key, style.Colors[ImGuiCol_SliderGrab]);
  cset("color_slider_grab_active"_key, style.Colors[ImGuiCol_SliderGrabActive]);
  cset("color_button"_key, style.Colors[ImGuiCol_Button]);
  cset("color_button_hovered"_key, style.Colors[ImGuiCol_ButtonHovered]);
  cset("color_button_active"_key, style.Colors[ImGuiCol_ButtonActive]);
  cset("color_header"_key, style.Colors[ImGuiCol_Header]);
  cset("color_header_hovered"_key, style.Colors[ImGuiCol_HeaderHovered]);
  cset("color_header_active"_key, style.Colors[ImGuiCol_HeaderActive]);
  cset("color_separator"_key, style.Colors[ImGuiCol_Separator]);
  cset("color_separator_hovered"_key, style.Colors[ImGuiCol_SeparatorHovered]);
  cset("color_separator_active"_key, style.Colors[ImGuiCol_SeparatorActive]);
  cset("color_resize_grip"_key, style.Colors[ImGuiCol_ResizeGrip]);
  cset("color_resize_grip_hovered"_key, style.Colors[ImGuiCol_ResizeGripHovered]);
  cset("color_resize_grip_active"_key, style.Colors[ImGuiCol_ResizeGripActive]);
  cset("color_plot_lines"_key, style.Colors[ImGuiCol_PlotLines]);
  cset("color_plot_lines_hovered"_key, style.Colors[ImGuiCol_PlotLinesHovered]);
  cset("color_plot_histogram"_key, style.Colors[ImGuiCol_PlotHistogram]);
  cset("color_plot_histogram_hovered"_key, style.Colors[ImGuiCol_PlotHistogramHovered]);
  cset("color_text_selected_bg"_key, style.Colors[ImGuiCol_TextSelectedBg]);
  cset("color_modal_window_dim_bg"_key, style.Colors[ImGuiCol_ModalWindowDimBg]);
  cset("color_docking_empty_bg"_key, style.Colors[ImGuiCol_DockingEmptyBg]);
}

// ── imgui_renderer ────────────────────────────────────────────────────────────

imgui_renderer::imgui_renderer(render_fn_map extra_render_fns) : render_fns_(built_in_render_fns()) {
  for (auto& [class_id, fn] : extra_render_fns)
    render_fns_[class_id] = fn;
}

void imgui_renderer::begin_frame() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
    io.DisplaySize = ImVec2(800.0f, 600.0f);
  if (io.DeltaTime <= 0.0f)
    io.DeltaTime = 1.0f / 60.0f;
  ImGui::NewFrame();
}

void imgui_renderer::end_frame() {
  ImGui::EndFrame();
}

bool imgui_renderer::wants_continuous_redraw() const {
  return ImGui::GetIO().WantTextInput;
}

void imgui_renderer::render_node(const ui_element& node, const context& s) {
  if (!node.get_as<bool>("visible"_key, true))
    return;

  // Per-element font override.  PushFont(nullptr) is valid — it selects the
  // default font — so the push/pop pair is always safe to emit.
  ImFont* font = nullptr;
  {
    auto font_path = node.get_as<std::string>("font_path"_key, "");
    auto font_size = node.get_as<float>("font_size"_key, 0.0f);
    if (font_path.empty() && font_size > 0.0f)
      font_path = "res/fonts/default.ttf";
    if (!font_path.empty() && font_size > 0.0f) {
      static const std::vector<std::string> kFontExtensions{"ttf", "otf"};
      auto full = file_service::resolve_or_fetch(
          font_path, s.resource_dir, s.allow_absolute_paths, s.allow_url_fetch, kFontExtensions);
      if (!full.empty())
        font = get_or_load_font(full.string(), font_size);
    }
  }
  ImGui::PushFont(font);

  // Scope every widget under this node to stable_id(node) via the ID stack,
  // instead of baking an id suffix into each widget's own label text (the
  // "###<id>" convention render_window still uses for its title -- see
  // with_id() in imgui_ui_renderer.hpp: ImGui::Begin() ignores the ID stack
  // for top-level windows, so PushID can't help there). This keeps widget
  // labels clean, and — since stable_id() is derived from a run-independent
  // dot-path where available — makes persisted per-widget ImGui state
  // (TreeNode/CollapsingHeader open state, Table column widths, etc.)
  // survive a restart the same way render_window's title id does.
  ImGui::PushID(stable_id(node).c_str());

  auto cls = node.as<key_t>(dynamic::CLASS);
  auto it = render_fns_.find(cls.id);
  if (it != render_fns_.end()) {
    it->second(*this, node, s);
  } else {
    // Unknown class: log placeholder and pass through so children still render.
    ImGui::TextDisabled("[wish: unknown element '%s']", std::to_string(cls.id).c_str());
    render_children(*this, node, s);
  }

  ImGui::PopID();
  ImGui::PopFont();
}

void imgui_renderer::render_session(const ui_element& root, const context& s) {
  if (!s.style_service) {
    render_node(root, s);
    return;
  }

  // Recompile the bison field map into an ImGuiStyle only when the client
  // has changed the style since the last compiled cache.
  if (s.style_service->is_dirty()) {
    auto compiled = std::make_shared<ImGuiStyle>();
    apply_style_fields(s.style_service->current_style(), *compiled);
    s.style_service->set_renderer_cache(compiled);
  }

  // RAII guard: swap in the cached style, render, restore.
  const ImGuiStyle& compiled = *std::static_pointer_cast<ImGuiStyle>(s.style_service->renderer_cache());
  ImGuiStyle saved = ImGui::GetStyle();
  ImGui::GetStyle() = compiled;
  try {
    render_node(root, s);
  } catch (...) {
    ImGui::GetStyle() = saved;
    throw;
  }
  ImGui::GetStyle() = saved;
}

ImTextureID imgui_renderer::get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir,
    const std::unordered_map<std::string, uint32_t>* embedded_crc32s) {
  auto it = texture_cache_.find(src);
  if (it != texture_cache_.end())
    return it->second;
  // Texture loading requires a GPU backend; return null in headless contexts.
  (void)resource_dir;
  (void)embedded_crc32s;
  return texture_cache_[src] = ImTextureID{};
}

ImFont* imgui_renderer::get_or_load_font(const std::string& path, float size) {
  // Font loading requires a GPU backend; return null (default font) here.
  (void)path;
  (void)size;
  return nullptr;
}

ImTextureID imgui_renderer::begin_render_target(int w, int h) {
  // Offscreen render targets require a GPU backend; headless-safe no-op.
  (void)w;
  (void)h;
  return ImTextureID{};
}

void imgui_renderer::end_render_target() {
  // No-op to match the base begin_render_target()'s "unsupported" contract.
}

} // namespace bdg::wish
