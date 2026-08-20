// MIT License © 2025 Binary Dice Games
/// @file imgui_layout.hpp
/// @brief Two-pass measure/arrange layout engine for the ImGui renderer.
///
/// Pure geometry computation, no widget drawing -- `imgui_ui_renderer.cpp`'s
/// `render_vertical_layout`/`render_horizontal_layout`/`render_splitter`/
/// `render_table` are consumers of this engine, not implementers of their
/// own sizing math. See `src/imgui/DESIGN.md` for the full architecture
/// (measure pass, arrange pass, dispatch tables, the `ensure_arranged()`
/// self-heal mechanism, and why resolved geometry lives as native C++
/// members on `ui_element` rather than generic dynamic fields).
#pragma once

#ifdef WISH_IMGUI_ENABLED

#include <context/context.hpp>
#include <imgui/imgui_renderer.hpp>
#include <ui/ui_element.hpp>

#include <imgui.h>

namespace bdg::wish {

/// @brief A node's own natural (intrinsic) size, as computed by `measure_node()`.
using natural_size = vec2f;

/// @brief Resolves a `VerticalLayout`/`HorizontalLayout` node's effective
///        inter-child gap: its own explicit `"spacing"` field when positive
///        (the literal pixel gap the author asked for), otherwise @p
///        axis_item_spacing (the active theme's `ImGuiStyle::ItemSpacing`
///        for the relevant axis) -- matching what plain sequential ImGui
///        widgets get for free from their own default cursor advance.
///        Shared between `imgui_layout.cpp`'s arrange pass (sizing the
///        stretch pool) and `imgui_ui_renderer.cpp`'s render pass (which
///        pushes this same value as the real `ImGuiStyleVar_ItemSpacing`
///        for natural-flow children), so the two can never disagree about
///        how much visual gap a row/column actually has.
///
/// @param node               The `VerticalLayout`/`HorizontalLayout` node.
/// @param axis_item_spacing  The ambient theme's `ItemSpacing.x` (horizontal)
///                            or `ItemSpacing.y` (vertical) to fall back to.
float effective_spacing(const ui_element& node, float axis_item_spacing);

/// @brief Computes @p node's own natural size, recursing into children as
///        needed, and stamps the result onto @p node via
///        `ui_element::set_measured_size()`.
///
/// Pure computation -- issues only `Calc*`/style-metric ImGui queries
/// (`CalcTextSize`, `GetFrameHeight[WithSpacing]()`,
/// `GetTextLineHeightWithSpacing()`, style `FramePadding`/`ItemSpacing`),
/// never `Begin`/`BeginChild`. Runs fresh every frame for every class with a
/// registered formula (see `imgui_layout.cpp`'s `measure_dispatch_fns()`) --
/// no cross-frame cache, so no staleness there. A leaf class with no
/// registered formula instead falls back to `ui_element::
/// last_rendered_size()`, a deliberate one-frame-lag cache -- see that
/// function's own doc comment for which classes get a real formula and why.
/// Replicates `imgui_renderer::render_node()`'s font-override resolution
/// (`font_path`/`font_size` fields) around its `Calc*` calls via
/// `resolve_element_font()`, so a node's measured size matches what it will
/// actually render at.
///
/// @param r     Renderer instance -- needed for `get_or_load_font()`, which
///              is virtual per-backend (headless/SDL3/web).
/// @param node  Node to measure.
/// @param s     Session context.
/// @return      @p node's own natural size (also stamped onto @p node).
natural_size measure_node(imgui_renderer& r, const ui_element& node, const context& s);

/// @brief Given @p node's actual available rect, recursively distributes
///        space to children using each child's width/height hint and stamps
///        resolved geometry onto each arranged child via
///        `ui_element::set_arranged_rect()`.
///
/// Hint convention: `0` -> the child's own natural size (from the most
/// recent `measure_node()`), `+N` -> fixed pixels, `-N` -> an exact weighted
/// share of remaining space. Computed once per level using measure's exact
/// numbers, never re-estimated. Only classes in this file's internal
/// arrange-dispatch table (`VerticalLayout`/`HorizontalLayout`/`Splitter`/
/// `Table`) actually recurse -- everything else stops here and keeps using
/// ImGui's own default flow underneath.
///
/// @param r       Renderer instance.
/// @param node    Node whose children should be arranged.
/// @param origin  Top-left of @p node's available rect, in the current
///                window's cursor-position space (i.e. suitable for
///                `ImGui::SetCursorPos()`).
/// @param avail   Size of @p node's available rect.
/// @param s       Session context.
void arrange_node(imgui_renderer& r, const ui_element& node, ImVec2 origin, ImVec2 avail, const context& s);

/// @brief Self-heal entry point: ensures @p node's arrange stash is fresh
///        for the current frame before a `render_*` function reads it.
///
/// If @p node already carries a stash written earlier *this same frame*
/// (`ui_element::is_arrange_fresh()`), does nothing -- some ancestor's
/// top-down `arrange_node()` pass already resolved it. Otherwise runs
/// `measure_node(r, node, s)` then `arrange_node(r, node,
/// ImGui::GetCursorPos(), ImGui::GetContentRegionAvail(), s)`, treating
/// @p node's own live cursor position as a locally-scoped root -- the exact
/// same `arrange_node()` call an ancestor would have made, never a second
/// algorithm. This is what makes `VerticalLayout`/`HorizontalLayout`/
/// `Splitter`/`Table` work correctly even when rendered directly (e.g. in a
/// test) without going through `render_window()`'s top-down hook points.
///
/// @param r     Renderer instance.
/// @param node  Node to ensure is arranged.
/// @param s     Session context.
/// @return      True if @p node's stash was already fresh this frame
///              (no-op); false if this call just (re)computed it.
bool ensure_arranged(imgui_renderer& r, const ui_element& node, const context& s);

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
