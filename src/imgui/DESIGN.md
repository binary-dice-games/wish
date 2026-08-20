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

`measure_dispatch_fns()` (function-local `static`, `imgui_layout.cpp`) has
**exactly four entries**: `VerticalLayout`/`HorizontalLayout`/`Splitter`
(need their own natural size *before* anything renders because they
distribute space to children — wish's own declarative fixed/stretch/auto
model, which ImGui itself has no concept of, so there is nothing to "trust
ImGui for" here) and `Spring` (always `{0,0}` — not because ImGui can't be
trusted for it, but because Spring has no ImGui content to measure at all;
the generic fallback below would be self-referential for it specifically,
since `render_spring()` always draws at exactly its *previous* arranged
size).

Every other class — `Label`, `Image`, `TreeNode`, `TabBar`, `Table`,
`Button`, `Checkbox`, `ProgressBar`, `TextEditor`, `Plot`, and any future
addition — has **no** entry. Its natural size instead comes from
`ui_element::last_rendered_size()`, this node's own real ImGui item rect
from the last time it actually rendered (populated generically by
`imgui_renderer::render_node()`'s post-dispatch rect capture, not per-class
code): one frame of lag on a genuinely brand-new node, self-correcting
immediately after. This is a deliberate, load-bearing design choice, not a
shortcut: an earlier version of this file gave nearly every leaf/composite
class its own bespoke formula (`measure_table()`'s row-height arithmetic,
`measure_label()`/`measure_image()`, `measure_tree_node()`/
`measure_tab_bar()`, ...), and every one of those formulas is a second,
physically separate place that can (and did) drift out of sync with what
its `render_*()` counterpart actually drew — `measure_table()` once
undercounted every row by `2*CellPadding.y`, the exact "few pixels of
scrollbar overflow" class of bug this whole engine exists to prevent. A
hand-written formula duplicating math ImGui already computes correctly
itself is a liability, not a convenience — the fallback trades one frame of
lag (invisible in practice for a widget that renders every frame) for zero
duplicated math, and is automatically correct for every widget class,
present and future, with no per-class maintenance.

The one case this trade-off might look unsafe at first — a node whose
*identity* doesn't survive across frames (this codebase's "clear children,
reinstantiate" list-refresh idiom, e.g. a file listing's per-row
icon+filename cell, rebuilt fresh on every navigate/sort/select) never
accumulates a real last-rendered size to fall back on, so a naive reading
would expect a permanently-stale `{0,0}` measurement — turns out not to
matter for what's actually visible: the *arrange* pass can under-measure
such a node indefinitely, but nothing downstream trusts that measurement
for *positioning* an unhinted sibling. See "Draw pass" below —
`render_vertical_layout()`/`render_horizontal_layout()` place an auto
child via ImGui's own real cursor advance (`SameLine()`/default stacking),
not a pre-computed arranged position, so a fresh icon's real drawn width is
what a following label's `SameLine()` actually lands after, regardless of
what this frame's measure pass guessed. A stale measurement can still
misjudge a `Spring`'s stretch-pool share or a grandparent's own natural
size by one frame in a row that also mixes a brand-new node with a
stretch/spring sibling — the same one-frame-lag trade-off every
fallback-eligible class already accepts, and only ever a sizing nuance, not
a visible overlap.

**"Self-correcting immediately after" depends on `last_rendered_size()`'s
own update rule getting this right, not just on there being a real render
to pick up.** `imgui_renderer::render_node()` only overwrites
`last_rendered_size()` when `ImGui::IsItemVisible()` is true for this
frame's dispatch **or** the node has no prior confirmed-good value yet
(still reading the `{0,0}` bootstrap default) — see that function's own
doc comment for the full reasoning. Both halves matter: refusing an update
while invisible protects an existing good value from a stale/misattributed
rect (e.g. a widget whose own `ItemAdd()` never ran at all because its
enclosing `BeginChild()` was itself entirely clipped — `window->SkipItems`
short-circuits before any real bb is computed); but refusing *every*
update while invisible, with no exception for a node that has never had a
good value at all, creates a real deadlock rather than a one-frame lag:
`message_box.cpp`'s icon+message row, built fresh every time the dialog
opens inside an `AlwaysAutoResize` `Window`, put the message `Label`'s
first-ever real render partially outside the window's still-too-small
(brand-new-id, no prior content-size data) clip rect — and since nothing
ever gave the fallback its first real number, the row's arrange (hence the
window's own auto-fit) could never grow to the label's real width on *any*
later frame either, no matter how many more frames rendered. The dialog
opened permanently too small with its message text invisible. The
first-measurement exception breaks the cycle: with nothing yet to protect,
this frame's freshly-computed rect is trusted even while reported
invisible (which, per `ImGui::ItemAdd()`'s own source, is a real, accurate
bb almost all of the time — clip status and rect *correctness* are
different questions).

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

## Draw pass — how `render_vertical_layout`/`render_horizontal_layout` consume the stash

The arrange pass only ever needs to know, for each child, whether it has an
explicit `+N`/`-N` hint at all — not *where* to put it. Positioning is left
entirely to ImGui's own immediate-mode cursor advance, the same way any
hand-written sequence of ImGui calls would work:

- `render_vertical_layout()`/`render_horizontal_layout()` open **one**
  `ImGui::BeginChild()` for their *entire* child set (not one per hinted
  child — see below), sized to `node.content_extent()` (not
  `arranged_size()` — the distinction matters and is explained under
  "Why `content_extent`, not `arranged_size`" below), with
  `ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse` and
  **no** `ImGuiChildFlags_AlwaysUseWindowPadding`: a wish Layout is a
  transparent flex-style container, not a bordered panel, so `WindowPadding`
  deliberately does not apply inside one (that would shift every child's
  position away from the panel's own reported edges, and is a distinct,
  separate concept from `ItemSpacing`, which *does* apply — see below).
  This single wrap is also what lets the node self-report its own rect via
  `GetWindowPos()`/`GetWindowSize()` (`report_self_rect()`, the same idiom
  `Window`/`DockSpaceViewport` already use — see `self_reports_rect` in
  `imgui_renderer.cpp`), replacing an older `BeginGroup()`/trailing-`Dummy()`
  rect-capture approximation entirely.
- Each child is then handled one of three ways:
  - **A `Spring`**: sized by the same stretch-pool arithmetic the arrange
    pass always computed (ImGui has no "fill remaining space, proportionally
    weighted" primitive, so this one piece of math is unavoidable), drawn via
    `ImGui::Dummy(computed_size)`.
  - **An auto (`hint == 0`) child**: `r.render_node(child, s)` directly, no
    position, no wrap. ImGui's own default top-to-bottom stacking
    (`VerticalLayout`) or an explicit `ImGui::SameLine()` before every
    non-first child (`HorizontalLayout`) places it — which is what makes
    `ItemSpacing` apply for free, instead of needing to be re-derived as a
    position offset the way an earlier version of this engine did.
  - **A fixed/stretch (`hint != 0`) child**: wrapped in its own inner
    `BeginChild(child_id, computed_size, ImGuiChildFlags_None, NoScrollbar|
    NoScrollWithMouse)`, so nested "fill available width/height" fields
    (`Table.outer_height:0`, `InputText.width:-1`, ...) resolve against this
    child's own box instead of the whole row/column's content region. This
    is the one case that still needs an explicit wrap, for the same reason
    stretch/fixed sizing needs arrange math at all: most leaf classes
    (`Button`, `Selectable`, `ProgressBar`, `InputText`, `Table`,
    `TextEditor`, `Plot`/`Plot3D`) already accept a size directly into their
    own field/ImGui call and need no help from this engine for a **fixed**
    hint — only a **stretch** hint, or any hint at all on a composite child
    with no native size parameter (nested `VerticalLayout`/`HorizontalLayout`,
    `TreeNode`, `TabBar`), genuinely has nowhere else to put a computed pixel
    number.
- A degenerate (`<=0` on either axis) size — for either the whole node's own
  wrap or a single hinted child — skips the `BeginChild()` entirely and
  renders directly instead. `ImGui::BeginChild()` treats a `0`/negative
  component as "fill the parent's remaining space" (`CalcItemSize()`'s
  `size == 0` branch), **not** "auto-size to nothing" — handing it a literal
  `0` would balloon an empty/zero-content panel out to whatever's left in
  its ambient container.
- An explicit `"spacing"` field override becomes a single
  `ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ...)` scoped tightly around
  this node's own direct children (pushed *after* the outer `BeginChild()`
  and popped *before* its `EndChild()`) — pushed unconditionally, using
  `effective_spacing()` (shared between `imgui_layout.cpp`'s arrange pass and
  this render pass so the two can never disagree about the actual gap),
  rather than only when an override is present, so a *nested* Layout with no
  override of its own doesn't inherit an ancestor's explicit value instead of
  the active theme's default.

### Why `content_extent`, not `arranged_size`

`arranged_size()` is "how much space this node was *given*" — its own
row/column allocation from a real parent hint, or, for a self-healed root,
whatever the ambient `GetContentRegionAvail()` happened to be. Those are the
same number whenever a stretch/`Spring` child exists to soak up the
remainder (the common case), but can diverge hugely when none of a node's
children stretch and the ambient avail came from an unbounded context — the
canonical case being `file_browser_utils.cpp`'s per-row icon-then-label
`HorizontalLayout`, self-healing directly inside a `Table` cell, where
`GetContentRegionAvail()` reports "whatever's left in the whole scrollable
table region", not "this one row's height". Sizing this node's own
`BeginChild()` wrap to `arranged_size()` in that case would balloon the row's
panel out to the table's entire remaining scroll area instead of hugging its
actual (small) content. `content_extent()` — computed by the arrange pass
as the real sum/max of every child's *actually assigned* size, springs
included — equals `arranged_size()` exactly whenever a stretch/fill child
exists (by construction: the stretch pool is defined as
`avail - fixed_total - spacing_total`, so summing every child's real share
always reaches `avail` again), and equals the node's own natural
`measured_size()` otherwise. Using it unconditionally is correct in both
cases, without needing to detect which one applies.

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

## `stable_id()`'s three-tier fallback

`stable_id()` (`imgui_ui_renderer.cpp`) is the identity every `BeginChild()`
id and `PushID()` scope in this file is built from: `__path__` when set (a
template-assigned dot-path, stable across a full process restart), else
`__wish_id` when nonzero (assigned by the server's object-registration
system), else — as of the "Draw pass" redesign above always wrapping a
`VerticalLayout`/`HorizontalLayout`'s own content in a real `BeginChild()`
— this node's own address.

That third tier used to return a fixed literal (`"0"`) instead. It was
harmless as long as nothing gave such a node its own `BeginChild()`: the
pre-redesign render path only ever wrapped a *hinted* child, and a
form-generated node with neither `__path__` nor `__wish_id` (e.g.
`file_browser_utils.cpp`'s `make_name_cell()` — every file-listing row's
icon+filename `HorizontalLayout`, rebuilt fresh on every navigate/sort/
select) is always an *auto* (unhinted) child, so it was never wrapped
either. Once every `VerticalLayout`/`HorizontalLayout` wraps its *own*
content unconditionally, every sibling row's instance hit the same literal
id within the same frame — confirmed live as a real, visible bug (not just
a theoretical one): file_explorer's entire file-listing name column
silently failed to render, because N different `ui_element` instances were
all opening/closing the *same* real ImGui child window by id within one
frame. A node's own address is stable for exactly as long as the node
itself is (the whole lifetime a single frame's render needs, and — for a
node that legitimately persists across frames, e.g. a real `Window` — for
however long that persists too), and is guaranteed unique among every
sibling alive the same frame, which is exactly the property this fallback
tier needs. It is not stable across a rebuild of the same logical row (a
fresh `make_name_cell()` call returns a new object at a new address) — an
acceptable trade-off, since these nodes have no persisted per-id ImGui
state worth keeping across a rebuild anyway (no scrolling, no
open/collapsed state, nothing `NoScrollbar`-flagged `BeginChild()` calls
here track).

## Non-goals

- Full CSS-style width-then-height constraint solving for wrap-dependent
  content (see the measure-pass note above).
- Simulating ImGui's internal cursor/indent advance for `TreeNode`/
  `TabBar`/`MenuBar` inside the arrange pass.
