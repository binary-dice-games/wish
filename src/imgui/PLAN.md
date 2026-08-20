# PLAN.md — Two-pass measure/arrange layout engine for the ImGui renderer

## Introductory note

No `src/imgui/DESIGN.md` exists yet; step 0 below creates one from this
plan's Architecture section, and every step from 1 onward should be read
against it. If a step's implementation reveals that DESIGN.md needs
adjusting, update it before diverging, per the repo's `CLAUDE.md` DESIGN.md
maintenance rule.

### Why this refactor

The File Explorer bug (gap between panels and the status bar; up to 4 stacked
scrollbars for a few pixels of overflow) was patched twice already —
`ImGuiChildFlags_AutoResizeY` and then `ImGuiWindowFlags_NoScrollbar` on the
wrapping `BeginChild()` calls in `render_vertical_layout()`/
`render_horizontal_layout()` (`src/imgui/imgui_ui_renderer.cpp`). Both patches
treated a symptom: the current sizing system computes each container's
available space from `ImGui::GetContentRegionAvail()` independently at every
nesting level, and reserves an *auto* child's height by caching its
**previous frame's** measured size (`context::layout_height_cache`,
`src/context/context.hpp:204-218`) because ImGui is immediate-mode and can't
know a child's size before drawing it. Every nesting level's own small
estimation error compounds, and each level's `BeginChild()` independently
trips ImGui's overflow-scrollbar heuristic.

Since wish (unlike raw ImGui) already has the **entire UI tree** available
before any frame is drawn, sizes can be computed in an explicit pre-pass, then
rendering just places widgets at already-known coordinates. The repo's own
root `DESIGN.md` (line 178) already anticipated this: *"A backend that is not
immediate-mode would implement the same two layout types as a two-pass
measure-then-place operation."* This plan builds that, and also (per explicit
scope decision) unifies the inconsistent width/height sentinel conventions
across widget types onto one contract (`0`=auto, `+N`=fixed px, `-N`=stretch/
fill) as Part 2.

### Architecture: measure → arrange → draw

New files: **`src/imgui/imgui_layout.hpp`/`.cpp`**, added to `CMakeLists.txt`
next to `src/imgui/imgui_plot_renderer.cpp` (both `WISH_IMGUI_ENABLED`-guarded
in the same `target_sources` block, `CMakeLists.txt:396-414`). Pure geometry
computation, no widget drawing — `imgui_ui_renderer.cpp` becomes a consumer of it.

**Measure pass** (`measure_node()`, bottom-up, pure `Calc*`/style-metric
queries — `CalcTextSize`, `GetFrameHeight[WithSpacing]()`,
`GetTextLineHeightWithSpacing()`, style `FramePadding`/`ItemSpacing` — no
`Begin`/`BeginChild`): computes each node's natural/intrinsic size,
recursing into children. Runs fresh every frame — no cross-frame cache, so no
staleness. Must replicate `render_node()`'s font-override resolution
(`font_path`/`font_size` fields, `PushFont`/`PopFont`) around its `Calc*`
calls so a node's measured size matches what it will actually render at —
this is a required correctness step, not a footnote.

**Arrange pass** (`arrange_node()`, top-down, pure arithmetic): given a root's
actual available rect, recursively distributes space to children using each
child's width/height hint — `0` → its own measured natural size, `+N` →
fixed, `-N` → exact weighted share of remaining space, computed once per
level using measure's exact numbers, never re-estimated. Resolved geometry is
stored as **native C++ members directly on `ui_element`**
(`src/ui/ui_element.hpp`), not as generic bison dynamic fields: every widget
class's `addClass(...)` registration already passes
`dynamic::make_factory<ui_element>(ns, klass)` as its factory (confirmed for
`Button`/`VerticalLayout`/etc. in `src/ui/ui_elements/*.cpp`), so every
instantiated widget is already a real `ui_element*`, not a generic `dynamic`.
That is precisely the purpose of a class's registered factory — to
specialize the C++ type produced for that class — so transient,
framework-internal state like this belongs on the specialized type itself,
plain and typed, rather than round-tripped through `std::map<key_t,field>`
boxing/hashing (that map stays reserved for genuinely RMI-visible,
per-widget-schema fields such as `label`/`checked`/`value`).

Dispatch tables mirror `imgui_renderer.cpp`'s existing `built_in_render_fns()`
pattern:
- `arrange_dispatch_fns()`: **exactly four entries** — `VerticalLayout`,
  `HorizontalLayout`, `Splitter`, `Table`. `VerticalLayout`/`HorizontalLayout`
  are the real hint-driven distributors; `Splitter` computes real per-pane
  boxes from each pane's own explicit size field but treats pane content as a
  leaf (never recurses into it, matching `Table` below — `render_splitter()`'s
  own pane-dividing/drag-bar logic owns everything about a pane's content,
  unchanged by this refactor). `Table`'s entry is registered but its function
  body is an intentional no-op: `TableRow`/`TableColumn` are never wish
  width/height-hint-driven (`TableColumn` uses its own `init_width` field,
  `TableRow` uses none), so there is nothing to distribute — it stays in the
  table (rather than being omitted, which would behave identically) purely so
  a reader sees this is deliberate, not forgotten; `render_table()` still
  calls `ensure_arranged()` to get **Table's own** resolved box before
  `BeginTable()`, which is what step 6's fill-semantics fix needs. Everything
  else (`TreeNode`, `TabBar`, `MenuBar`, any leaf) has no entry — recursion
  stops there and that subtree keeps using plain ImGui default flow,
  untouched.
- `measure_dispatch_fns()`: **superseded by a generic fallback — see
  "Revision: `last_rendered_size()` replaces per-leaf measure_fns" below.**
  What follows is the *original* design (every leaf widget got a bespoke
  formula function); it shipped, caused several real bugs from formula/
  render drift, and was deliberately replaced. Kept here for history: `Label`,
  `Button`, `InputText`-family, `Checkbox`/`RadioButton`, `Separator`, nested
  `VerticalLayout`/`HorizontalLayout`, `Splitter`, `Table` (header + rows ×
  row-height arithmetic), `TreeNode`/`CollapsingHeader` (recursive, open
  state only), `TabBar` (recursive into the selected `TabItem` only —
  matches current render behavior), `Spring` (always `{0,0}`).
- **Default fallback** for both tables (an unregistered class): `arrange_fn`
  is simply absent (recursion stops) — unchanged by the revision below.
  `measure_fn`'s fallback is what changed; see that section.

**`ensure_arranged(node, s)`** — the self-heal mechanism, called at the top
of `render_vertical_layout`/`render_horizontal_layout`/`render_splitter`/
`render_table`: if `node` already carries a stash freshly written *this
frame* (see staleness field below), does nothing (some ancestor's top-down
pass already resolved it). Otherwise runs `measure_node(node, s)` then
`arrange_node(node, ImGui::GetCursorPos(), ImGui::GetContentRegionAvail())`
treating `node`'s own live cursor position as a locally-scoped root — the
exact same `arrange_node()` call an ancestor would have made, never a second
algorithm. **This is load-bearing, not optional**: confirmed via
`tests/test_imgui_renderer.cpp` (e.g. `VerticalLayoutWithThreeLabelsDoesNotThrow`,
line 688) that existing tests call `renderer_->render_node()` directly on a
bare `VerticalLayout`/`HorizontalLayout` root wrapped only in a raw
`ImGui::Begin("TestWindow")` (`in_window()` helper, `tests/test_imgui_renderer.cpp:60-70`)
— **never** through wish's own `render_window()` — so the top-down pass
literally never reaches these nodes; only the self-heal path makes them work.

**Layout stash** — a `layout_stash` struct (plain `float`s/`int`, no boxing)
added as a `mutable` member of `ui_element`, all written exclusively by
`imgui_layout.cpp` through `ui_element`'s own accessor methods (the
`mutable` member plus `const` setters means no `const_cast` is needed the
way `report_self_rect()`'s `__wish_win_rect_*__` writes currently require):

| Member | Type | Meaning |
|---|---|---|
| `measured_size` (`{w, h}`) | float pair | Node's own natural size |
| `arranged_pos` (`{x, y}`) | float pair | Resolved top-left, relative to *parent's* content-region cursor-start origin — feeds directly to `ImGui::SetCursorPos()` |
| `arranged_size` (`{w, h}`) | float pair | Resolved content box size |
| `arranged_frame` | int | `ImGui::GetFrameCount()` at write time — lets `ensure_arranged()` distinguish "fresh this frame" from "stale leftover" (a node whose `Window` ancestor was collapsed/invisible this frame, or sits under a just-collapsed `TreeNode`, must self-heal rather than trust an old rect) |

`ui_element` gains methods that own this state and its staleness logic,
keeping `imgui_layout.cpp` as a pure consumer rather than reaching into raw
fields:
- `ImVec2 measured_size() const;` / `void set_measured_size(ImVec2) const;`
- `ImVec2 arranged_pos() const;`, `ImVec2 arranged_size() const;`,
  `void set_arranged_rect(ImVec2 pos, ImVec2 size, int frame) const;`
- `bool is_arrange_fresh(int current_frame) const;` — the one-line staleness
  check `ensure_arranged()` calls, instead of comparing a raw field against
  `GetFrameCount()` inline.

Automation's `get_widget()` (which today reads `__wish_win_rect_*__` via
generic `findField()`) exposes the measured/arranged geometry the same way
it already special-cases window rect: `dynamic_cast<const ui_element*>`
then call the new accessors, not a field lookup — keeping framework-internal
geometry off the generic/RMI field path.

**Hook points**: in `render_window()` (`imgui_ui_renderer.cpp`), immediately
before each of its two existing `render_children(r, node, s)` calls (line
~137 modal path, ~241 normal path), insert `measure_node(node, s);
arrange_node(node, ImVec2(0,0), ImGui::GetContentRegionAvail());`. `Window`'s
own `width`/`height` (`ImGuiCond_FirstUseEver`, live user-resize) stays
completely untouched — genuinely live ImGui/OS-window state, correctly left
outside this system.

---

## Part 1 — Layout engine

**Status: shipped.** Steps 0–7 were implemented in one pass rather than as
staged, separately-reviewed diffs (steps 3/4/5's Spring migration was folded
into steps 3/4 directly, since `arrange_vertical_layout`/
`arrange_horizontal_layout` already stamp every child's — including a
Spring's — arranged rect unconditionally, making a separate migration step
free once the engine exists). Step 8's automation sweep covered all seven
apps that actually exist in this checkout (`calculator`, `file_explorer`,
`log_tail`, `notepad`, `pix`, `process_explorer`, `zip_tool` — `dev/editor`/
`dev/git` from the original module list don't exist yet); the root
`DESIGN.md` update and step 8b (automation `get_widget()` exposure) are not
yet done. Two real bugs were found and fixed during that automation sweep,
beyond what this plan originally scoped:

1. **Coordinate-space double-counting across nested `BeginChild()`s.**
   `arrange_node()`'s cascade computes every descendant's position in one
   coordinate frame rooted at the outermost `ensure_arranged()` call, but
   rendering re-enters a *fresh* local frame at every nested `BeginChild()`
   this refactor introduces (for any child with a nonzero width/height
   hint) — `ImGui::SetCursorPos()` always interprets its argument relative
   to whichever frame is current. Using each child's raw, globally-cascaded
   `arranged_pos()` directly as the `SetCursorPos()` argument made offsets
   compound once per nesting level: three levels deep (file_explorer's real
   `main → panels → right → right_header` tree) pushed a widget's on-screen
   position over 1000px outside a 920px-wide window. Fixed in both
   `render_vertical_layout`/`render_horizontal_layout` by anchoring each
   child to `(current live cursor position) + (child's arranged position −
   this node's own arranged position)` instead of the child's raw
   position — see the "Pin the cursor"-adjacent comments in
   `imgui_ui_renderer.cpp`. Covered by
   `DeeplyNestedStretchLayoutsStayWithinWindowBounds` in
   `test_imgui_renderer.cpp`.
2. **`measure_table()` ignored an explicit `outer_width`/`outer_height`.**
   It computed a Table's natural size purely from row-count arithmetic,
   even when the app had fixed the table's own render-time outer size
   explicitly (e.g. zip_tool's `"outer_height": 300` file listing). An
   unwrapped parent `VerticalLayout` (no `BeginChild` to clip against) then
   measured as tall as however many rows the table currently had, pushing
   later siblings (a button row) far past the window and into overlapping
   the table's own correctly-clipped content. Fixed by preferring an
   explicit positive `outer_width`/`outer_height` over the row-count
   fallback. Covered by `MeasureTableRespectsExplicitOuterHeightOverRowCount`
   in `test_imgui_layout.cpp`.

3. **A leftover one-`style.ItemSpacing.y` overshoot in every unwrapped
   container's own reported height.** `imgui_renderer::render_node()`'s
   generic `BeginGroup()`/`EndGroup()` rect-capture wrap (needed for
   automation/highlight, `needs_group_wrap`) submits its own trailing
   zero-size `Dummy()` immediately before `EndGroup()` — a defensive
   workaround there for a stale-rect fallback in ImGui's own
   `EndGroup()`/`EndTable()` interplay. That `Dummy()` lands wherever the
   cursor naturally sits after the node's last child renders, which is one
   `style.ItemSpacing.y` *past* the true content bottom (ImGui's normal
   post-item cursor advance, reserved for wherever the next sibling would
   start) — regardless of whether that last child was itself
   `BeginChild()`-wrapped. `EndGroup()` folds that inflated position into
   the group's own reported height, which for a node with no `BeginChild()`
   between it and an ancestor `Window` genuinely extends that `Window`'s
   own scrollable content region by one `ItemSpacing.y` — a small but real,
   visible spurious scrollbar, the same symptom class this whole refactor
   exists to eliminate, just narrowed from dozens of pixels to a handful.
   Fixed by `pin_cursor_to_arranged_bottom()` (`imgui_ui_renderer.cpp`),
   called at the end of `render_vertical_layout()`/
   `render_horizontal_layout()`: it lands the cursor one `ItemSpacing.y`
   *short* of the arranged bottom and submits its own `Dummy()` there, so
   that `Dummy()`'s automatic post-item advance puts the cursor exactly on
   the true bottom — which is where the outer caller's own trailing
   `Dummy()` then submits, instead of one hop further out. Verified by
   tightening `SpringMirrorsVerticalLayout`'s tolerance from an explicit
   3.0f workaround back down to 0.5f (essentially exact) once this landed.
4. **`Image`'s internal `"__auto_size_to_font__"` escape hatch wasn't
   measured.** `file_browser_utils.cpp`'s `make_name_cell()` (every
   file_explorer/file_dialog row's icon-then-filename cell) deliberately
   keeps an icon `Image`'s own `width`/`height` fields at `0` while still
   rendering it at a real nonzero size (`GetTextLineHeight()`) computed
   from the active font at render time — see that field's own doc comment
   in `imgui_ui_renderer.cpp` for why it's a field rather than a stamped
   pixel value. `Image` had no `measure_fn` entry at all, so it measured as
   `{0,0}`, allocating the icon zero column width in its enclosing
   `HorizontalLayout` — the following filename `Label`'s arranged x landed
   right on top of where the icon actually draws, visually overlapping the
   icon and the first character or two of the filename in every row of
   every file listing. Fixed by adding `measure_image()`, mirroring
   `render_image()`'s own early-out conditions (including the
   `__auto_size_to_font__` override) exactly so measure and render can
   never disagree. Covered by three tests in `test_imgui_layout.cpp`
   (`MeasureImageWithAutoSizeToFontReturnsTextLineHeight`,
   `MeasureImageWithEmptySrcStillReturnsZero`,
   `IconThenLabelRowDoesNotOverlap`) — note `__auto_size_to_font__` must be
   set via direct C++ field assignment in a test, not through
   `import_json()`, since `ui_descriptor.cpp` deliberately skips any
   leading-double-underscore key as "reserved bookkeeping that should never
   come from a hand-authored descriptor" — a real trap during this
   specific debugging session, since a test written with the field inside
   the JSON string silently no-ops instead of failing loudly.

Also found and fixed along the way: unconditionally wrapping every Layout
child in a `BeginChild` (as step 3/4's "always now" language originally
proposed) broke `ContextMenu`'s "attach to the preceding sibling's last
item" behavior, since a `BeginChild` counts as its own ImGui item. Fixed by
only wrapping children with a nonzero explicit width/height hint, matching
the pre-refactor behavior for auto children.

All four bugs were caught by driving the *actual* apps end-to-end via
`AutomationClient` (`bindings/python/wish/automation.py`) against a real
`wish server --renderer web` — not by the gtest suite alone, which stayed
green throughout every regression. `measure_dispatch_fns()`'s coverage was
audited against every widget "type" actually used anywhere in `modules/`
and `src/ui/forms/` (`grep -rohE '"type"\s*:\s*"[A-Za-z0-9_]+"'`), not just
the plan's originally-scoped list — `Combo`, `ProgressBar`, and `Image`
were the gaps that turned up, followed by a second pass auditing *every*
registered "wish" UI class (`grep -rohE '"[A-Za-z0-9_]+"_rkey' src/ui/
ui_elements/*.cpp ...`), which found `Plot3D`, `TextEditor`, and the whole
`SliderFloat`/`SliderInt`/`DragFloat`/`DragInt`/`ColorEdit` family too.

### Revision: `last_rendered_size()` replaces per-leaf `measure_fn`s

Registering one more widget class at a time (six bugs deep — `Combo`,
`ProgressBar`, `Image`, `Plot`, `Plot3D`, `TextEditor` — before this
revision) is a fundamentally fragile design: `measure_dispatch_fns()` is a
manually-maintained allowlist duplicating each widget's own render-time
sizing formula in a second, physically separate function, with nothing
enforcing that the two stay in sync. Every bug above was exactly that: a
leaf whose real size differs from a hardcoded default (`ProgressBar`'s
`width:-1`, `Image`'s `__auto_size_to_font__`, `Plot`/`Plot3D`/`TextEditor`'s
non-zero fixed defaults) got silently measured as `{0,0}` because nobody
had (yet) written and registered its formula. There is no way to audit this
complete — the next new widget, or the next widget used in a
not-yet-exercised way, reintroduces the same bug class by construction.

The fix: `imgui_renderer::render_node()` already computes each node's real
ImGui item rect generically after every dispatch (`last_resolved_rect_min_`/
`max_`, used for automation/highlight) — this is "ask ImGui for the size" in
exactly the sense a two-pass predictive measure was trying to avoid needing
per-widget replication for. That rect is now *also* stashed onto the node
itself (`ui_element::last_rendered_size()`, populated by one generic line in
`render_node()`, no per-class code). `measure_node()`'s fallback for any
class with no registered `measure_fn` changed from "assume `{0,0}`" to "use
this node's own `last_rendered_size()`" — self-correcting within one frame
of any real size change, exactly like the original pre-refactor
`layout_height_cache`'s accepted one-frame-lag tradeoff, but automatically
correct for every widget class, present and future, with zero maintenance.

This let all ~17 leaf `measure_fn`s (`measure_label`, `measure_button`,
`measure_checkbox`, `measure_separator`, `measure_input_text`,
`measure_image`, `measure_single_line_field`, `measure_progress_bar`,
`measure_plot`, `measure_plot3d`, `measure_text_editor`) be deleted outright.
`measure_dispatch_fns()` now holds exactly the classes whose own natural
size must be known *before* anything renders because they distribute space
to children or need to recurse into exactly one child ahead of time:
`VerticalLayout`, `HorizontalLayout`, `Splitter`, `Table`,
`TreeNode`/`CollapsingHeader`, `TabBar`, and `Spring` (kept as an explicit
`{0,0}` — the generic fallback would be circular for it, since
`render_spring()` always draws at exactly its *previous* arranged size).

A second, related bug turned up in the same pass: `pin_cursor_to_arranged_
bottom()` (the step-3/4 fixup for the residual one-`ItemSpacing.y`
scrollbar overshoot) used `arranged_size()` — "how much space this node was
*given*" — as if it were "how much its content actually used". For a node
with a filling stretch/fill child (the common case, e.g. `main`'s own
stretch `panels` row) those are the same value, so it worked. For a node
with *no* stretch child that self-heals in an unbounded context (exactly
`file_browser_utils.cpp`'s per-row icon-then-label `HorizontalLayout`,
self-healing inside a `Table` cell where the ambient
`GetContentRegionAvail()` is "whatever's left in the whole scrollable table
region", not this one row) they diverge hugely — jumping the cursor down by
however much of the table's remaining scroll region was ambiently
available, opening a large visible gap before the table's next row (the
"`.. [Up]`, huge gap, then the rest of the listing" symptom). Fixed by
adding a second stash value, `content_extent()` — the actual space
`arrange_vertical_layout()`/`arrange_horizontal_layout()`'s own children
loop consumed, always `<= arranged_size()` — and having
`pin_cursor_to_arranged_bottom()` read that instead.

Both fixes were verified against **file_explorer, zip_tool** (the exact
regression scenario, `build/app` navigated via the real automation
workflow) and a full pass over **every tab of `examples/demo`**
(Basics/Sliders/Text & Numbers/Selection/Tree & Collapse/Misc/Tables/Plots
incl. an expanded `Plot`/Plot3D/Files/Forms/Icons incl. its own icon+label
table) — the richest widget-combination surface in this repo, specifically
because the user flagged it as showing issues. Two blank-space-looking
spots turned up there (`tab_files`' unset-image-preview placeholder,
`tab_forms`' small top gap); both were confirmed **pixel-identical against
unmodified `main`** via `git stash`, i.e. pre-existing, unrelated to this
refactor. Every `render_*` unit test and the whole
existing `ctest` suite (680+ tests) pass with only the same four
pre-existing, unrelated failures present on `main` before this refactor
(`DragDropTest.DraggingSourceOntoMatchingTargetEmitsDroppedWithPayload` and
three `ZipToolEventTest` overwrite-confirm tests).

### Step 0 — Create `src/imgui/DESIGN.md`

**Goal:** A design document exists at `src/imgui/DESIGN.md` describing the
measure/arrange/draw architecture above, so this `PLAN.md` has something to
point to and future changes in this directory have a design contract to
respect.

**Deliverables:** `src/imgui/DESIGN.md` containing: overview of the render
pipeline (`renderer` → `imgui_renderer` → `sdl3_renderer`/`web_renderer`,
shared dispatch via `render_fns_`), the measure/arrange/draw model from the
Architecture section above (dispatch tables, stash fields, hook points,
`ensure_arranged()`'s self-heal role), and the *why* behind the four-vs-broader
dispatch-table split and the decision to leave `Window`'s own sizing and
`TreeNode`/`TabBar`/`Splitter`'s internal drag/expand state outside the system.

**Tests:** N/A (documentation only) — reviewed for accuracy against step 1-8's
actual implementation as those land; update if implementation diverges.

### Step 1 — Infrastructure, wired to nothing

**Goal:** `imgui_layout.hpp`/`.cpp` exist, compile, and are unit-testable in
isolation; nothing in `render_*` calls them yet, so behavior is unchanged.

**Deliverables:**
- `src/imgui/imgui_layout.hpp` — `natural_size`, `measure_fn`/`arrange_fn`
  typedefs, `measure_node()`, `arrange_node()`, `ensure_arranged()`
  declarations. No stash-field-name constants — geometry is read/written via
  `ui_element`'s own accessor methods, not field-map keys.
- `src/ui/ui_element.hpp`/`.cpp` — add the `layout_stash` struct
  (`measured_size`, `arranged_pos`, `arranged_size`, `arranged_frame`) as a
  `mutable` member of `ui_element`, plus its `measured_size()`/
  `set_measured_size()`, `arranged_pos()`/`arranged_size()`/
  `set_arranged_rect()`, and `is_arrange_fresh(int frame)` accessor methods.
- `src/imgui/imgui_layout.cpp` — the two dispatch-table builders
  (`measure_dispatch_fns()`, `arrange_dispatch_fns()`, each a function-local
  `static const` map built once, mirroring `built_in_render_fns()`'s shape in
  `imgui_renderer.cpp`), per-class `measure_fn`s for the class list in the
  Architecture section, `arrange_fn`s for `VerticalLayout`/`HorizontalLayout`/
  `Splitter`/`Table`, and the font-override-resolution replica needed for
  measure-pass font-metric parity.
- `CMakeLists.txt`: add `imgui_layout.hpp`/`.cpp` to the `WISH_IMGUI_ENABLED`
  `target_sources` block (`CMakeLists.txt:396-414`), next to
  `imgui_plot_renderer.cpp`.
- `tests/test_imgui_layout.cpp` (new file, same headless `ImGuiContext`
  fixture pattern as `tests/test_imgui_renderer.cpp`'s `ImguiRendererTest`).
- Baseline automation screenshots (see step 8's module list) captured now,
  before any behavioral change, saved for later diffing.

**Tests:**
- `measure_node()` on a bare `Label`/`Button` returns a size matching
  `CalcTextSize`/`GetFrameHeight()` expectations.
- `measure_node()` on a `VerticalLayout` of three fixed-height `Button`s
  returns `sum(heights) + spacing*(n-1)`.
- A `height:-1` (stretch) child contributes `0` to its parent's own natural
  height.
- `arrange_node()` on a `VerticalLayout` with one fixed(100) + one
  stretch(-1) child, given `content_size={0,300}`, places the fixed child at
  `y=0,h=100` and the stretch child at `y=100,h=200` (accounting for spacing).
- `ensure_arranged()` called directly on a bare node with no ancestor stash
  returns `false` and still populates `ui_element`'s `arranged_pos()`/
  `arranged_size()` correctly against `GetCursorPos()`/
  `GetContentRegionAvail()` inside an `in_window(...)` wrapper.
- `is_arrange_fresh()` staleness: stamp a node at frame `N` via
  `set_arranged_rect(..., N)`, advance to a synthetic frame `N+1` without
  re-arranging, assert `ensure_arranged()` returns `false` (self-heals)
  rather than trusting the stale rect.
- A `Label` with explicit `font_size` inside an auto-height `VerticalLayout`
  row arranges to that font's actual line height, not the default font's
  (font-metric-parity regression test).
- Full existing `ctest` suite still passes (nothing wired in yet, so this
  should be trivially true — confirms no accidental breakage from the new
  translation unit).

### Step 2 — Wire into `render_window`'s two hook points, output unused

**Goal:** The new passes run every frame for every `Window`/modal root and
never crash; `render_vertical_layout`/`render_horizontal_layout`/etc. still
use their old logic entirely, so no visual output changes.

**Deliverables:** In `render_window()` (`imgui_ui_renderer.cpp`), insert the
two-line `measure_node`/`arrange_node` call immediately before each of the
two existing `render_children(r, node, s)` calls (non-modal and modal paths).
Include `imgui_layout.hpp`.

**Tests:**
- Full existing `tests/test_imgui_renderer.cpp` suite passes bit-for-bit —
  this is the regression gate for this step.
- New assertion-only test: `ui_element::is_arrange_fresh()` is true on a
  `VerticalLayout` node after a `Window`-wrapped render.
- Automation: File Explorer screenshot, diffed against step 1's baseline —
  must be pixel-identical (nothing reads the new stash yet).

### Step 3 — `render_vertical_layout` consumes the stash

**Goal:** `VerticalLayout` rows are positioned/sized purely from
`ensure_arranged()`'s stash; the old stretch-pool pre-scan and
`layout_height_cache` reads/writes are gone from this function.

**Deliverables:** Rewrite `render_vertical_layout()`: call `ensure_arranged()`,
then for each child read its `arranged_pos()`/`arranged_size()`, `SetCursorPos`, and use a
fixed-size `BeginChild` (`ImGuiWindowFlags_NoScrollbar |
ImGuiWindowFlags_NoScrollWithMouse` always now — sizes are exact, not
estimated, so this is always safe). `Spring` keeps using its existing
`__wish_spring_w__/h__` fields for this step (full migration is step 5, kept
separate to bound this diff).

**Tests:**
- Every existing `VerticalLayout*`/`Spring*Vertical*` test in
  `tests/test_imgui_renderer.cpp` passes unmodified.
- New test `VerticalLayoutAutoHeaderStretchBodyNoOneFrameLag`: a header whose
  content height changes between frame 1 and frame 2 resolves correctly on
  frame 1 (no one-frame lag, unlike the deleted `layout_height_cache`).
- Automation: File Explorer screenshot now shows panels flush to the status
  bar (the gap half of the original bug fixed).
- Regression: `WindowRectMatchesWindowSizeNotLastChild`,
  `VerticalLayoutRectSpansAllChildrenNotLastOnly` still pass (the
  `BeginGroup`/`EndGroup` rect-reporting wrap in `render_node()` is untouched).

### Step 4 — `render_horizontal_layout` consumes the stash

**Goal:** Same as step 3, width axis, additionally fixing the pre-existing
width-axis asymmetry (no prior auto-width cache existed at all).

**Deliverables:** Same shape as step 3 for `render_horizontal_layout()`,
using arrange's per-child absolute `x` instead of `SameLine`-based flow for
positioning, keeping `align:"right"` behavior by feeding the offset into
`content_origin.x` passed to `arrange_node()`, and preserving the per-child
`BeginGroup()`/`EndGroup()` wrap `render_node()` needs for rect reporting.

**Tests:**
- Existing `HorizontalLayout*`/`Spring*Horizontal*` tests pass unmodified.
- New test `HorizontalLayoutAutoWidthColumnMatchesActualContent`.
- Automation: File Explorer screenshot now shows zero stacked scrollbars
  (the second half of the original bug fixed).
- Automation: **Notepad's toolbar row** (`modules/bdg/desktop/notepad`)
  explicitly screenshotted — this is the historical regression
  `ImGuiChildFlags_AutoResizeY` was built to prevent (fixed-width toolbar
  buttons ballooning to full window height); confirm it can't recur, both via
  a gtest rect assertion and the screenshot.

### Step 5 — `Splitter`/`Spring` full migration

**Goal:** `Splitter` consumes its own arranged outer box from its parent
Layout while its internal pane-dividing logic stays untouched; `Spring` fully
migrates onto `ui_element`'s native `arranged_size()`.

**Deliverables:**
- `render_splitter()`: call `ensure_arranged()` at the top for Splitter's own
  outer box; leave the pane-size pre-scan, `usable`/`explicit_sum` math,
  drag-bar `InvisibleButton` handling, and end-of-frame persistence into each
  pane's `width`/`height` field completely unchanged. Add `Splitter`'s
  `arrange_fn` entry: treats each pane as a normal hint-driven child but does
  not recurse geometry into pane content (panes stay leaves from arrange's
  perspective, like `Table`).
- `render_spring()`: read `arranged_size()` directly; delete the
  `__wish_spring_w__/h__` dynamic-field stamping code from
  `render_vertical_layout`/`render_horizontal_layout` (Spring's own outer
  size moves onto `ui_element`'s native layout stash, same as every other
  widget).

**Tests:**
- Existing `SpringCentersSingleChildInHorizontalLayout`,
  `SpringSpaceBetweenTwoChildrenInHorizontalLayout`,
  `SpringWeightBiasesSplitInHorizontalLayout`, `SpringMirrorsVerticalLayout`
  pass unmodified.
- New test `SplitterPaneDragStillMutatesWidthFieldLive`: simulated drag (same
  `fake_click`/`MouseDelta` idiom already used in `test_imgui_renderer.cpp`)
  still correctly updates a pane's persisted `width`/`height` field.

### Step 6 — `Table.outer_width`/`outer_height` fill semantics

**Goal:** `render_table` fills its arranged box exactly when
`outer_width`/`outer_height` is `0` and Table sits inside a Layout hint —
the direct, named fix for the File Explorer bug's root cause.

**Deliverables:** Before `ImGui::BeginTable(...)`, call `ensure_arranged()`;
when `outer_width`/`outer_height` is exactly `0` **and** Table has a real
arranged box from an enclosing Layout hint, use that box's `w`/`h` instead of
passing `0` straight to ImGui. Reproduce the original File Explorer bug first
to decide between the narrower rule (only reinterpret when Table's own
Layout-hint field is explicitly negative/positive) and the broader rule (also
applies when Table is a bare `Window` child with no Layout wrapper); document
whichever is chosen as a deliberate, named behavior change.

**Tests:**
- New test `TableOuterHeightZeroFillsArrangedBoxInsideStretchRow`: rect
  assertion via `ImGui::GetItemRectSize()` after `EndTable()`, <1px tolerance
  against the enclosing `VerticalLayout`'s stretch row.
- Automation: File Explorer end-to-end via the paired `AutomationClient` +
  `wish.Client` driver pattern (per `CLAUDE.md`'s automation workflow) —
  confirm both the gap and the stacked scrollbars are gone.

### Step 7 — Delete `context::layout_height_cache`

**Goal:** The old ad-hoc mechanism is fully gone.

**Deliverables:** Remove the `layout_height_cache` field and its doc comment
from `src/context/context.hpp:204-218`. `grep -rn layout_height_cache src/`
returns nothing afterward.

**Tests:** Full existing test suite compiles and passes with the field
removed — pure "nothing broke" gate, no new test needed.

### Step 8 — Full-tree regression pass + root `DESIGN.md` update

**Goal:** Every existing form/module renders identically (or, for step 6's
named intentional Table behavior change, deliberately and acceptably
differently) across the whole `modules/bdg/` and `src/ui/forms/` surface.

**Deliverables:**
- Automation before/after screenshots (diffed against step 1's baselines) for
  every `modules/bdg/*` app: `notepad`, `file_explorer`, `process_explorer`,
  `pix`, `zip_tool`, `log_tail`, `calculator`, `dev/editor`, `dev/git`; plus
  any `src/ui/forms/*` demo.
- Update the root `/home/carlos/github/wish/DESIGN.md`'s "Layout classes
  control how their children are arranged" section (currently describes
  plain `SetCursorPosY`/`BeginGroup`+`SameLine` flow with no mention of the
  stretch-pool or `Spring` mechanics that actually exist) to describe the
  real measure/arrange architecture, replacing the forward-looking sentence
  about a "two-pass measure-then-place operation" with a description of what
  was actually built.

**Tests:** Full `ctest` suite. Visual diff review of every screenshot pair
listed above.

### Step 8b — Expose the layout stash through automation's `get_widget()`

**Goal:** An automation client can read a widget's measured/arranged
geometry (not just its post-render hit-test rect) for layout debugging,
without changing `get_widget()`'s existing `rect`/`hovered` fields, which
stay sourced from the unrelated hit-test capture mechanism
(`imgui_renderer.cpp`'s `last_resolved_rect_min_`/`max_`,
`src/automation/automation_query.cpp:80-88`).

**Deliverables:** In `automation_query.cpp`'s per-widget JSON builder,
`dynamic_cast<const ui_element*>` the node (mirroring how
`self_reports_rect` already special-cases `Window`/`DockSpaceViewport`
rather than doing a generic field lookup) and, when non-null, add a
`measured` (`{w, h}`) and an `arranged` (`{x, y, w, h}`, or `null` if
`is_arrange_fresh()` is false for the current frame) key alongside the
existing `rect`/`hovered`/`active`/`visible`. Update
`bindings/python/wish/automation.py`'s `get_widget()` docstring/type hints
and `src/automation/DESIGN.md`'s widget-JSON-shape section to document the
two new keys.

**Tests:** New `automation_query` test asserting `measured`/`arranged` are
present and numerically sane for a widget inside a `VerticalLayout`, and
that `arranged` is `null` for a widget in a collapsed/never-rendered
subtree (mirrors the existing `rect: null` case for the same subtree kind).
No screenshot diff needed — this step only adds JSON keys, it changes no
rendered pixel.

---

## Part 2 — Widget field convention unification

### Step 9 — Tier 1: additive schema declarations, zero behavior change

**Goal:** Every widget that already functionally supports a `width`/`height`
hint has it formally declared in its schema; `Drag*` gains the same support
other value editors already have.

**Deliverables:** Add `addField` registrations (with Doxygen doc comments
matching `InputText`'s existing contract text) for `width` on `Button`,
`SliderFloat`, `SliderInt`, `Combo`, `InputInt`, `InputFloat`, `ColorEdit` —
each already reads the field via `get_as` fallback at render time, so this is
schema-only, no runtime code change. Add `width` support to `DragFloat`/
`DragInt` (`src/ui/ui_elements/drag.cpp`, `imgui_ui_renderer.cpp`'s
`render_drag_float`/`render_drag_int`), mirroring `InputText`'s
`SetNextItemWidth` idiom — this is a new capability (currently silently
ignored), not a behavior change for existing usage.

**Tests:** Existing tests for each widget type pass unmodified (schema
addition doesn't change runtime behavior for Tier-1 items). New test:
`DragFloatWidthFieldAppliesSetNextItemWidth`.

### Step 10 — Tier 2: `Selectable.width` convention flip

**Goal:** `Selectable.width:0` means "auto/size to content," matching every
other widget, instead of its current "fill available width."

**Deliverables:** In `src/ui/ui_elements/selectable.cpp` and
`render_selectable()`, flip `0`'s meaning to auto; introduce `-1` as the new
explicit fill sentinel. Confirmed via `grep -rn '"type":\s*"Selectable"'`
across `src/`/`modules/` that no current usage exists anywhere in this
codebase — zero migration needed for existing forms.

**Tests:** New tests asserting `width:0` now measures/renders at
content-size, `width:-1` fills available width (mirroring `Layout`'s own
stretch-child test shape). Automation: N/A (no existing form uses
`Selectable`) — note this explicitly in the PR rather than skipping silently.

### Step 11 — Tier 2: `Image` "0 = natural size" implementation

**Goal:** An `Image` with `src` set and `width`/`height` left at `0` renders
at the texture's actual pixel dimensions, matching its own (currently
unimplemented) documented contract.

**Deliverables:** In `imgui_renderer.cpp`'s `get_or_load_texture()` path,
expose the loaded texture's intrinsic pixel dimensions (add an out-param or a
lookup keyed by the same cache `get_or_load_texture()` already uses). In
`render_image()`, when `src` is non-empty and width or height is `0`, use the
texture's natural dimension for that axis instead of early-out-to-nothing.
**Must not trigger when `src` is empty** — confirmed via `pix.cpp:150,359-364`
that `preview_image` is deliberately initialized to `width:0,height:0` with
empty `src` as an intentional "nothing selected" placeholder, and already
sets real `width`/`height` explicitly once an image loads, so this step
changes no existing `pix.cpp` behavior.

**Tests:** New test rendering an `Image` with a real `src` and `width:0,
height:0`, asserting the drawn size matches the texture's actual dimensions.
Regression test confirming `src:""` with `width:0,height:0` still renders
nothing (the placeholder case). Automation: `modules/bdg/desktop/pix`
screenshot before/after — must be visually identical (placeholder behavior
unchanged; only the previously-unreachable "has src, zero size" path changes).

### Step 12 — Tier 2: `TextEditor` width/height sentinel clarification

**Goal:** `TextEditor.width`/`height` distinguishes `0` (use the built-in
fixed default) from an explicit `-1` (fill), instead of collapsing both to
the same fill sentinel.

**Deliverables:** In `imgui_text_editor_renderer.cpp`, change the current
`w > 0 ? w : -1.f` collapse so that `0` maps to the schema's documented fixed
default (`400` for height; a sane default width) and only an explicit
negative value maps to ImGuiColorTextEdit's `-1` fill sentinel.

**Tests:** New tests for `width:0`/`height:0` (uses default), explicit
`width:-1`/`height:-1` (fills). Automation: whatever module uses `TextEditor`
(`modules/bdg/dev/editor`) screenshotted before/after — grep confirms no
current form authors exactly `0`, so this should be a no-op visually; the
screenshot pair documents that.

---

## Non-goals (explicitly deferred, not silently in scope)

- Simulating ImGui's internal cursor/indent advance for `TreeNode`/`TabBar`/
  `MenuBar` inside the pure-computation arrange pass — those keep using
  ImGui's live default flow; anything nested inside gets correct geometry via
  `ensure_arranged()`'s self-heal, not the top-down pass.
- Full CSS-style width-then-height constraint solving for wrap-dependent
  content — not needed for the current widget set (see Architecture); the
  documented extension point is adding a dedicated `measure_fn` for any
  future widget that violates the assumption.
- `stable_id()`'s fallback collision for runtime-constructed nodes with no
  `__path__`/`__wish_id` (e.g. `file_dialog.cpp`'s per-row icons) — a
  `PushID`/widget-identity issue, unrelated to sizing. This refactor's
  per-node stash needs no string key at all (unlike the deleted
  `layout_height_cache`), which incidentally removes one *cross-top-level-
  root* collision risk that existed in the old cache — but the underlying
  `stable_id()` gap itself is not fixed here.

## Risks / must-not-break list

- Notepad toolbar regression (step 4).
- `imgui_renderer::render_node()`'s `BeginGroup()`/`EndGroup()` rect-reporting
  wrap around `VerticalLayout`/`HorizontalLayout`/`Splitter`/`TabBar`/
  `TabItem`/`TreeNode`/`CollapsingHeader`/`Table`/`TableRow`/`Plot`/`Plot3D`
  dispatch — one level outside everything this plan touches; re-run
  `WindowRectMatchesWindowSizeNotLastChild` and
  `VerticalLayoutRectSpansAllChildrenNotLastOnly` from step 3 onward.
- Splitter's live-drag persistence (step 5's dedicated test).
- Modal `Window`'s one-shot `OpenPopup`/`CloseCurrentPopup` handshake
  (`__modal_opened__`/`__request_close__`) — hook points are inserted after
  `BeginPopupModal()` succeeds, identical placement to the non-modal path, no
  interaction with the open/close state machine; re-run existing modal tests.
- Font-metric parity (step 1's dedicated test) — a node with a custom
  `font_size` must measure against the same resolved font it renders with.
- Step 6's Table behavior change is the one place Part 1 *intentionally*
  changes visible output — name it explicitly in the PR, don't let the
  general step 8 sweep be the only thing that catches it.
- `pix.cpp`'s `preview_image` placeholder pattern (step 11's dedicated
  regression test) — the `Image` natural-size fix must not make an unset,
  no-`src` placeholder start rendering something.

## Completion Criteria

- `grep -rn layout_height_cache src/` returns nothing.
- `grep -rn __wish_spring_w__\|__wish_spring_h__ src/` returns nothing
  (fully migrated onto `ui_element`'s native `layout_stash` members).
- Full `ctest` suite passes, including every new test named in steps 1–12.
- Every automation screenshot pair listed in step 8 has been visually
  reviewed; any difference is either none, or one of the two explicitly
  named/documented intentional changes (step 6's Table fill semantics, or a
  Part 2 Tier-2 flip with an empty/no-diff automation note per step 10/12).
- `src/imgui/DESIGN.md` and the root `DESIGN.md` both accurately describe the
  shipped architecture (no stale "default imgui top-to-bottom flow" text
  remaining).
- No `TODO`/dead code left from the old `layout_height_cache`-based stretch
  math in `render_vertical_layout`/`render_horizontal_layout`/
  `render_splitter`/`render_table`.
