// MIT License © 2025 Binary Dice Games
/// @file imgui_layout.cpp
/// @brief Two-pass measure/arrange layout engine for the ImGui renderer.
///
/// See imgui_layout.hpp and src/imgui/DESIGN.md for the architecture. This
/// file is pure geometry computation -- no `Begin`/`BeginChild`, no widget
/// drawing.
#include "imgui_layout.hpp"

#include "src/rmi/shared/profiling.hpp"

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
float effective_spacing(const ui_element& node, float axis_item_spacing) {
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

// Table's own natural size (when acting as an auto/unhinted child, e.g. to
// feed a sibling Spring's stretch-pool math) is no longer computed by a
// bespoke row-height formula here -- that formula duplicated ImGui's own
// TableNextRow()/TableEndRow() row-height arithmetic (CellPadding.y et al.)
// in a second, physically separate place, which is exactly the kind of
// drift bug this codebase keeps hitting (it once undercounted every row by
// ~4px because CellPadding was missing). Table instead falls through to
// measure_node()'s generic last_rendered_size() fallback below, same as
// TextEditor/Plot/Button and every other composite/leaf without a
// registered measure_fn: one frame of lag on a genuinely brand-new Table,
// self-correcting immediately after, in exchange for zero duplicated math.
// An explicit positive "outer_width"/"outer_height" is unaffected either
// way -- render_table() passes those straight to ImGui::BeginTable()
// regardless of what measure_node() returns for the auto/fallback case.
//
// Exactly four entries: VerticalLayout/HorizontalLayout/Splitter need their
// own natural size *before* anything renders because they distribute space
// to children -- ImGui has no "what would this row's content naturally add
// up to" query, since that's wish's own declarative fixed/stretch/auto
// model, not something ImGui itself has any concept of. Spring keeps its
// own entry for a different reason -- not "ImGui knows this and we don't"
// (Spring has no ImGui content at all to ask about), but circularity: the
// generic last_rendered_size() fallback below would be self-referential for
// Spring specifically, since render_spring() always draws Dummy() at
// exactly its *previous* arranged size (see measure_spring()'s own comment).
//
// Every other class -- Label, Image, TreeNode, TabBar, Table, Button,
// ProgressBar, TextEditor, Plot, and anything added in the future -- is
// intentionally absent: measure_node()'s fallback below uses that node's
// own real, last-rendered size instead (ui_element::last_rendered_size(),
// populated generically by imgui_renderer::render_node() after every real
// render, not per-class code) rather than a hand-written formula
// re-deriving what ImGui already computed correctly itself. This used to be
// a narrower list -- Label/Image/TreeNode/TabBar each had their own formula
// -- but every one of those formulas was a second, physically separate
// place that could (and did) drift out of sync with what its render_*()
// counterpart actually drew (see the Table row-height bug this same
// rationale already applies to). The trade-off is one frame of lag on a
// genuinely brand-new node, self-correcting immediately after -- invisible
// in practice for anything that renders every frame, and no longer a
// correctness problem for a node that's destroyed and recreated every frame
// (e.g. file_browser_utils.cpp's per-row icon+label cells) either: since
// render_vertical_layout()/render_horizontal_layout() render an unhinted
// (auto) sibling via ImGui's own natural cursor flow rather than an
// absolute pre-computed position (see imgui_ui_renderer.cpp), a fresh
// icon's real (nonzero) width is what a following label's SameLine()
// actually lands after, regardless of what this pass measured it as -- the
// one-frame-lag-here risk that made Label/Image worth a bespoke formula no
// longer exists once nothing downstream trusts a stale measurement for
// *positioning*. A stale measurement can still misjudge a Spring's
// stretch-pool share or a grandparent's own natural size by one frame in a
// row that also mixes a brand-new node with a stretch/spring sibling --
// exactly the same one-frame-lag tradeoff every other fallback-eligible
// class already accepts.
static const measure_fn_map& measure_dispatch_fns() {
  static const measure_fn_map tbl{
      {"Spring"_key.id, measure_spring},
      {"VerticalLayout"_key.id, measure_vertical_layout},
      {"HorizontalLayout"_key.id, measure_horizontal_layout},
      {"Splitter"_key.id, measure_splitter},
  };
  return tbl;
}

natural_size measure_node(imgui_renderer& r, const ui_element& node, const context& s) {
  if (!node.get_as<bool>("visible"_key, true)) {
    node.set_measured_size({0.0f, 0.0f}, ImGui::GetFrameCount());
    return {0.0f, 0.0f};
  }

  const char* profiler_marker = nullptr;
  const auto* pm = node.findField("profiler_marker"_key);
  if (pm && pm->is<std::string>() && !pm->as<std::string>().empty()) {
    profiler_marker = pm->as<std::string>().c_str();
  }
  BISON_TRACE_SCOPE(profiler_marker);

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
  node.set_measured_size(sz, ImGui::GetFrameCount());
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
  // large. The main-axis (width) extent is the actual x the cursor reached,
  // not avail.x, for the identical reason.
  node.set_content_extent({x - origin.x, max_col_h});
}

static void arrange_splitter(imgui_renderer& r, const ui_element& node, ImVec2 origin, ImVec2 avail, const context& s) {
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
    // Route through the generic arrange_node() (not a direct
    // set_arranged_rect() stamp) so a pane that is itself a
    // VerticalLayout/HorizontalLayout/Table/Splitter gets its own
    // arrange_fn invoked too, cascading pane_avail down into *its*
    // children and setting its own content_extent -- exactly like
    // arrange_vertical_layout()/arrange_horizontal_layout() already do for
    // every child of theirs, hinted or not. Skipping this (as an earlier
    // version of this function did, stamping the pane directly) leaves a
    // Layout/Table pane's content_extent() at its stale/default value,
    // since nothing else ever calls its arrange_fn -- render_vertical_layout()
    // then sees a false has_self_size, skips its own BeginChild wrap, and
    // every further-nested descendant free-falls into ensure_arranged()'s
    // ambient-GetContentRegionAvail() self-heal instead of the size this
    // Splitter actually allocated it, drifting further off with each
    // nesting level (observed: a pane's own Table ending up taller than
    // the pane itself, and a deeper pane's last child rendering below the
    // window entirely). A plain leaf pane (Label, TreeNode, ...) is
    // unaffected either way, since arrange_node() no-ops for any class
    // without a registered arrange_fn.
    arrange_node(r, *panes[i], pane_origin, pane_avail, s);
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
  int frame = ImGui::GetFrameCount();
  if (node.is_arrange_fresh(frame))
    return true;
  // An enclosing Window/modal's own unconditional top-level measure_node()
  // call (imgui_ui_renderer.cpp's render_window()) already primed this
  // subtree's measured_size stash earlier this frame -- only arrange_node()
  // still needs to run, since Window has no arrange_dispatch_fns() entry
  // and so never arranges descendants itself. Re-measuring here would
  // redundantly re-walk the whole subtree a second time every frame.
  if (!node.is_measure_fresh(frame))
    measure_node(r, node, s);
  arrange_node(r, node, ImGui::GetCursorPos(), ImGui::GetContentRegionAvail(), s);
  return false;
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
