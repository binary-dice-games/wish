// MIT License © 2025 Binary Dice Games
/// @file imgui_layout.cpp
/// @brief Two-pass measure/arrange layout engine for the ImGui renderer.
///
/// See imgui_layout.hpp and src/imgui/DESIGN.md for the architecture. This
/// file is pure geometry computation -- no `Begin`/`BeginChild`, no widget
/// drawing.
#include "imgui_layout.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── Optional frame-to-frame instability log ──────────────────────────────────
//
// Diagnostic aid only, not part of the normal build's behavior: set
// WISH_LAYOUT_DEBUG_LOG=/path/to/file before launching the app to have
// arrange_node() append one line every time a node's resolved position/size
// changes from what it was the *previous* frame by more than half a pixel --
// i.e. every frame where this node's on-screen box actually moved or
// resized, for a reason other than "it was never arranged before". A node
// whose layout is genuinely stable across frames produces no output at all;
// a node logging every single frame (or oscillating between two values) is
// the direct signature of visible flicker -- this exists specifically to let
// a user reproduce a flicker report once with this env var set and hand back
// the resulting file instead of a hard-to-narrate description.
static std::ofstream* layout_debug_log() {
  static std::ofstream* stream = [] {
    const char* path = std::getenv("WISH_LAYOUT_DEBUG_LOG");
    if (!path || !*path)
      return static_cast<std::ofstream*>(nullptr);
    auto* f = new std::ofstream(path, std::ios::app);
    return f->is_open() ? f : (delete f, static_cast<std::ofstream*>(nullptr));
  }();
  return stream;
}

static std::string debug_node_label(const ui_element& node) {
  const auto* path_field = node.findField("__path__"_key);
  if (path_field && path_field->is<std::string>() && !path_field->as<std::string>().empty())
    return path_field->as<std::string>();
  // No "__path__" (e.g. a form-generated TableRow cell -- see stable_id()'s
  // identical fallback in imgui_ui_renderer.cpp): identify by class hash id
  // instead of leaving the log line unattributed.
  auto cls = node.as<key_t>(dynamic::CLASS);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "class#%08x", static_cast<unsigned>(cls.id));
  return buf;
}

// ── Measure dispatch table ───────────────────────────────────────────────────

using measure_fn = natural_size (*)(imgui_renderer&, const ui_element&, const context&);
using measure_fn_map = std::unordered_map<hash_t, measure_fn>;

// Spring has no content at all -- its own size comes 100% from the stretch
// pool division its enclosing Layout gives it, never from anything it would
// render on its own. (This is the one leaf-shaped class that keeps an
// explicit measure_fn: the generic last_rendered_size() fallback below
// would create a feedback loop for Spring specifically, since render_spring()
// always draws Dummy() at exactly its *previous* arranged size -- "what did
// I render last frame" is circular for a widget whose only content is
// whatever this frame's arrange pass decides to give it.)
static natural_size measure_spring(imgui_renderer&, const ui_element&, const context&) {
  return {0.0f, 0.0f};
}

natural_size measure_node(imgui_renderer& r, const ui_element& node, const context& s);

static const measure_fn_map& measure_dispatch_fns();

// Label and Image are registered (unlike most other leaves -- see
// measure_dispatch_fns()'s own comment below for why the rest deliberately
// aren't) because both are frequently torn down and rebuilt from scratch by
// this codebase's "clear children, reinstantiate" list-refresh idiom (e.g.
// file_browser_utils.cpp's make_name_cell(), rebuilt on every navigate/sort/
// select) and both have a size formula that queries exactly the same
// primitive ImGui's own render call uses, computed fresh this frame -- not
// an approximation, and not dependent on the node having rendered before.
// A brand-new node has last_rendered_size() == {0,0} (never rendered), which
// for an icon+label row meant the label was positioned as if the icon had
// zero width while the icon still drew at its real size on top of it --
// visible overlap on every rebuild, not just a one-frame startup glitch,
// because these nodes are genuinely new objects each time, never the same
// instance surviving long enough to "self-correct next frame". Label's
// CalcTextSize() also tracks a live-updating text field exactly (e.g. a
// stats readout whose digits change every refresh), where last_rendered_size
// would otherwise always trail one frame behind the real width.
static natural_size measure_label(imgui_renderer&, const ui_element& node, const context&) {
  auto text = node.get_as<std::string>("text"_key, "");
  ImVec2 sz = ImGui::CalcTextSize(text.c_str());
  return {sz.x, sz.y};
}

// Mirrors render_image()'s own early sizing exactly (imgui_ui_renderer.cpp):
// "__auto_size_to_font__" sizes to the current line height; otherwise the
// declared width/height fields are used verbatim, with 0 meaning "reserves
// no space" (render_image() only calls Dummy() when both are positive).
// Never depends on whether the source file actually resolves/decodes --
// that's runtime-load state, not layout.
static natural_size measure_image(imgui_renderer&, const ui_element& node, const context&) {
  if (node.get_as<bool>("__auto_size_to_font__"_key, false)) {
    float line = ImGui::GetTextLineHeight();
    return {line, line};
  }
  int32_t w = node.get_as<int32_t>("width"_key, 0);
  int32_t h = node.get_as<int32_t>("height"_key, 0);
  return {w > 0 ? float(w) : 0.0f, h > 0 ? float(h) : 0.0f};
}

// Layout::spacing (src/ui/ui_elements/layout.cpp) defaults to 0.0f, the same
// sentinel this codebase's other Layout hints (width/height) use for "no
// explicit opinion, use the default" -- for spacing specifically, "the
// default" is the active theme's own ImGuiStyle::ItemSpacing, exactly what
// plain sequential ImGui widgets get for free from their normal cursor
// advance. Before the two-pass measure/arrange engine, every
// VerticalLayout/HorizontalLayout child was placed via that same natural
// ImGui flow, so an author who never set "spacing" still saw the theme's
// gap; arrange_vertical_layout()/arrange_horizontal_layout() stamping
// absolute positions via SetCursorPos() lost that for free -- a real,
// user-reported regression (buttons/rows touching with the "wish" theme's
// nonzero ItemSpacing/FramePadding/WindowPadding all set). An explicit
// positive "spacing" field still wins outright (it's the literal pixel gap
// the author asked for, not an addition on top of the theme).
static float effective_spacing(const ui_element& node, float axis_item_spacing) {
  float spacing = node.get_as<float>("spacing"_key, 0.0f);
  return spacing > 0.0f ? spacing : axis_item_spacing;
}

static natural_size measure_vertical_layout(imgui_renderer& r, const ui_element& node, const context& s) {
  float spacing = effective_spacing(node, ImGui::GetStyle().ItemSpacing.y);
  float total_h = 0.0f;
  float max_w = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    bool is_spring = child.as<key_t>(dynamic::CLASS) == "Spring"_key;
    natural_size child_sz = measure_node(r, child, s);
    if (!is_spring) {
      float h = child.get_as<float>("height"_key, 0.0f);
      // A stretch child (h < 0) contributes 0 to the parent's own natural
      // height -- it wants to fill whatever's left over, not define it.
      total_h += h > 0.0f ? h : (h < 0.0f ? 0.0f : child_sz.y);
      max_w = std::max(max_w, child_sz.x);
    }
    ++n;
  });
  if (n > 1)
    total_h += spacing * float(n - 1);
  return {max_w, total_h};
}

static natural_size measure_horizontal_layout(imgui_renderer& r, const ui_element& node, const context& s) {
  float spacing = effective_spacing(node, ImGui::GetStyle().ItemSpacing.x);
  float total_w = 0.0f;
  float max_h = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    bool is_spring = child.as<key_t>(dynamic::CLASS) == "Spring"_key;
    natural_size child_sz = measure_node(r, child, s);
    if (!is_spring) {
      float w = child.get_as<float>("width"_key, 0.0f);
      total_w += w > 0.0f ? w : (w < 0.0f ? 0.0f : child_sz.x);
      max_h = std::max(max_h, child_sz.y);
    }
    ++n;
  });
  if (n > 1)
    total_w += spacing * float(n - 1);
  return {total_w, max_h};
}

static natural_size measure_splitter(imgui_renderer& r, const ui_element& node, const context& s) {
  auto orientation = node.get_as<std::string>("orientation"_key, "vertical");
  bool is_vertical = orientation != "horizontal";
  float thickness = std::max(1.0f, node.get_as<float>("thickness"_key, 4.0f));
  key_t size_field = is_vertical ? "width"_key : "height"_key;
  float total = 0.0f;
  float cross = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    natural_size sz = measure_node(r, child, s);
    float own = child.get_as<float>(size_field, 0.0f);
    float extent = is_vertical ? sz.x : sz.y;
    total += own > 0.0f ? own : extent;
    cross = std::max(cross, is_vertical ? sz.y : sz.x);
    ++n;
  });
  if (n > 1)
    total += thickness * float(n - 1);
  return is_vertical ? natural_size{total, cross} : natural_size{cross, total};
}

// Shared by TreeNode/CollapsingHeader: a one-line header (GetFrameHeight())
// plus, when open, its children stacked exactly like a VerticalLayout.
// "Open" uses the same previous-frame "__open__" field render_tree_node()/
// render_collapsing_header() already persist -- this frame's real open
// state isn't knowable before TreeNodeEx()/CollapsingHeader() actually run,
// so (like layout_height_cache before it) this is a deliberate one-frame
// lookback, self-correcting the frame after any toggle.
static natural_size measure_tree_node(imgui_renderer& r, const ui_element& node, const context& s) {
  bool leaf = node.get_as<bool>("leaf"_key, false);
  auto label = node.get_as<std::string>("label"_key, "");
  float header_h = ImGui::GetFrameHeight();
  float header_w = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetTreeNodeToLabelSpacing();

  const auto* open_f = node.findField("__open__"_key);
  bool is_open =
      (open_f && open_f->is<bool>()) ? open_f->as<bool>() : node.get_as<bool>("open"_key, false);

  if (leaf || !is_open)
    return {header_w, header_h};

  float sum_h = 0.0f;
  float max_w = header_w;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    natural_size sz = measure_node(r, child, s);
    sum_h += sz.y;
    max_w = std::max(max_w, sz.x);
  });
  return {max_w, header_h + sum_h};
}

// Only the active tab's content is actually rendered this frame (see
// render_tab_bar()/render_tab_item()) -- measuring just that one, via the
// same previous-frame "__selected__" field render_tab_item() persists,
// mirrors that behavior exactly instead of summing every tab's content.
static natural_size measure_tab_bar(imgui_renderer& r, const ui_element& node, const context& s) {
  float tab_strip_h = ImGui::GetFrameHeight();
  ui_element* selected = nullptr;
  ui_element* first = nullptr;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    if (!first)
      first = &child;
    if (!selected && child.get_as<bool>("__selected__"_key, false))
      selected = &child;
  });
  ui_element* active = selected ? selected : first;
  if (!active)
    return {0.0f, tab_strip_h};

  float sum_h = 0.0f;
  float max_w = 0.0f;
  active->for_each_child_ordered([&](key_t, ui_element& child) {
    natural_size sz = measure_node(r, child, s);
    sum_h += sz.y;
    max_w = std::max(max_w, sz.x);
  });
  return {max_w, tab_strip_h + sum_h};
}

// TableRow's own row height is always GetTextLineHeightWithSpacing() plus
// two CellPadding.y's (see render_table()'s row Selectable, and ImGui's own
// TableNextRow()/TableEndRow(), which extend every row's real footprint by
// style.CellPadding.y on each side beyond the Selectable's own content
// height -- omitting this here previously undercounted every row by ~4px,
// the "few pixels of scrollbar overflow" symptom for any auto-height Table)
// regardless of cell content, so the table's natural height is exact
// arithmetic, not a content measurement -- but only as a *fallback*: an
// explicit positive "outer_width"/"outer_height" (render_table()'s own
// ImGui outer-size params, a different field than the wish Layout
// width/height hint that decides whether this table is an auto/fixed/
// stretch child of its parent) is a deliberate fixed size and must win over
// the row-count arithmetic -- otherwise a Table meant to stay at a fixed
// height (e.g. zip_tool's "outer_height": 300 file listing) would instead
// measure as tall as however many rows it currently has, ballooning its
// unwrapped parent VerticalLayout/HorizontalLayout (which has no
// BeginChild to clip against) far past the window itself. TableColumn/
// TableRow children are never recursed into for the same reason
// arrange_table() below never recurses into them: they're not wish
// width/height-hint-driven.
static natural_size measure_table(imgui_renderer&, const ui_element& node, const context&) {
  bool headers = node.get_as<bool>("headers"_key, false);
  float outer_w = node.get_as<float>("outer_width"_key, 0.0f);
  float outer_h = node.get_as<float>("outer_height"_key, 0.0f);
  float row_h = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().CellPadding.y * 2.0f;
  int32_t row_count = 0;
  float col_w_sum = 0.0f;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    auto cls = child.as<key_t>(dynamic::CLASS);
    if (cls == "TableRow"_key)
      ++row_count;
    else if (cls == "TableColumn"_key)
      col_w_sum += child.get_as<float>("init_width"_key, 0.0f);
  });
  float natural_h = (headers ? row_h : 0.0f) + float(row_count) * row_h;
  return {outer_w > 0.0f ? outer_w : col_w_sum, outer_h > 0.0f ? outer_h : natural_h};
}

// Two kinds of entry here: classes whose own natural size must be known
// *before* anything renders because they distribute space to children
// (VerticalLayout/HorizontalLayout/Splitter/Table) or need to recurse into
// exactly one child ahead of time (TreeNode/CollapsingHeader/TabBar); and
// leaves with a cheap, exact, render-history-independent formula
// (Label/Image -- see their own comments above for why those two
// specifically). Spring keeps its own entry for the circularity reason
// described on measure_spring() above.
//
// Every other leaf (Button, ProgressBar, SliderFloat, Plot, TextEditor, and
// anything added in the future) is intentionally absent: measure_node()'s
// fallback below uses that node's own real, last-rendered size instead, so
// there is no formula to write or keep in sync with render_*() for the
// common case of a widget that is created once and re-rendered many times
// (self-correcting within one frame of any real size change -- see
// ui_element::last_rendered_size()'s doc comment). That fallback is a
// deliberate trade-off, not a free lunch: it is only safe for a node whose
// identity survives across frames. A node that is destroyed and replaced
// by a fresh instance every time its content changes (this codebase's
// "clear children, reinstantiate" list-refresh idiom) never accumulates a
// real last-rendered size to fall back on -- which is exactly what made
// Label/Image worth promoting to real formulas rather than leaving them on
// this fallback.
static const measure_fn_map& measure_dispatch_fns() {
  static const measure_fn_map tbl{
      {"Spring"_key.id, measure_spring},
      {"VerticalLayout"_key.id, measure_vertical_layout},
      {"HorizontalLayout"_key.id, measure_horizontal_layout},
      {"Splitter"_key.id, measure_splitter},
      {"TreeNode"_key.id, measure_tree_node},
      {"CollapsingHeader"_key.id, measure_tree_node},
      {"TabBar"_key.id, measure_tab_bar},
      {"Table"_key.id, measure_table},
      {"Label"_key.id, measure_label},
      {"Image"_key.id, measure_image},
  };
  return tbl;
}

natural_size measure_node(imgui_renderer& r, const ui_element& node, const context& s) {
  if (!node.get_as<bool>("visible"_key, true)) {
    node.set_measured_size({0.0f, 0.0f});
    return {0.0f, 0.0f};
  }

  ImFont* font = resolve_element_font(r, node, s);
  ImGui::PushFont(font);

  auto cls = node.as<key_t>(dynamic::CLASS);
  natural_size sz{0.0f, 0.0f};
  auto it = measure_dispatch_fns().find(cls.id);
  if (it != measure_dispatch_fns().end()) {
    sz = it->second(r, node, s);
  } else {
    // Unregistered class: this node's own real, last-rendered size (see
    // ui_element::last_rendered_size()'s doc comment) -- {0,0} only for a
    // node that has genuinely never been rendered yet, self-correcting the
    // very next frame once it has. Still recurse so any nested Layout gets
    // a fresh stash for its own self-heal regardless.
    sz = node.last_rendered_size();
    node.for_each_child_ordered([&](key_t, ui_element& child) { measure_node(r, child, s); });
  }

  ImGui::PopFont();
  node.set_measured_size(sz);
  return sz;
}

// ── Arrange dispatch table ───────────────────────────────────────────────────

using arrange_fn = void (*)(imgui_renderer&, const ui_element&, ImVec2, ImVec2, const context&);
using arrange_fn_map = std::unordered_map<hash_t, arrange_fn>;

void arrange_node(imgui_renderer& r, const ui_element& node, ImVec2 origin, ImVec2 avail, const context& s);

static const arrange_fn_map& arrange_dispatch_fns();

static void arrange_vertical_layout(imgui_renderer& r, const ui_element& node, ImVec2 origin, ImVec2 avail, const context& s) {
  float spacing = effective_spacing(node, ImGui::GetStyle().ItemSpacing.y);
  struct child_info {
    ui_element* elem;
    float height;
    bool is_spring;
    float spring_weight;
  };
  std::vector<child_info> children;
  float fixed_total = 0.0f;
  float stretch_weight_total = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    bool is_spring = child.as<key_t>(dynamic::CLASS) == "Spring"_key;
    if (is_spring) {
      float weight = std::max(0.0f, child.get_as<float>("weight"_key, 1.0f));
      stretch_weight_total += weight;
      children.push_back({&child, 0.0f, true, weight});
      ++n;
      return;
    }
    float h = child.get_as<float>("height"_key, 0.0f);
    children.push_back({&child, h, false, 0.0f});
    if (h > 0.0f)
      fixed_total += h;
    else if (h < 0.0f)
      stretch_weight_total += -h;
    else
      fixed_total += child.measured_size().y;
    ++n;
  });
  float spacing_total = n > 1 ? spacing * float(n - 1) : 0.0f;
  float stretch_pool = std::max(0.0f, avail.y - fixed_total - spacing_total);

  float y = origin.y;
  bool first = true;
  for (auto& c : children) {
    if (!first)
      y += spacing;
    first = false;

    float row_h;
    if (c.is_spring)
      row_h = stretch_weight_total > 0.0f ? stretch_pool * (c.spring_weight / stretch_weight_total) : 0.0f;
    else if (c.height > 0.0f)
      row_h = c.height;
    else if (c.height < 0.0f)
      row_h = stretch_weight_total > 0.0f ? stretch_pool * (-c.height / stretch_weight_total) : 0.0f;
    else
      row_h = c.elem->measured_size().y;

    // Cross-axis (width): a Spring has no content, so it must never be
    // what drives the row's own size -- 0, not the full row width, same
    // as render_vertical_layout()'s pre-refactor spring stamping.
    float row_w = c.is_spring ? 0.0f : avail.x;
    arrange_node(r, *c.elem, ImVec2(origin.x, y), ImVec2(row_w, row_h), s);
    y += row_h;
  }

  // See layout_stash::content_extent's doc comment: this equals avail.y
  // whenever a stretch/fill child (or a plain sum that happens to reach
  // avail.y) soaks up the remainder, but can be much smaller when none of
  // the children stretch and avail.y itself came from an unbounded ambient
  // context (e.g. self-healing inside a Table cell).
  node.set_content_extent({avail.x, y - origin.y});
}

static void arrange_horizontal_layout(imgui_renderer& r, const ui_element& node, ImVec2 origin, ImVec2 avail, const context& s) {
  float spacing = effective_spacing(node, ImGui::GetStyle().ItemSpacing.x);
  struct child_info {
    ui_element* elem;
    float width;
    bool is_spring;
    float spring_weight;
  };
  std::vector<child_info> children;
  float fixed_total = 0.0f;
  float stretch_weight_total = 0.0f;
  int n = 0;
  node.for_each_child_ordered([&](key_t, ui_element& child) {
    bool is_spring = child.as<key_t>(dynamic::CLASS) == "Spring"_key;
    if (is_spring) {
      float weight = std::max(0.0f, child.get_as<float>("weight"_key, 1.0f));
      stretch_weight_total += weight;
      children.push_back({&child, 0.0f, true, weight});
      ++n;
      return;
    }
    float w = child.get_as<float>("width"_key, 0.0f);
    children.push_back({&child, w, false, 0.0f});
    if (w > 0.0f)
      fixed_total += w;
    else if (w < 0.0f)
      stretch_weight_total += -w;
    else
      fixed_total += child.measured_size().x;
    ++n;
  });
  float spacing_total = n > 1 ? spacing * float(n - 1) : 0.0f;
  float stretch_pool = std::max(0.0f, avail.x - fixed_total - spacing_total);

  float x = origin.x;
  float max_col_h = 0.0f;
  bool first = true;
  for (auto& c : children) {
    if (!first)
      x += spacing;
    first = false;

    float col_w;
    if (c.is_spring)
      col_w = stretch_weight_total > 0.0f ? stretch_pool * (c.spring_weight / stretch_weight_total) : 0.0f;
    else if (c.width > 0.0f)
      col_w = c.width;
    else if (c.width < 0.0f)
      col_w = stretch_weight_total > 0.0f ? stretch_pool * (-c.width / stretch_weight_total) : 0.0f;
    else
      col_w = c.elem->measured_size().x;

    // Cross-axis (height): a Spring contributes 0, same reasoning as
    // arrange_vertical_layout()'s width special-case above. A real column
    // honors its own "height" field (0 = auto, >0 fixed, <0 = fill the row).
    float col_h;
    if (c.is_spring) {
      col_h = 0.0f;
    } else {
      float h_hint = c.elem->get_as<float>("height"_key, 0.0f);
      col_h = h_hint > 0.0f ? h_hint : (h_hint < 0.0f ? avail.y : c.elem->measured_size().y);
    }

    arrange_node(r, *c.elem, ImVec2(x, origin.y), ImVec2(col_w, col_h), s);
    x += col_w;
    max_col_h = std::max(max_col_h, col_h);
  }

  // See layout_stash::content_extent's doc comment (and
  // arrange_vertical_layout()'s identical fixup above): the cross-axis
  // (height) extent is the tallest column actually assigned, not avail.y --
  // a column only reaches avail.y when its own height hint says to (fixed
  // >= avail.y, or an explicit fill), never merely because avail.y was
  // large.
  node.set_content_extent({x - origin.x, max_col_h});
}

static void arrange_splitter(imgui_renderer&, const ui_element& node, ImVec2 origin, ImVec2 avail, const context&) {
  auto orientation = node.get_as<std::string>("orientation"_key, "vertical");
  bool is_vertical = orientation != "horizontal";
  float thickness = std::max(1.0f, node.get_as<float>("thickness"_key, 4.0f));
  key_t size_field = is_vertical ? "width"_key : "height"_key;

  std::vector<ui_element*> panes;
  node.for_each_child_ordered([&](key_t, ui_element& child) { panes.push_back(&child); });
  if (panes.empty())
    return;

  float main_avail = is_vertical ? avail.x : avail.y;
  float usable = std::max(0.0f, main_avail - thickness * float(panes.size() > 1 ? panes.size() - 1 : 0));
  float used = 0.0f;
  for (size_t i = 0; i + 1 < panes.size(); ++i)
    used += std::max(0.0f, panes[i]->get_as<float>(size_field, 0.0f));
  float last_size = std::max(0.0f, usable - used);

  float pos = is_vertical ? origin.x : origin.y;
  int frame = ImGui::GetFrameCount();
  for (size_t i = 0; i < panes.size(); ++i) {
    bool is_last = (i + 1 == panes.size());
    float sz = is_last ? last_size : std::max(0.0f, panes[i]->get_as<float>(size_field, 0.0f));
    ImVec2 pane_origin = is_vertical ? ImVec2(pos, origin.y) : ImVec2(origin.x, pos);
    ImVec2 pane_avail = is_vertical ? ImVec2(sz, avail.y) : ImVec2(avail.x, sz);
    // Panes stay leaves from arrange's perspective (like Table): the
    // pane's own box is stamped directly rather than via arrange_node(),
    // since Splitter's own pane-dividing logic (render_splitter(), not
    // this file) owns everything about a pane's content, unchanged by
    // this refactor.
    panes[i]->set_arranged_rect({pane_origin.x, pane_origin.y}, {pane_avail.x, pane_avail.y}, frame);
    pos += sz + thickness;
  }
}

// Table's own box is already stamped generically by arrange_node() before
// this dispatches -- TableRow/TableColumn children are never wish
// width/height-hint-driven (TableColumn uses "init_width", TableRow uses
// none), so there is nothing further to distribute here. Registered
// explicitly (rather than omitted) so a reader sees this is a deliberate
// no-op, not a missing case -- render_table() still calls ensure_arranged()
// to get Table's own resolved box before BeginTable().
static void arrange_table(imgui_renderer&, const ui_element&, ImVec2, ImVec2, const context&) {}

static const arrange_fn_map& arrange_dispatch_fns() {
  static const arrange_fn_map tbl{
      {"VerticalLayout"_key.id, arrange_vertical_layout},
      {"HorizontalLayout"_key.id, arrange_horizontal_layout},
      {"Splitter"_key.id, arrange_splitter},
      {"Table"_key.id, arrange_table},
  };
  return tbl;
}

void arrange_node(imgui_renderer& r, const ui_element& node, ImVec2 origin, ImVec2 avail, const context& s) {
  if (std::ofstream* log = layout_debug_log()) {
    // Compare against last frame's stash *before* overwriting it. Skip a
    // node's first-ever arrange (has_arranged() false): everything would log
    // once on its very first appearance otherwise, drowning out the signal
    // this exists to surface -- a node whose box keeps changing on frames
    // *after* it already had a real one.
    vec2f prev_pos = node.arranged_pos();
    vec2f prev_size = node.arranged_size();
    constexpr float kEpsilon = 0.5f;
    bool moved = std::abs(prev_pos.x - origin.x) > kEpsilon || std::abs(prev_pos.y - origin.y) > kEpsilon;
    bool resized = std::abs(prev_size.x - avail.x) > kEpsilon || std::abs(prev_size.y - avail.y) > kEpsilon;
    if (node.has_arranged() && (moved || resized)) {
      *log << "[frame " << ImGui::GetFrameCount() << "] " << debug_node_label(node) << " arrange changed:"
           << " pos (" << prev_pos.x << "," << prev_pos.y << ") -> (" << origin.x << "," << origin.y << ")"
           << " size (" << prev_size.x << "," << prev_size.y << ") -> (" << avail.x << "," << avail.y << ")"
           << "\n";
      log->flush();
    }
  }

  node.set_arranged_rect({origin.x, origin.y}, {avail.x, avail.y}, ImGui::GetFrameCount());
  auto cls = node.as<key_t>(dynamic::CLASS);
  auto it = arrange_dispatch_fns().find(cls.id);
  if (it != arrange_dispatch_fns().end())
    it->second(r, node, origin, avail, s);
}

bool ensure_arranged(imgui_renderer& r, const ui_element& node, const context& s) {
  if (node.is_arrange_fresh(ImGui::GetFrameCount()))
    return true;
  measure_node(r, node, s);
  arrange_node(r, node, ImGui::GetCursorPos(), ImGui::GetContentRegionAvail(), s);
  return false;
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
