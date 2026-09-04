# PLAN.md — `DockLayout`: declarative default dock arrangements

**Status: implemented and tested.** A UI can ship a default docking layout so
first-run users get a sensible split arrangement without dragging windows
into place. Once applied, the layout is owned by `imgui.ini`; the author
bumps a `version` to re-apply a changed default.

Read alongside **[docs/dock-layout.md](../../../docs/dock-layout.md)** (the
user tutorial) and the `#### DockLayout` section of
**[docs/ui-elements.md](../../../docs/ui-elements.md)**.

## What was built

Three ordinary `"wish"` elements — **`DockLayout` › `DockSplit` ›
`DockArea`** — carry the arrangement as a small tree. Because they are
registered elements, they flow through every path that carries a UI tree
with no extra plumbing:

- a server-side `wish::form` → `form::set_default_dock_layout(ui_element_ptr)`,
  with the tree built directly by the `bdg::wish::dock::` helpers in
  `src/ui/dock_layout_spec.hpp` (no JSON, no descriptor round trip);
- a **client-registered template** → the client puts a `{"type":"DockLayout",
  …}` node in its descriptor; `build_ui_node` resolves it like any element;
- a hand-authored `import_json` tree, or a `DockSpaceViewport` child.

The renderer realizes the tree once via ImGui's `DockBuilder` API and then
leaves the arrangement to `imgui.ini`.

### Key decisions

1. **Typed elements, built directly — no intermediate JSON.** An earlier
   draft had the `dock::` builder emit a descriptor-JSON string that
   `form::set_default_dock_layout` re-parsed via `import_json`. Dropped: the
   `dock::` helpers now return `ui_element_ptr` built with
   `ui_element_ptr::create("wish"_key, "DockArea"_key)` etc. — the same
   objects the template importer produces, just without the text step. The
   shared representation with templates is the **registered element
   classes**, not a serialization format.

2. **Three nested elements, not a JSON-string field or per-`Window` hints.**
   A split tree expressed as relative hints on N disconnected `Window`s
   needs ordering rules that get unreadable past two panes. `DockArea.windows`
   is a **newline-delimited string** (not a JSON array — the client
   descriptor importer, `ui_descriptor.cpp`, silently drops array/object
   scalar fields, so an array would vanish in a template).

3. **`DockLayout` renders (a no-op draw) and self-applies.** Its dispatch fn
   `render_dock_layout` runs `DockBuilder` when needed and draws nothing —
   so every host wrapper keeps working unchanged. Precondition: some
   dockspace published an id this frame (same as `render_window`'s existing
   default-dock path).

4. **`imgui_internal.h` is quarantined** to the new
   `src/imgui/imgui_dock_layout.cpp` — the one renderer TU allowed to
   include it (for `DockBuilder*` and the settings handler).
   `render_dock_layout` in `imgui_ui_renderer.cpp` stays on the public API
   and calls into that TU.

5. **Apply on first run / version bump / sibling takeover; else leave it.**
   `render_dock_layout` (re)applies when: the layout has no persisted record
   at its current `version` (fresh `imgui.ini`, or an author bumped it);
   `DockBuilderGetNode(target) == nullptr`; **or** the windows the layout
   names are not currently docked under `target` (`layout_windows_are_live`
   is false — e.g. docker / kubectl / git all share the host chrome's one
   `HostDockSpace`, so running one after another must re-lay-out each).
   Once a layout's windows *are* live under `target` it is untouched, so a
   user's own rearrangement survives. State is persisted per layout in
   `imgui.ini` under `[WishDockLayout]`, **keyed by the hash of the layout's
   window-path list** (`layout_identity()`), not the dockspace id — so two
   apps sharing a dockspace keep independent records. The
   `ImGuiSettingsHandler` is installed in `imgui_renderer::begin_frame()`
   before the first `NewFrame`.

### Files

| File | Change |
|---|---|
| `src/ui/ui_element.hpp` | `ui_dock_layout` / `ui_dock_split` / `ui_dock_area` typed accessor classes |
| `src/ui/ui_elements/docking.cpp` | register `DockLayout` / `DockSplit` / `DockArea` in `register_docking()` |
| `src/ui/dock_layout_spec.hpp` | new — `dock::layout()` / `dock::split()` / `dock::area()` element builders |
| `src/imgui/imgui_dock_layout.{hpp,cpp}` | new — `build_dock_layout()` (`DockBuilder` walk) + `[WishDockLayout]` settings handler |
| `src/imgui/imgui_ui_renderer.{hpp,cpp}` | `render_dock_layout()` coordinator |
| `src/imgui/imgui_renderer.cpp` | dispatch entry; `install_dock_layout_settings_handler()` in `begin_frame()` |
| `src/ui/forms/form.{hpp,cpp}` | `set_default_dock_layout()`; `extra_internal_roots_` cleaned by `remove_internal_objects()` |
| `modules/bdg/dev/docker/server/docker.cpp` | drop `pos_x`/`pos_y`; seed the grid in `on_init()` |
| `CMakeLists.txt` | new sources |
| `docs/ui-elements.md`, `docs/dock-layout.md`, `README.md`, `CHANGELOG.md`, `docker/DESIGN.md`, `docker/docker_mock.json` | docs |

### Tests

| Test | File | Covers |
|---|---|---|
| `DockLayoutFamilyResolvesThroughImporter`, `DockLayoutDescriptorRoundTrips`, `DockAreaWindowsStaysAScalarString` | `test_ui_importer.cpp` | element registration + descriptor/`build_ui_node` path |
| 8 `DockLayout*` / `TwoLayoutsSharingOneDockspace*` cases in `test_imgui_renderer.cpp` | `DockBuilder` realization, side semantics, no-reapply / version bump, shared-dockspace takeover, focus, no-op, robustness, template shape |
| `FormDockLayout.RegistersDockLayoutTopLevelObject`, `FormDockLayout.TornDownWithForm` | `test_form_base.cpp` | `set_default_dock_layout` registration + teardown |
| `RegistersDefaultDockLayout` + a close-teardown case in `test_docker.cpp`, `test_kubectl.cpp`, `test_git.cpp` | each module's dock-layout wiring |
| `IntegrationTest.ClientTemplateCanCarryDockLayout` | `test_integration.cpp` | client `register_template` → `instantiate` round trip |

## Deviations from the original plan

- The `dock::` builder produces typed `ui_element_ptr`, not a JSON string;
  `form::set_default_dock_layout` takes `ui_element_ptr`, not a descriptor
  string. (Simpler; no `import_json` in the form path.)
- No `examples/demo` change — the demo has a single window, so a `DockLayout`
  there could not show a split without adding a contrived second window.
  The docker module is the worked in-repo example; `docs/dock-layout.md`
  carries a full template example.
- Version persistence shipped in the same pass (was a separate step) — it is
  ~40 lines and avoids a follow-up ini-format change.
- `dock::split(dir, ratio, near, far)`: `near` is the pane on the `dir` side
  and takes `ratio` of the space (verified against a live docker run's
  `[Docking][Data]`). `DockLayoutBuildsSplitTreeIntoAmbientDockspace` pins
  the side, not just "different nodes".

## Verified live

`wish standalone --renderer sdl3 --run=<docker|kubectl|git>` against a fresh
`imgui.ini` writes `[WishDockLayout][…] Version=1` and a `[Docking][Data]`
tree matching the intended arrangement:

- **docker / kubectl** — left 62% (list/stats tabs over a 24% Console
  strip), right 38% (Logs + Inspect/Describe tabbed).
- **git** — left 70% (commit graph over a 27% Log strip), right 30% (Files
  over Diff).

## Known limitations (documented in docs/dock-layout.md)

- No ambient dockspace and no `target` ⇒ inert no-op.
- `imgui.ini` is one shared file keyed by hashed window ids; colliding window
  paths across apps fight over entries.
- Re-instantiating the same template twice collides on window paths
  (pre-existing template behaviour); the second `DockLayout` is a no-op.
