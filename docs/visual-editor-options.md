# Visual UI Editor Options: ImRAD Studio Fork vs. Native Editor Extension

**Status: proposal / not yet implemented.** This document records an
analysis of how wish could grow a WYSIWYG (drag-and-drop, click-to-select,
property-inspector) editor for its JSON/YAML UI format, and lays out two
viable implementation paths without picking one. Use it as the starting
point when this work is actually scheduled.

## Context

wish's UI hierarchies are described by a JSON/YAML tree
(`{"type": ..., ...fields, "children": {...}}`), parsed by
`ui_descriptor.hpp`/`ui_importer.hpp` into `ui_element` (bison `dynamic`)
objects — see [ui-elements.md](ui-elements.md) for the full schema and
catalog. Today the *only* editing tool is `modules/bdg/dev/editor`
(`wish client --run=editor -- ui.json`, off by default) — a
syntax-highlighted `TextEditor` next to a live, continuously-reparsed
preview. It is explicitly **not** WYSIWYG: since this document was
written, its cursor→schema help panel and autocompletion (type names,
field names, enum values) have since been implemented — see its own
`DESIGN.md` §10 — sourced from a new shared query module,
`src/ui/ui_schema_help.hpp`, that walks the live class registry in-process
(server-side, no RMI round-trip needed). Only an element palette remains
unimplemented. There is still no drag-and-drop placement, no
click-to-select, no property inspector — every edit is hand-typed JSON.

This document evaluates **ImRAD Studio** (a mature, GPL-licensed, GUI
drag/resize/property-panel builder for Dear ImGui that generates/parses
C++) as a shortcut to a real visual editor, against building that visual
layer natively into wish's own MIT-licensed codebase. The tradeoffs are
genuine (engineering time vs. license/fidelity/coverage risk) and worth a
deliberate choice before starting implementation.

### Key facts established by research

**wish side** (`docs/ui-elements.md`, `DESIGN.md`, `src/ui/`):
- Schema: `{type, ...fields, children: {name: node}}`. `type` → registered
  class name; unknown fields silently ignored (no validation — typos are
  invisible); `children` object → named dot-path children, array →
  unnamed numeric children.
- 60 registered widget classes (`src/ui/ui_elements/*.cpp`,
  `plot_elements/`, `plot3d_elements/`): basic controls, menus, tabs/tree,
  tables, docking (`DockSpace`/`DockSpaceViewport`), a `TextEditor`
  element, 14 2D plot types, 8 3D plot types.
- Field schema (`DisplayName`/`Description`/`Category`/`Range`/`Step`/
  `EnumFlags`) is attached via C++ `addField`/`attr<...>` calls at
  class-registration time — **no RMI introspection endpoint exists** to
  query it live over the wire. Any external tool needs either a
  hand-maintained mapping table (drift risk — `docs/ui-elements.md` itself
  already warns it can drift from source) or a new introspection endpoint
  added to wish.
- `automation::get_tree()` is **not** a usable "dump live UI → JSON
  template" round-trip today: it returns a flat array (not nested), only 7
  hand-picked probed fields per widget (`label`/`text`/`value`/`title`/
  `checked`/`selected`/`hint` — drops most of a widget's actual configured
  state, e.g. a `Window`'s `width`/`height`/`modal`, a `Slider`'s
  `min`/`max`), and only covers named (not array-indexed) children. A true
  round-trip export would need new server-side code, though it's
  buildable — bison already supports generic field reflection
  (`forEach`/`findField`, used by `ui_importer.cpp`'s
  `set_field_from_dynamic`).
- License: wish is **MIT**.

**ImRAD side** (github.com/tpecholt/imrad):
- **License: GPL** for the tool's own source; generated C++ output and
  `imrad.h` are explicitly excluded from GPL (so *using* it to author
  unrelated projects is fine — but modifying and redistributing the *tool
  itself* must stay GPL/source-available).
- **No plugin or alternate-export-format system.** Persistence is C++ text
  itself: it re-parses existing `.h`/`.cpp` files (comment-delimited
  generated sections) via `cpp_parser.h`, and regenerates via
  `cppgen.h`/`cppgen.cpp`. This confirms the premise of this
  investigation — forking is the only way to add JSON export.
- **But the internal model is separable enough to make forking
  tractable**: `UINode` (base class, in `node_standard.h`/
  `node_container.h`/`node_window.h`/`node_extra.h`) exposes `virtual
  Properties() -> vector<Prop>` / `virtual Events() -> vector<Prop>`, a
  generic name→`property_base*` list every concrete widget populates — a
  serializer can walk this without a switch-per-type. `cppgen.h` builds a
  `vector<Config>` (node pointer + params map) before ever touching
  C++-specific output — an alternate JSON-export pass could plausibly
  consume that same `Config` vector in parallel with `ExportH()`/
  `ExportCpp()`, without touching `cpp_parser.h` at all (a JSON export
  doesn't need to *re-parse* C++, only *emit* JSON).
- **Widget coverage overlap**: ImRAD's node types (`Button`, `CheckBox`,
  `RadioButton`, `Input*`, `Combo`, `Slider`, `ProgressBar`, `ColorEdit`,
  `Image`, `Table`, `Child`, `CollapsingHeader`, `TabBar`/`TabItem`,
  `TreeNode`, `MenuBar`/`ContextMenu`/`MenuItem`, `Splitter`,
  `CustomWidget`) map reasonably well onto roughly **30 of wish's 60**
  classes (basic controls, menus, tabs/tree, tables). It has **no
  equivalent** for wish's docking elements, any of the 22 2D/3D plot
  classes, `TextEditor`, or wish-specific attributes (`drag_type`/
  `drop_type`, `Layout`'s `align`/width-hint semantics). Those would need
  new custom `UINode` subclasses written into the fork regardless of the
  JSON-export work.
- Actively maintained (3-platform CI, ~1.4k stars), single primary
  maintainer, small codebase (`src/` is ~65 files).

**Other tools considered**: ImStudio (Raais/ImStudio) is a smaller, less
mature Dear ImGui layout designer with the same "exports to C++ calls, not
data" limitation as ImRAD and no clearer path to a JSON exporter — not a
stronger candidate than ImRAD. A green-field custom web-based
(React/HTML5) drag-and-drop builder targeting wish's JSON natively was
considered and rejected as a primary path: it would need to re-approximate
~60 widgets' visual appearance outside of wish's actual ImGui renderer,
which is its own WYSIWYG-fidelity/drift risk on top of being the most
from-scratch option of all three.

---

## Option A — Fork ImRAD Studio, add a JSON exporter/importer

**Shape of the work:**
1. Fork `tpecholt/imrad`; confirm the fork stays GPL-licensed and is
   distributed as a standalone dev tool, **not linked into wish's own MIT
   binaries** (analogous to using a GPL asset tool to produce data files —
   keeps wish's own license clean, but the fork's *own* source must stay
   available under GPL terms if you distribute it to anyone).
2. Add a `jsongen.h`/`.cpp` module parallel to `cppgen.h`/`.cpp`: walk the
   existing `Config` vector (or `UINode` tree directly), emit wish's
   `{type, ...fields, children}` shape via each node's `Properties()`.
3. Build and hand-maintain the ~60-entry class/field mapping table (ImRAD
   node type/property name → wish class/field name) — needed because
   there's no live schema introspection on the wish side to auto-generate
   it from (see "wish side" facts above). Flag this file prominently for
   review whenever either project's widget catalog changes.
4. For the ~30 wish classes with no ImRAD equivalent (plots, docking,
   `TextEditor`, wish-specific layout/drag-drop attributes), add new
   custom `UINode` subclasses to the fork, following the pattern of the
   existing `node_*.cpp` files.
5. Decide the binding/event story: ImRAD's `bindable<T>` properties assume
   generating C++ variable bindings; wish's JSON has zero binding/handler
   syntax (wiring happens in host code after `instantiate_template()`).
   The exporter should emit only static field *values*, not any binding
   expression — this needs an explicit design pass so ImRAD's editing UI
   doesn't imply capabilities the JSON format can't express.
6. (Stretch) A JSON *importer* (JSON → `UINode` tree, so an existing wish
   JSON file can be opened and re-edited in the fork) is the harder
   direction — it re-adds a parse step ImRAD's own `cpp_parser.h` exists
   to avoid needing twice. Consider scoping the first cut to export-only
   (author visually, save as wish JSON) and treat re-opening as a later
   stretch goal.
7. Validate output against wish: round-trip a handful of
   `docs/ui-elements.md`-cataloged examples (including
   `examples/editor_sample_ui.json`) through the exporter, then load the
   result with `wish client --run=editor -- <output.json>` to confirm it
   parses and previews correctly.

**Tradeoffs:** Fastest path to mature drag/resize/property-panel UX
(ImRAD's core strength). Costs: GPL fork to maintain long-term (rebasing
against upstream ImRAD changes), partial (not full-catalog) coverage even
after the work in step 4, a hand-maintained mapping table with the same
drift risk `docs/ui-elements.md` already has today, and no guarantee that
ImRAD's own ImGui rendering is pixel/behavior-identical to wish's
(`src/imgui/imgui_ui_renderer.cpp` has wish-specific layout quirks — e.g.
the `HorizontalLayout`/modal-window click bug noted in `CLAUDE.md` — that
a separate codebase's renderer won't reproduce), so what you build
visually in the fork isn't guaranteed to be what wish actually renders.

---

## Option B — Extend the native `editor` module toward WYSIWYG

This is exactly the path the module's own `DESIGN.md` §10 already earmarks
as its next real design pass (palette, then inspector) — not a rejected
idea, just explicitly deferred pending its own design work.

**Shape of the work, in incremental, independently-shippable stages:**

1. **Element palette.** A browsable, categorized list of the registered
   classes (grouped `ui`/`plot2d`/`plot3d` per the DESIGN.md's own
   framing). `ui_schema_help::enumerate_ui_element_classes()` (added since
   this document was first written; see the editor module's `DESIGN.md`)
   already provides the flat class list with display names/descriptions/
   fields in-process — reusable directly for stage 1's own list. What it
   does *not* provide is collection/grouping metadata (`ui` vs. `plot2d`
   vs. `plot3d`), which still doesn't exist anywhere in the registry (no
   class-level `Category` attribute is ever attached) and would need
   either a new class-level attribute or a hand-maintained grouping table.
   Clicking a palette entry inserts a default-field JSON skeleton into the
   `TextEditor` at the cursor.
2. **Preview → source selection.** Reuse the automation module's existing
   hit-test/screen-rect infrastructure (`build_tree_snapshot()`/
   `automation_query.cpp`) — clicking a widget in the live preview already
   resolves to a dot-path via `mock_id_to_path_` (the editor module
   already builds this map on every reparse, today only used for the
   event log). Extend that click handling to scroll/highlight the
   corresponding node in the source `TextEditor`. This is more tractable
   than the DESIGN.md's originally-flagged direction (cursor position →
   AST node for a *help panel*) since dot-path → text search is simpler
   than the general cursor→AST mapping problem.
3. **Property inspector.** Once a node is selected (stage 2), show a form
   of its current field values (reusing bison's existing generic field
   reflection — the same `forEach`/`findField` machinery
   `ui_importer.cpp`'s `set_field_from_dynamic` already uses, so no
   separate schema table to maintain) and write edits back into the JSON
   text, keeping the existing text-editor-is-source-of-truth model intact.
4. **(Stretch) Click-to-place / drag-repositioning** — the actual "visual
   authoring" capability, layered on top of stages 1–3 once selection and
   field-editing are solid. Likely the largest scope of the four; worth
   its own design pass once 1–3 are shipped and validated.

**Tradeoffs:** No license entanglement (stays MIT throughout, consistent
with the rest of wish). True WYSIWYG by construction — editing happens in
wish's actual runtime renderer, not a lookalike, so it cannot diverge from
what a real client sees. Full 60-class coverage from day one, self-updating
as new widget classes are added (no external mapping table). Costs: more
net-new engineering overall, especially stage 4 (no existing precedent
anywhere in wish/bison for click-driven canvas manipulation); doesn't
inherit ImRAD's years of RAD-tool UX polish.

---

## Decision criteria (for when this is scheduled)

- **If GPL is a non-starter** for anything wish-adjacent, or full-catalog
  fidelity (plots/docking) matters from day one → Option B.
- **If a resizable/draggable canvas UX is wanted fast**, and maintaining a
  GPL fork indefinitely plus accepting partial catalog coverage (basic
  controls/menus/tabs/tables only, initially) is acceptable → Option A.
- **Either way**, the `automation::get_tree()` round-trip gap and the lack
  of a live schema-introspection RMI endpoint are shared prerequisites
  worth fixing on the wish side regardless of which path is chosen —
  they're needed for a real "dump running UI → JSON" feature and for any
  external tool (including an ImRAD fork) to stay in sync with wish's
  widget catalog without hand-maintenance.

## Verification (once a direction is chosen and implemented)

- Build with `-DWISH_MODULE_BDG_DEV_EDITOR=ON` (Option B) and drive the
  resulting editor via the automation module (`AutomationClient`, per
  `CLAUDE.md`'s automation workflow) to confirm palette-insert / select /
  inspector-edit round-trips correctly into the source JSON and live
  preview.
- For Option A, round-trip `examples/editor_sample_ui.json` and a handful
  of `docs/ui-elements.md` catalog examples through the fork's exporter,
  then load the output with `wish client --run=editor -- <output.json>`
  (or the automation module) to confirm it parses without error and the
  preview matches what was authored visually.
- Either way, add a regression test under `tests/` exercising the new
  import/export or selection/inspector logic, per this repo's testing
  conventions (GoogleTest, `TEST_F`, deterministic).

## See also

- [ui-elements.md](ui-elements.md) — the JSON/YAML schema and full widget
  catalog this proposal builds on.
- `modules/bdg/dev/editor/DESIGN.md` — the existing text+live-preview
  editor's architecture, including its own "Implementation Status" section
  that first flagged the palette/inspector gap this document addresses.
- `src/automation/DESIGN.md` — the hit-test/tree-query infrastructure
  Option B stage 2 reuses.
