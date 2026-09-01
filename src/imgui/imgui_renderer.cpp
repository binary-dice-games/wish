// MIT License © 2025 Binary Dice Games
/// @file imgui_renderer.cpp
/// @brief Dear ImGui concrete renderer — element dispatch.
#include <context/file_service.hpp>
#include <imgui/imgui_renderer.hpp>
#include <server/renderer.hpp>
#include <context/context.hpp>
#include <context/logger.hpp>
#include <context/style_service.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/profiling.hpp"

#include <imgui.h>

#include <imgui/imgui_graph_renderer.hpp>
#include <imgui/imgui_plot3d_renderer.hpp>
#include <imgui/imgui_plot_renderer.hpp>
#include <imgui/imgui_ui_renderer.hpp>
#include <imgui/themes/themes.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
      // Graph (lane-based DAG visualization, e.g. a git commit graph)
      {"GraphNode"_key.id, render_graph_node},
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
      {"Splitter"_key.id, render_splitter},
      {"Spring"_key.id, render_spring},
      // ObjectInspector's children are always exactly [Table, Label]
      // (see object_inspector.cpp's set_target()) -- no bespoke render
      // function needed, it lays out identically to a VerticalLayout.
      {"ObjectInspector"_key.id, render_vertical_layout},
      {"ColorEdit"_key.id, render_color_edit},
      // Menu
      {"MenuBar"_key.id, render_menu_bar},
      {"Menu"_key.id, render_menu},
      {"MenuItem"_key.id, render_menu_item},
      {"MenuButton"_key.id, render_menu_button},
      {"ContextMenu"_key.id, render_context_menu},
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

// ── Theme registry ────────────────────────────────────────────────────────────

// A theme's is_light bit is declared once here, at registration, rather than
// inferred elsewhere from its compiled colors -- see register_theme()'s doc
// comment (imgui_renderer.hpp) for why.
struct theme_entry {
  theme_fn fn;
  bool is_light;
};

static std::unordered_map<std::string, theme_entry>& theme_registry() {
  static std::unordered_map<std::string, theme_entry> registry;
  return registry;
}

void register_theme(const std::string& name, theme_fn fn, bool is_light) {
  theme_registry()[name] = {fn, is_light};
}

// Fallback theme applied -- with a logged warning -- when a session's
// style_service::preset() names a theme this renderer has no registered
// function for (e.g. a typo, or a name meant for a different renderer
// backend). style_service itself does not validate preset names -- see its
// "Supported preset names" doc comment.
static constexpr const char* kDefaultThemeName = "wish";

// Each built-in theme is defined in its own src/imgui/themes/theme_*.cpp
// file (see themes.hpp) and explicitly registered here -- rather than left
// as file-local static initializers -- so registration doesn't depend on
// whether that .cpp's object file happens to get pulled out of the static
// archive (it only would if something else referenced a symbol from it).
static bool register_built_in_themes() {
  register_theme_dark();
  register_theme_light();
  register_theme_classic();
  register_theme_wish();
  return true;
}
static const bool built_in_themes_registered_ = register_built_in_themes();

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

// Apply all fields from sd into style, returning whether the resolved theme
// is light-based (its registered is_light -- see register_theme()'s doc
// comment -- not inferred from the compiled colors below). @p logger, if
// non-null, receives a warning when sd's "preset" field names a theme this
// renderer has no registered function for (see kDefaultThemeName's doc
// comment). Defaults to `true` (matching kDefaultThemeName's own "wish" =
// light) when sd has no "preset" field at all -- e.g. a session that only
// ever used per-field `set()` overrides without a preceding `preset()` call.
static bool apply_style_fields(const bison::dynamic& sd, ImGuiStyle& style, const logger_ptr& logger) {
  bool is_light = true;

  // Apply named preset first so per-field overrides can refine it.
  const auto* preset_f = sd.findField("preset"_key);
  if (preset_f && preset_f->is<std::string>()) {
    const auto& registry = theme_registry();
    const std::string& name = preset_f->as<std::string>();
    auto it = registry.find(name);
    if (it == registry.end()) {
      if (logger)
        logger->warn("wish: unknown theme '" + name + "', falling back to '" + kDefaultThemeName + "'");
      it = registry.find(kDefaultThemeName);
    }
    if (it != registry.end()) {
      it->second.fn(&style);
      is_light = it->second.is_light;
    }
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

  return is_light;
}

// ── imgui_renderer ────────────────────────────────────────────────────────────

imgui_renderer::imgui_renderer(render_fn_map extra_render_fns) : render_fns_(built_in_render_fns()) {
  for (auto& [class_id, fn] : extra_render_fns)
    render_fns_[class_id] = fn;
}

void imgui_renderer::begin_frame() {
  // Cleared here, re-set this frame by whichever dockspace host renders
  // first: an opt-in server-frame host wrapper (`dockspace_renderer`,
  // `host_renderer`) or a `DockSpaceViewport` element in a session's tree.
  ambient_dockspace_id_ = 0;
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

void handle_drag_drop(const ui_element& node, const context& s) {
  const std::string& drag_type = node.drag_type_ref();
  if (!drag_type.empty() && ImGui::BeginDragDropSource()) {
    const std::string& drag_payload = node.drag_payload_ref();
    ImGui::SetDragDropPayload(drag_type.c_str(), drag_payload.data(), drag_payload.size());
    ImGui::TextUnformatted(drag_payload.c_str());
    ImGui::EndDragDropSource();
  }
  const std::string& drop_type = node.drop_type_ref();
  if (!drop_type.empty() && ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* accepted = ImGui::AcceptDragDropPayload(drop_type.c_str())) {
      dynamic payload;
      payload["type"_key] = drop_type;
      payload["payload"_key] =
          std::string(static_cast<const char*>(accepted->Data), static_cast<size_t>(accepted->DataSize));
      enqueue_event(s, node.wish_id(), "dropped"_key, std::move(payload));
    }
    ImGui::EndDragDropTarget();
  }
}

void handle_tooltip(const ui_element& node) {
  const std::string& tooltip = node.tooltip_ref();
  if (tooltip.empty())
    return;
  // Classic `if (IsItemHovered()) SetTooltip(...)` idiom (see imgui.h's
  // SetTooltip() comment): attaches to the last item drawn by the node's
  // dispatch call -- see this function's doc comment.
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", tooltip.c_str());
}

void draw_highlight_if_set(const ui_element& node, ImVec2 rect_min, ImVec2 rect_max) {
  if (!node.highlight())
    return;
  ImGui::GetWindowDrawList()->AddRect(rect_min, rect_max, IM_COL32(255, 215, 0, 255), 0.0f, 0, 2.5f);
}

// See imgui_layout.cpp's identical WISH_LAYOUT_DEBUG_LOG helper for the full
// rationale; duplicated locally (rather than shared across translation
// units) since it's a few lines and this file's own signal is different --
// this one flags a node's own *rendered* rect (last_resolved_rect_min_/
// max_, captured below and consumed both by automation and by
// measure_node()'s last_rendered_size() fallback) changing frame-to-frame
// even though its class has no wish arrange/measure involvement at all
// (e.g. a Table-cell Label/ProgressBar, which imgui_layout.cpp's
// arrange_node() never recurses into) -- catching instability that
// imgui_layout.cpp's own log can't see, since that log only instruments the
// arrange pass.
static std::ofstream* render_debug_log() {
  static std::ofstream* stream = [] {
    const char* path = std::getenv("WISH_LAYOUT_DEBUG_LOG");
    if (!path || !*path)
      return static_cast<std::ofstream*>(nullptr);
    auto* f = new std::ofstream(path, std::ios::app);
    return f->is_open() ? f : (delete f, static_cast<std::ofstream*>(nullptr));
  }();
  return stream;
}

static std::string render_debug_node_label(const ui_element& node) {
  const std::string& path = node.path_ref();
  if (!path.empty())
    return path;
  auto cls = node.class_key();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "class#%08x", static_cast<unsigned>(cls.id));
  return buf;
}

ImFont* resolve_element_font(imgui_renderer& r, const ui_element& node, const context& s) {
  float font_size = node.font_size();
  // No per-element font override at all -- the overwhelmingly common case;
  // matches the original "font_path.empty() || font_size <= 0" guard, since a
  // font is only ever resolved when font_size > 0.
  if (font_size <= 0.0f)
    return nullptr;
  static const std::string kDefaultFontPath{"res/fonts/default.ttf"};
  const std::string& font_path_field = node.font_path_ref();
  const std::string& font_path = font_path_field.empty() ? kDefaultFontPath : font_path_field;
  static const std::vector<std::string> kFontExtensions{"ttf", "otf"};
  auto full = file_service::resolve_or_fetch(
      font_path, s.resource_dir, s.allow_absolute_paths, s.allow_url_fetch, kFontExtensions);
  if (full.empty())
    return nullptr;
  return r.get_or_load_font(full.string(), font_size);
}

void imgui_renderer::render_node(const ui_element& node, const context& s) {
  if (!node.visible())
    return;

  const std::string* profiler_marker = node.profiler_marker();
  BISON_TRACE_SCOPE(profiler_marker ? profiler_marker->c_str() : nullptr);

  // Per-element font override.  PushFont(nullptr) is valid — it selects the
  // default font — so the push/pop pair is always safe to emit.
  ImFont* font = resolve_element_font(*this, node, s);
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
  // Fast path: a node with a "__path__" (the common case for template/form-
  // built trees) needs no allocation -- stable_id() returns that same string
  // verbatim. Only the synthesized-id fallbacks (wish_id number / node
  // address) still build a temporary.
  if (const std::string& node_path = node.path_ref(); !node_path.empty())
    ImGui::PushID(node_path.c_str());
  else
    ImGui::PushID(stable_id(node).c_str());

  auto cls = node.class_key();

  // Window/DockSpaceViewport open a genuine new top-level ImGui window via
  // Begin()/BeginPopupModal() -- GetItemRectMin/Max() after their dispatch
  // call reflects whatever was drawn last *inside* that window, not the
  // window itself, and a BeginGroup()/EndGroup() wrap can't see across a
  // Begin()/End() boundary either. VerticalLayout/HorizontalLayout wrap
  // their entire child set in one real BeginChild() of their own now (see
  // render_vertical_layout()/render_horizontal_layout(), imgui_ui_renderer.cpp)
  // for the same "own real window" reason -- a BeginChild() is a genuine
  // ImGui window internally, so it can self-report via GetWindowPos()/
  // GetWindowSize() the same way Window/DockSpaceViewport do, instead of
  // needing the BeginGroup()/EndGroup() approximation below. All four
  // report their own rect directly (Window/DockSpaceViewport via the
  // "__wish_win_rect_*__" fields stamped by report_self_rect() in
  // render_window()/render_dockspace_viewport(); VerticalLayout/
  // HorizontalLayout via the same report_self_rect() call made directly
  // inside their own BeginChild() scope) instead of being wrapped here. A
  // degenerate (<=0) self size skips that BeginChild() entirely (see
  // render_vertical_layout()'s wrap_self guard) -- report_self_rect() then
  // never ran, so the "defensive fallback" branch below (GetItemRectMin/Max)
  // is what actually captures the rect in that rare case, not a bug.
  bool self_reports_rect =
      cls == "Window"_key || cls == "DockSpaceViewport"_key || cls == "VerticalLayout"_key || cls == "HorizontalLayout"_key;

  // Only classes whose render function actually recurses into ui_element
  // children need a group wrap to report their own bounding box -- every
  // leaf class (Button, Label, Checkbox, ...) already reports its own exact
  // rect via the single ImGui item it draws, and wrapping it anyway would
  // be actively harmful: EndGroup() unconditionally reassigns
  // g.LastItemData.ID to 0 unless the group currently contains the active
  // (or just-deactivated) id (see ImGui::EndGroup(), imgui.cpp), so
  // GetItemID()/IsItemActive() read after render_node() returns for an idle
  // leaf widget would silently go from "that widget's real id" to 0 --
  // breaking anything that keys off a widget's id after the fact (this
  // broke several existing click-simulation tests in test_imgui_renderer.cpp
  // during development of this fix, which is how the need for this
  // exclusion was caught). MenuBar/Menu/MenuButton are deliberately
  // excluded despite recursing: MenuBar's own internal group already resets
  // window->DC.CursorMaxPos back to its pre-menu-bar value (so the menu bar
  // doesn't perturb the parent window's layout), which would make an outer
  // group's bounding box degenerate rather than useful; Menu/MenuButton's
  // own visible identity (the "File" label / trigger Button in the
  // *current* window) is already a single, correctly-reported leaf item --
  // their actual children render inside a separate floating popup window a
  // group around the current window can't meaningfully describe anyway, so
  // there is nothing a wrap would improve for these two, only the same
  // id-forwarding risk described above to avoid. All three are documented
  // as a known, narrower residual limitation instead.
  // Selectable only recurses into children when it actually has any (see
  // render_selectable()'s children-overlay support) -- a plain childless
  // Selectable stays a leaf and must NOT be wrapped, for the same
  // ID-forwarding reason documented above.
  bool selectable_has_children = cls == "Selectable"_key && node.has_children();

  // TabItem is deliberately NOT wrapped: a BeginGroup()/EndGroup() around a
  // BeginTabItem()/EndTabItem() pair is not a sanctioned ImGui pattern, and
  // EndGroup()'s trailing Dummy()+ItemSize() advances the enclosing window's
  // DC.CursorMaxPos by one ItemSpacing.y *per non-selected tab* -- content the
  // window never actually drew (a non-selected tab renders only its button, up
  // in the strip). With many tabs that phantom height (N * ItemSpacing.y) is
  // enough to give the enclosing window a spurious scrollbar. render_tab_item()
  // relies on ImGui's own tab-content layout (EndTabBar() reconciles
  // CurrTabsContentsHeight) and, for a "scroll" tab, its own BeginChild().
  bool needs_group_wrap = cls == "Splitter"_key || cls == "TabBar"_key ||
      cls == "TreeNode"_key || cls == "CollapsingHeader"_key || cls == "Table"_key || cls == "TableRow"_key ||
      cls == "Plot"_key || cls == "Plot3D"_key || selectable_has_children;

  // Set by whichever rect-capture branch below actually runs; gates the
  // last_rendered_size() update further down -- see that call site's doc
  // comment for why a clipped/invisible item must not overwrite it.
  bool item_visible = true;

  if (needs_group_wrap)
    ImGui::BeginGroup();

  auto it = render_fns_.find(cls.id);
  if (it != render_fns_.end()) {
    it->second(*this, node, s);
  } else {
    // Unknown class: log placeholder and pass through so children still render.
    ImGui::TextDisabled("[wish: unknown element '%s']", std::to_string(cls.id).c_str());
    render_children(*this, node, s);
  }

  if (needs_group_wrap) {
    // Guarantees EndGroup() always closes over something drawn at THIS
    // container's own current position. Without this, a container whose
    // dispatch call drew nothing (a collapsed TreeNode, an empty Layout, a
    // BeginTabBar()/BeginTable()/BeginPlot() that returned false, ...)
    // leaves EndGroup()'s bounding-box computation free to fall back to
    // g.LastItemData.Rect.Max -- a #7543 EndTable() workaround in ImGui
    // itself -- which is whatever unrelated item was drawn immediately
    // *before* this container started, silently misattributing this
    // container's rect to a sibling. Placed after all real content (never
    // before), a zero-size Dummy() only overwrites this bookkeeping and
    // never shifts anything the container itself already drew: EndGroup()
    // discards its cursor position via BackupCursorPos regardless. Same
    // root cause and idiom as render_image()'s reserve() lambda above.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndGroup();
    last_resolved_rect_min_ = ImGui::GetItemRectMin();
    last_resolved_rect_max_ = ImGui::GetItemRectMax();
    item_visible = ImGui::IsItemVisible();
  } else if (self_reports_rect) {
    vec2f self_pos;
    vec2f self_size;
    if (node.self_rect(self_pos, self_size)) {
      last_resolved_rect_min_ = ImVec2(self_pos.x, self_pos.y);
      last_resolved_rect_max_ = ImVec2(self_pos.x + self_size.x, self_pos.y + self_size.y);
    } else {
      // Fallback: shouldn't happen for Window/DockSpaceViewport (they
      // always stamp these fields whenever they actually open their
      // window), but is a real, expected path for VerticalLayout/
      // HorizontalLayout with a degenerate (<=0) self size -- their own
      // wrap_self guard (imgui_ui_renderer.cpp) skips BeginChild()/
      // report_self_rect() entirely in that case, so there is no self-
      // reported rect to read. item_visible must still be computed here
      // (unlike the branch above, which is exempt because a real
      // BeginChild()/Begin() always reports true geometry regardless of
      // clipping) since this path's rect genuinely can come from a clipped
      // last item.
      last_resolved_rect_min_ = ImGui::GetItemRectMin();
      last_resolved_rect_max_ = ImGui::GetItemRectMax();
      item_visible = ImGui::IsItemVisible();
    }
  } else {
    // A leaf class (or MenuBar -- see needs_group_wrap's doc comment):
    // neither wrapped nor self-reporting, so the plain post-dispatch ImGui
    // item state already IS this node's own rect, unmodified from before
    // this fix -- exactly as accurate as it always was for these classes.
    last_resolved_rect_min_ = ImGui::GetItemRectMin();
    last_resolved_rect_max_ = ImGui::GetItemRectMax();
    item_visible = ImGui::IsItemVisible();
  }

  // Feeds imgui_layout.cpp's measure_node() fallback for any class with no
  // registered measure_fn (see ui_element::last_rendered_size()'s doc
  // comment) -- generic, so no per-class code is needed here or there.
  //
  // Gated on item_visible -- EXCEPT when this node has no prior confirmed-
  // good value to protect (last_rendered_size() still reads the bootstrap
  // default {0,0}, meaning it has never captured a real size before), in
  // which case this frame's computed size is trusted even if currently
  // reported invisible. Both halves of this rule are load-bearing, each
  // fixing a distinct real bug:
  //
  // - Protecting an EXISTING good value (the original rule) matters
  //   because a node whose real ImGui item was entirely clipped this frame
  //   (e.g. scrolled out of an ancestor window's visible region, or
  //   rendered inside a BeginChild() that was itself fully clipped -- see
  //   ImGuiWindow::SkipItems, which makes a widget call return before it
  //   ever computes a real bb at all, e.g. SliderScalar()'s own
  //   `if (window->SkipItems) return false;`) can get a stale, unrelated
  //   rect from ImGui regardless of the node's actual content. Feeding
  //   that into last_rendered_size() unconditionally corrupts the *next*
  //   frame's measure/arrange for this node's siblings, and for a node
  //   whose arrange result itself depends on that measurement, the
  //   wrong-then-right-then-wrong-again cycle repeats forever -- confirmed
  //   via WISH_LAYOUT_DEBUG_LOG as the mechanism behind a real
  //   user-reported flicker in a HorizontalLayout row sitting next to a
  //   Spring, scrolled out of view.
  // - Trusting a FIRST-EVER measurement even while reported invisible
  //   matters because refusing to ever go first creates a real, separate
  //   deadlock: message_box.cpp's icon+message HorizontalLayout, freshly
  //   built every time the dialog opens, has no measure_fn (see
  //   imgui_layout.cpp's measure_dispatch_fns() comment) so its own
  //   AlwaysAutoResize Window starts one frame too small; that put the
  //   message Label's first real render partially outside the window's
  //   still-too-small clip rect (IsItemVisible() false). Unconditionally
  //   gating on item_visible then permanently blocks last_rendered_size()
  //   from ever updating: visible-depends-on-window-size, window-size-
  //   depends-on-last-rendered-size, last-rendered-size-update-depends-on-
  //   visible -- a genuine circular deadlock that persists no matter how
  //   many more frames render (confirmed via a temporary per-frame stderr
  //   trace: item_visible stayed false and last_rendered_size stayed
  //   {0,0} for 100+ consecutive frames). There is no "last known-good"
  //   value to protect in this case -- the bootstrap {0,0} isn't one --
  //   so trusting the fresh (real, per ImGui::ItemAdd() stamping
  //   g.LastItemData.Rect from the widget's own already-computed bb
  //   *before* its clip early-return -- confirmed against ImGui's own
  //   source, e.g. TextEx() calls CalcTextSize() and ItemSize() ahead of
  //   ItemAdd()) measurement is strictly better than leaving it at {0,0}
  //   forever.
  bool had_prior_confirmed_size = node.last_rendered_size().x != 0.0f || node.last_rendered_size().y != 0.0f;
  if (item_visible || self_reports_rect || !had_prior_confirmed_size) {
    vec2f new_last_rendered_size{
        last_resolved_rect_max_.x - last_resolved_rect_min_.x, last_resolved_rect_max_.y - last_resolved_rect_min_.y};
    if (std::ofstream* log = render_debug_log()) {
      vec2f prev = node.last_rendered_size();
      constexpr float kEpsilon = 0.5f;
      // has_arranged() isn't a fit here (this runs for every leaf, arranged
      // or not); (0,0) is this field's own genuine "never rendered before"
      // bootstrap default (see ui_element::last_rendered_size()'s doc
      // comment), so a first-ever transition away from exactly {0,0} is
      // expected and not logged.
      bool had_prior = prev.x != 0.0f || prev.y != 0.0f;
      bool changed = std::abs(prev.x - new_last_rendered_size.x) > kEpsilon ||
          std::abs(prev.y - new_last_rendered_size.y) > kEpsilon;
      if (had_prior && changed) {
        *log << "[frame " << ImGui::GetFrameCount() << "] " << render_debug_node_label(node)
             << " last_rendered_size changed: (" << prev.x << "," << prev.y << ") -> (" << new_last_rendered_size.x
             << "," << new_last_rendered_size.y << ")\n";
        log->flush();
      }
    }
    node.set_last_rendered_size(new_last_rendered_size);
  }

  // Attaches to whatever ImGui item the dispatch call above drew last -- see
  // handle_drag_drop()'s own doc comment.
  handle_drag_drop(node, s);

  // Generic per-element tooltip: attaches to the same last-drawn item as
  // handle_drag_drop() above, so it shares the "leaf element only" caveat.
  handle_tooltip(node);

  // "__wish_highlight__" is an ad-hoc field (like "__wish_id"/"__path__"
  // elsewhere) set by the editor module to box whichever preview widget
  // corresponds to the JSON element enclosing the source TextEditor's
  // cursor. Drawn into the *current* window's own draw list (see
  // draw_highlight_if_set()'s doc comment) so it participates in normal
  // window z-ordering instead of always drawing on top of every window --
  // Window/DockSpaceViewport already drew their own highlight box (if set)
  // from inside their own Begin()/End() scope, since by this point their
  // own End()/EndPopup() has already run and there is no longer a current
  // window to attribute a GetWindowDrawList() call to.
  if (!self_reports_rect)
    draw_highlight_if_set(node, last_resolved_rect_min_, last_resolved_rect_max_);

  ImGui::PopID();
  ImGui::PopFont();
}

void imgui_renderer::render_session(const ui_element& root, const context& s) {
  BISON_TRACE_SCOPE("render_session");
  if (!s.style_service) {
    render_node(root, s);
    return;
  }

  // Recompile the bison field map into an ImGuiStyle only when the client
  // has changed the style since the last compiled cache.
  if (s.style_service->is_dirty()) {
    auto compiled = std::make_shared<ImGuiStyle>();
    bool is_light = apply_style_fields(s.style_service->current_style(), *compiled, s.logger_service);
    // Record the resolved theme's declared is_light (see register_theme()'s
    // doc comment) so render functions with access to the session --
    // render_label(), render_text_editor(), ... -- can pick a
    // theme-appropriate color live every frame via
    // style_service::is_light_theme(), rather than any of them baking a
    // color into data once and having it go stale on a later theme change.
    s.style_service->set_is_light_theme(is_light);
    s.style_service->set_renderer_cache(compiled);
  }

  // RAII guard: swap in the cached style, render, restore. static_cast off the
  // shared_ptr<void> rather than static_pointer_cast (which would build a
  // temporary shared_ptr<ImGuiStyle>, i.e. an atomic refcount inc/dec) just to
  // read it -- the style_service keeps the cache alive for the whole call.
  const ImGuiStyle& compiled =
      *static_cast<const ImGuiStyle*>(s.style_service->renderer_cache().get());
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

void imgui_renderer::flush_draw_list(ImDrawList& draw_list, int w, int h) {
  // No backend to submit to; matches begin_render_target()'s "unsupported"
  // contract.
  (void)draw_list;
  (void)w;
  (void)h;
}

} // namespace bdg::wish
