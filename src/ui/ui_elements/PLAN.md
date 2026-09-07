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

5. **Apply at most once per version; else leave it alone, unconditionally.**
   `render_dock_layout` (re)applies only when: the layout has no persisted
   record at its current `version` (fresh `imgui.ini`, or an author bumped
   it), or `DockBuilderGetNode(target) == nullptr` (no tree at all). Once
   applied at a version, nothing reapplies it again at that version — not a
   drag, not a float-out, not a sibling wish tool sharing the same host
   dockspace (docker / kubectl / git all share the host chrome's one
   `HostDockSpace`) rebuilding it wholesale. An earlier revision of this
   design also reapplied when the layout's windows were no longer live under
   `target`, to re-lay-out a tool displaced by a sibling's rebuild — dropped
   because inferring "displaced by a sibling" from transient docking state
   is indistinguishable from an ordinary drag or float-out reaching the same
   state, and treating either as license to rebuild discarded users' own
   rearrangements. State is persisted per layout in `imgui.ini` under
   `[WishDockLayout]`, **keyed by the hash of the layout's window-path list**
   (`layout_identity()`), not the dockspace id — so two apps sharing a
   dockspace keep independent records. The `ImGuiSettingsHandler` is
   installed in `imgui_renderer::begin_frame()` before the first
   `NewFrame`.

6. **Isolating an app's dockspace from siblings: nest it, don't tag it.**
   Sharing one host dockspace across apps (docker / kubectl / git) means
   any of them rebuilding its layout can rearrange or evict a sibling's
   windows, because `DockArea.windows` names land in the same physical
   dock node. Rather than adding per-window ownership/exclusion logic to
   the shared node, `DockSpaceViewport` gained an `embedded` bool: when
   true it renders as an ordinary dockable window (one tile, docking into
   whatever's ambient on first use — the same `SetNextWindowDockID(...,
   ImGuiCond_FirstUseEver)` fallback `render_window` already uses) instead
   of the fullscreen host chrome, and hosts its own **nested**
   `ImGui::DockSpace()`. `dock::viewport(id, layout(..., target=id))`
   wraps that up: the app's whole window set lives in a physically
   distinct node no sibling can ever reach. `DockLayout.target` resolving
   a named id had already been implemented for this exact purpose but was
   untested until this pass surfaced a real bug in it (see next point).
7. **`DockLayout.target` must hash against `window->ID`, not the current ID
   stack top.** `render_dock_layout()`'s named-target resolution originally
   called `window->GetID(target)`, which hashes against whatever is on top
   of `IDStack` *at the point this code runs* — and a `DockLayout` nested
   several levels into the tree-walk dispatch runs behind other elements'
   own `PushID()` scopes, so that top is **not** `window->ID` by the time
   it gets there. `render_dockspace_viewport()`, by contrast, computes its
   nested dockspace id via `ImGui::GetID(id)` immediately after `Begin()`,
   before any child (and thus any `PushID`) has run — a different, stable
   seed. The mismatch built a second, orphan dock node every frame,
   silently discarded by ImGui's housekeeping one frame later, so a
   window targeting it never stayed docked. Fixed by hashing directly:
   `ImHashStr(target.c_str(), 0, w->ID)`, matching `ImGui::GetID`'s own
   seed regardless of dispatch depth.

8. **`layout_identity()` must fold in the target dockspace, not just the
   window-path list.** After decisions 6/7 shipped, a live run reported
   "the layout is completely broken, no window is attached to the docking
   space" -- a real regression the unit-test suite above did not catch.
   Root cause: docker/kubectl/git/dbg's window sets and `version` didn't
   change when they opted into `dock::viewport(...)`, only their
   `DockLayout.target` did (ambient → a named nested id). `imgui.ini` from
   before the opt-in already recorded that exact window-path-list identity
   as "applied at version 1", so `should_apply_dock_layout()`'s primary
   check said "already applied" -- and its only fallback
   (`DockBuilderGetNode(target_id) == nullptr`) never fired either, because
   `render_dockspace_viewport()` calls `ImGui::DockSpace(new_target_id,
   ...)` (which auto-creates an empty node at that id) *before* the nested
   `DockLayout` child renders, so the node already existed the instant the
   check ran. Net effect: `build_dock_layout()` never ran against the new
   node, so every window the layout would have placed there was left
   wherever ImGui's (unrelated, stale) per-window ini state put it --
   silently, with no error. Fixed by hashing `target_id` into
   `layout_identity()` alongside the window-path list, so a layout whose
   target changed is a new identity regardless of window set or version.
   See `DockLayoutReappliesWhenTargetChangesEvenAtSameVersion` in
   `test_imgui_renderer.cpp`, which drives this exact "already-applied
   under the old target, now pointed at a fresh, auto-created node" trap
   directly against `should_apply_dock_layout()`/`build_dock_layout()` --
   confirmed to fail (windows left with `DockId == 0`) against the
   pre-fix code before being confirmed to pass against the fix.

9. **Decision 8 was necessary but not sufficient: the real, remaining root
   cause was top-level-object render order, not `layout_identity()`.**
   After decision 8 shipped, live testing (`wish-standalone --run=docker`,
   fresh `imgui.ini`) still showed every window sharing one tabbed node
   instead of the intended split -- a plain unit test could not have
   caught this, because every existing test drives a hand-picked,
   author-controlled render order (`DockLayout` before its windows), while
   production renders a session's `top_level_objects`
   (`std::unordered_map`, see `src/context/context.hpp`) in **arbitrary**
   order. Debug instrumentation on a live server confirmed all of
   docker's `Window` top-levels called `Begin()` -- resolving
   `SetNextWindowDockID(ambient, ImGuiCond_FirstUseEver)` -- *before* its
   `DockSpaceViewport`/`DockLayout` rendered, in the very frame
   `should_apply_dock_layout()` first returned true. That ordering means
   `DockBuilderDockWindow()` reassigns windows to the new split nodes only
   *after* their placement for that frame is already resolved; ImGui then
   prunes the freshly-built, still-unhosted nodes at end of frame,
   collapsing the split permanently (no second chance, per decision
   above: applies at most once per identity). Fixed by rendering a
   session's `DockLayout`/`DockSpaceViewport` top-levels in a first pass,
   before every other top-level, in both `src/standalone/standalone.cpp`
   and `src/server/server.cpp` -- confirmed live afterward (server debug
   log showed the corrected ordering; a Playwright `getTree()` probe
   showed the intended 62/38 split with tabbed groups and a console
   strip, not one flat shared node). See
   [docs/dock-layout.md](../../../docs/dock-layout.md)'s "Render-order
   hazard" section.

10. **`DockSpaceViewport.id` is never user-facing, so a separate `title`
    field was added rather than reusing `id` for display.** `id` doubles
    as the ImGui window's ID-stack anchor -- `ImGui::GetID(id)` derives
    the DockSpace id, and a nested `DockLayout.target` matches it via
    `ImHashStr(target, 0, w->ID)`, which only works if `target` and `id`
    are the exact same string. Once embedded viewports became visible
    tiles (decision above's `embedded` field), their tab showed that raw
    internal id (e.g. `docker_dock`) instead of a name a user would
    recognize. Rather than let a display string leak into the identity
    hash, `title` layers a friendly name on top using the same
    `"Title###id"` trick `with_id()` already uses for ordinary windows --
    `ImHashStr()` resets and hashes only what follows `###`, so
    `ImGui::GetID("Docker###docker_dock")` (Begin()'s window label) and
    `ImGui::GetID("docker_dock")` (used for the DockSpace id, and by
    `DockLayout.target`) hash identically; changing `title` can never
    change dock identity. `dock::viewport(id, title, layout(...))` now
    takes the display name as a required second argument; confirmed live
    afterward via a Playwright `getTree()` probe against `wish-server
    --run=docker` (the `DockSpaceViewport` node reports `title: "Docker"`
    while its `path`/id stays `__docklayout_0`, and the split geometry is
    byte-for-byte unchanged from before the change).

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
| `src/ui/ui_elements/docking.cpp`, `src/ui/ui_element.hpp` | new `embedded` bool field on `DockSpaceViewport` |
| `src/imgui/imgui_ui_renderer.cpp` | `render_dockspace_viewport()`: embedded-mode `Begin` flags/title, ambient-id capture + restore |
| `src/imgui/imgui_dock_layout.cpp` | `render_dock_layout()`'s named-`target` resolution fixed to hash against `window->ID` (see key decision above) |
| `src/ui/dock_layout_spec.hpp` | new — `dock::viewport(id, layout(...))` builder |
| `modules/bdg/dev/docker/server/docker.cpp`, `modules/bdg/dev/kubectl/server/kubectl.cpp`, `modules/bdg/desktop/git/server/git.cpp`, `modules/bdg/dev/dbg/server/dbg.cpp` | opted into `dock::viewport(...)` for per-app dockspace isolation |
| `src/imgui/imgui_dock_layout.{hpp,cpp}` | `layout_identity()`/`should_apply_dock_layout()`/`note_dock_layout_applied()` fold `target_id` into the persisted identity (see key decision 8 -- fixes the "no window attached" regression) |

### Tests

| Test | File | Covers |
|---|---|---|
| `DockLayoutFamilyResolvesThroughImporter`, `DockLayoutDescriptorRoundTrips`, `DockAreaWindowsStaysAScalarString` | `test_ui_importer.cpp` | element registration + descriptor/`build_ui_node` path |
| 8 `DockLayout*` / `TwoLayoutsSharingOneDockspace*` cases in `test_imgui_renderer.cpp` | `DockBuilder` realization, side semantics, no-reapply / version bump, shared-dockspace takeover, focus, no-op, robustness, template shape |
| `EmbeddedDockSpaceViewportDocksIntoOuterAmbient`, `EmbeddedDockSpaceViewportNestedLayoutTargetsOwnNode`, `EmbeddedDockSpaceViewportRestoresOuterAmbientAfterward`, `DockerAndGitInSeparateShellsDoNotDisturbEachOther`, `DockLayoutReappliesWhenTargetChangesEvenAtSameVersion` | `test_imgui_renderer.cpp` | embedded-mode ambient docking, `DockLayout.target`'s id-hash fix realizing a physically distinct nested node, ambient-id restore after use, the concrete two-app isolation regression, and the "already applied under old target" regression (key decision 8) |
| `FormDockLayout.RegistersDockLayoutTopLevelObject`, `FormDockLayout.TornDownWithForm` | `test_form_base.cpp` | `set_default_dock_layout` registration + teardown |
| `RegistersDefaultDockLayout` + a close-teardown case in `test_docker.cpp`, `test_kubectl.cpp`, `test_git.cpp` | each module's dock-layout wiring (now asserts a `DockSpaceViewport` root, since each opts into `dock::viewport(...)`) |
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
