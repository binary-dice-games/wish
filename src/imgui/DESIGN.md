# src/imgui — Architecture & Design

## Overview

`src/imgui` is wish's Dear ImGui rendering backend: the concrete
implementation of `bdg::wish::renderer` (`src/server/renderer.hpp`) that
turns a `ui_element` tree into actual ImGui widget calls.

```
renderer (abstract, src/server/renderer.hpp)
  |
  +-- imgui_renderer (src/imgui/)          -- headless-safe base: dispatch,
  |     |                                     font/texture caching, no GPU
  |     |                                     backend required
  |     +-- sdl3_renderer (src/sdl/)       -- windowed desktop display
  |     +-- web_renderer  (src/web/)       -- browser canvas via WebSocket
  |
  imgui_layout.{hpp,cpp}                   -- pure geometry: measure/arrange
```

`imgui_renderer::render_node()` dispatches on a node's class (`__class`
field) through a `render_fn_map` (class-id -> function pointer) built once
by `built_in_render_fns()` and copied into each instance, optionally
extended/overridden by a caller-supplied `extra_render_fns` map. Every
`render_*` function lives in `imgui_ui_renderer.cpp` and shares the
signature `void(imgui_renderer&, const ui_element&, const context&)`.
`sdl3_renderer`/`web_renderer` add a real GPU-backed `get_or_load_font()`/
`get_or_load_texture()` and an actual draw-call backend; the dispatch table
and every `render_*` function are shared, unmodified, by all three.

`imgui_layout.{hpp,cpp}` is a separate concern: pure geometry computation
(no `Begin`/`BeginChild`, no widget drawing) that a handful of `render_*`
functions consume for sizing, described below.

---

## Why a measure/arrange pass exists

A File Explorer bug (a gap between panels and the status bar; up to four
stacked scrollbars for a few pixels of overflow) kept recurring because the
underlying sizing model was ad hoc: each container computed its own
available space from `ImGui::GetContentRegionAvail()` independently at
every nesting level, and reserved an *auto* child's height by caching its
**previous frame's** measured size (the now-deleted `context::
layout_height_cache`), because ImGui is immediate-mode and can't know a
child's size before drawing it. Every nesting level's own small estimation
error compounded, and each level's `BeginChild()` independently tripped
ImGui's overflow-scrollbar heuristic.

Since wish (unlike raw ImGui) has the **entire UI tree** available before
any frame is drawn, sizes can instead be computed in an explicit pre-pass,
and rendering just places widgets at already-known coordinates. This is
what `imgui_layout.cpp` implements: **measure** (bottom-up, natural sizes)
then **arrange** (top-down, resolved rects), consumed by `render_*`
functions that used to do their own ad hoc sizing math.

## Measure pass

`measure_node(r, node, s)` computes a node's own natural (intrinsic) size,
recursing into children as needed, and stamps the result onto the node
(`ui_element::set_measured_size()`). It issues only `Calc*`/style-metric
ImGui queries (`CalcTextSize`, `GetFrameHeight[WithSpacing]()`,
`GetTextLineHeightWithSpacing()`, style `FramePadding`/`ItemSpacing`) —
never `Begin`/`BeginChild` — so it runs fresh every frame with no
cross-frame cache and therefore no staleness. It replicates
`imgui_renderer::render_node()`'s font-override resolution
(`font_path`/`font_size` fields) via the shared `resolve_element_font()`
helper (`imgui_renderer.hpp`), so a node's measured size matches the font
it will actually render with — a node with a custom `font_size` inside an
auto-height row must measure against that font's real line height, not the
default font's.

`measure_dispatch_fns()` (function-local `static`, `imgui_layout.cpp`) maps
class id -> measure function for every class confirmed as an
actually-exercised auto (`0`-width/height) Layout child in this codebase:
`Label`, `Button`, `Checkbox`/`RadioButton`, `Separator`, the
`InputText`/`InputInt`/`InputFloat` family, nested `VerticalLayout`/
`HorizontalLayout`, `Splitter`, `Table` (row-height arithmetic, not content
measurement — see below), `TreeNode`/`CollapsingHeader` (recursive, gated
on the previous frame's persisted open state), `TabBar` (recursive into
only the previously-selected `TabItem`'s children, matching
`render_tab_bar`'s own render-only-the-active-tab behavior), and `Spring`
(always `{0,0}`). A class with no entry recurses into its children (so any
nested Layout still gets a fresh stash for its own self-heal) but
contributes `{0,0}` to its own parent's sizing — the extension point for a
future widget with genuinely wrap-dependent content height; none exist in
the current widget set (`TextEditor`/`Plot`/`Plot3D` always have an
explicit or fixed-default height today).

**Known simplification, not a bug**: full CSS-style width-then-height
constraint solving (e.g. a wrapped `Label`'s real height depends on the
width its parent will eventually give it, which isn't known during a
bottom-up pass) is out of scope. `wrap:true` measures the same as
unwrapped text. This matches the pre-refactor behavior for wrapped auto
children and is not a regression.

## Arrange pass

`arrange_node(r, node, origin, avail, s)` stamps `node`'s own resolved rect
(`origin`/`avail`, in the current window's cursor-position space) via
`ui_element::set_arranged_rect()`, then — only for classes in
`arrange_dispatch_fns()` — recursively distributes `avail` to children
using each child's width/height hint: `0` -> the child's own natural size
from the last `measure_node()` call, `+N` -> fixed pixels, `-N` -> an exact
weighted share of remaining space, computed once per level using measure's
exact numbers, never re-estimated.

`arrange_dispatch_fns()` has **exactly four entries**: `VerticalLayout`,
`HorizontalLayout`, `Splitter`, `Table`.

- `VerticalLayout`/`HorizontalLayout` are the real hint-driven distributors
  — the stretch-pool math `render_vertical_layout`/`render_horizontal_layout`
  used to own directly now lives here instead.
- `Splitter`'s panes are still explicit-size-driven children (each pane's
  own `width`/`height` field is its persisted pixel size, the last pane
  always filling the remainder), so it gets real per-pane box computation
  too — but panes are treated as **leaves**: their own content is never
  recursed into by `arrange_node()`, matching `Table` below, since
  `render_splitter()`'s own pane-dividing/drag-bar logic (unchanged by this
  refactor) owns everything about what's inside a pane.
- `Table` is registered but its function body is an intentional no-op:
  `TableRow`/`TableColumn` are never wish width/height-hint-driven
  (`TableColumn` uses its own `init_width` field, `TableRow` uses none), so
  there is nothing to distribute. It stays in the table (rather than being
  omitted, which would behave identically) so a reader sees this is a
  deliberate case, not a forgotten one — `render_table()` still calls
  `ensure_arranged()` to get **Table's own** resolved box before
  `BeginTable()`, which is what lets an `outer_width`/`outer_height` of `0`
  fill an enclosing stretch row instead of collapsing to nothing.

Every other class (`TreeNode`, `TabBar`, `MenuBar`, any leaf) has no entry:
recursion stops there, and that subtree keeps using ImGui's own default
top-to-bottom flow underneath, unmodified by this system. Simulating
ImGui's internal cursor/indent advance for those classes inside the
arrange pass is explicitly out of scope — anything nested inside still
gets correct geometry through `ensure_arranged()`'s self-heal (below) when
it itself is a `VerticalLayout`/etc.

## `ensure_arranged()` — the self-heal mechanism

`render_vertical_layout`/`render_horizontal_layout`/`render_splitter`/
`render_table` each call `ensure_arranged(r, node, s)` at their own top,
before doing anything else. If `node` already carries a stash written
*this same frame* (`ui_element::is_arrange_fresh()`), it's a no-op — some
ancestor's top-down `arrange_node()` pass already resolved it via the hook
points below. Otherwise it runs `measure_node(r, node, s)` then
`arrange_node(r, node, ImGui::GetCursorPos(), ImGui::GetContentRegionAvail(),
s)`, treating `node`'s own live cursor position as a locally-scoped root —
the exact same `arrange_node()` call an ancestor would have made, never a
second algorithm.

This is **load-bearing, not optional**: `tests/test_imgui_renderer.cpp`
has multiple tests (e.g. `VerticalLayoutWithThreeLabelsDoesNotThrow`) that
call `renderer_->render_node()` directly on a bare `VerticalLayout`/
`HorizontalLayout` root wrapped only in a raw `ImGui::Begin("TestWindow")`
— never through wish's own `render_window()` — so the top-down pass
literally never reaches these nodes; only the self-heal path makes them
work at all.

A node's stash also self-heals for a live UI reason, not just tests: a
`Window` that was collapsed or briefly invisible this frame, or a subtree
under a just-collapsed `TreeNode`, never has `arrange_node()` reach it from
above — `ui_element::is_arrange_fresh(current_frame)` comparing against
the frame the stash was last written catches exactly this and forces a
recompute rather than trusting a stale rect from whenever it last rendered.

## Hook points

`render_window()` (`imgui_ui_renderer.cpp`) calls `measure_node()` then
`arrange_node(node, ImVec2(0,0), ImGui::GetContentRegionAvail(), s)`
immediately before each of its two `render_children(r, node, s)` calls
(the modal and non-modal paths), so every top-level window's subtree gets
a fresh top-down pass once per frame before anything under it renders.
`Window`'s own `width`/`height` (`ImGuiCond_FirstUseEver`, live user
resize) stays completely outside this system — genuinely live ImGui/OS
window state, correctly left alone. `TreeNode`/`TabBar`/`Splitter`'s own
internal drag/expand/scroll state is similarly left to ImGui itself; this
system only ever resolves the **rect a child gets**, never how a container
draws or manages itself internally.

## Why geometry lives as native `ui_element` members, not dynamic fields

Resolved geometry (`measured_size`, `arranged_pos`, `arranged_size`,
`arranged_frame`) is stored as plain, typed C++ members directly on
`ui_element` (`src/ui/ui_element.hpp`'s `layout_stash` struct and its
accessor methods), not as generic bison dynamic fields the way
`__wish_spring_w__`/`__wish_win_rect_*__` are. Every wish widget class's
`addClass(...)` registration already passes `dynamic::make_factory<
ui_element>(ns, klass)` as its factory (see `src/ui/ui_elements/*.cpp`), so
every instantiated widget is already a real `ui_element*`, not a generic
`dynamic` — that is precisely the purpose of a class's registered factory:
to specialize the C++ type produced for that class. Transient,
framework-internal state like a one-frame layout stash belongs on the
specialized type itself, plain and typed, with ordinary member access (no
`const_cast`, since the accessors are `const` methods writing through a
`mutable` member), rather than round-tripped through
`std::map<key_t,field>` boxing/hashing — that map stays reserved for
genuinely RMI-visible, per-widget-schema fields (`label`, `checked`,
`value`, ...).

The stash is never preserved across `clone()`:
`cloneable_dynamic<Derived>::clone_ptr()` default-constructs a fresh
`Derived` and only copies `dynamic`'s own field map into it, so a cloned
node always starts with `arranged_frame == -1` (never arranged) until the
next measure/arrange pass runs — exactly the self-heal behavior a stale
clone should have anyway.

## Non-goals

- Full CSS-style width-then-height constraint solving for wrap-dependent
  content (see the measure-pass note above).
- Simulating ImGui's internal cursor/indent advance for `TreeNode`/
  `TabBar`/`MenuBar` inside the arrange pass.
- `stable_id()`'s fallback collision for runtime-constructed nodes with no
  `__path__`/`__wish_id` — a `PushID`/widget-identity issue, unrelated to
  sizing.
