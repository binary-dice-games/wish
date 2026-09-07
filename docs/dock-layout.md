# Tutorial — Shipping a default dock layout

By default, a wish `Window` with no `pos_x`/`pos_y` docks into the ambient
host dockspace, but with **no split geometry**: every such window lands in
the same node as tabs. A **`DockLayout`** lets your app declare the split
arrangement its windows should start in — a Docker-Desktop-style grid, an
IDE-style sidebar + editor + output, and so on — so a first-run user sees a
sensible layout without dragging anything.

Once applied, the arrangement is saved to `imgui.ini` like any manual drag.
The user rearranges freely from there; your default is applied again only
if you bump its `version`.

- [How it works](#how-it-works)
- [From a server-side form](#from-a-server-side-form)
- [Isolating one app's dockspace from others](#isolating-one-apps-dockspace-from-others)
- [From a client-registered template](#from-a-client-registered-template)
- [The tree grammar](#the-tree-grammar)
- [Versioning and re-applying](#versioning-and-re-applying)
- [Gotchas](#gotchas)

---

## How it works

`DockLayout` is an ordinary wish element (`DockLayout` › `DockSplit` ›
`DockArea`). It renders nothing. When the renderer reaches it and a
dockspace is available, it checks whether to apply:

- the `version` on the element is higher than the one last recorded as
  applied for this exact layout (persisted in `imgui.ini` under
  `[WishDockLayout]`) — covers a fresh `imgui.ini` (nothing recorded yet)
  and an author's deliberate `version` bump, **or**
- the target dock node doesn't exist at all (rare — e.g. a hand-edited
  `imgui.ini` missing its `[Docking]` data).

If so, it walks the `DockSplit`/`DockArea` tree and realizes it with ImGui's
`DockBuilder` API — one `DockBuilderSplitNode` per `DockSplit`, one
`DockBuilderDockWindow` per window named in a `DockArea` — then calls
`DockBuilderFinish`. Every later frame is a cheap "already applied" check.

Windows are matched **by path**: a `DockArea`'s `windows` field lists each
`Window`'s `__path__`. That's the form root key server-side, or the
descriptor dot-path in a template.

`DockLayout` is a **no-op** when there is no ambient dockspace (a plain
`wish server` with no host chrome) and no explicit `target`.

---

## From a server-side form

Build the tree with the `bdg::wish::dock::` helpers
(`src/ui/dock_layout_spec.hpp`) and hand it to
`form::set_default_dock_layout()` at the end of `on_init()`, after every
window is registered.

```cpp
#include <ui/dock_layout_spec.hpp>

void my_frontend::on_init() {
  // ... build_*_window() calls that register each Window and stamp its
  // __path__ (internal_root_key_, internal_root_key_ + "_logs", ...) ...

  using namespace bdg::wish::dock;
  set_default_dock_layout(layout(
      split(dir::left, 0.70f,
          // left 70%: a console strip along the bottom 25%, the main list above it
          split(dir::down, 0.25f,
              area({console_root_key_}),
              area({internal_root_key_})),
          // right 30%: logs and inspect, tabbed, logs selected
          area({logs_root_key_, inspect_root_key_}, /*focused=*/logs_root_key_))));
}
```

The `DockLayout` object is registered as a hidden top-level object and torn
down automatically with the form (`~form()` / `remove_internal_objects()`).

`dock::layout(root, version = 1, target = "")` wraps the tree;
`dock::split(dir, ratio, near, far)` and `dock::area({paths...}, focused =
"")` build the nodes. In `split`, `near` is the pane on the `dir` side and
takes `ratio` of the space; `far` fills the rest.

The docker module (`modules/bdg/dev/docker/server/docker.cpp`) is a
worked example.

---

## Isolating one app's dockspace from others

By default every app's windows auto-dock into the **one** shared, ambient
dockspace the host chrome publishes each frame (`ambient_dockspace_id()`,
set once by `host_renderer`/`wish_server_c.cpp`'s `dockspace_renderer`
before any session renders). That's fine when only one app is ever open,
but running app A, then app B, then app A again means all three runs
share the *same physical dock node* — B's layout can rearrange or evict
A's windows, because there's nothing to tell them apart.

`dock::viewport(id, title, layout(...))` wraps an app's layout in an
**embedded `DockSpaceViewport`**: the whole app then presents as a single
dockable tile in the host chrome (it docks into the ambient dockspace
like any other un-positioned window), and that tile hosts its own
**nested** dockspace for the app's own windows. Two apps wrapped this way
can never physically share a dock node, no matter what order they run
in.

```cpp
using namespace bdg::wish::dock;
set_default_dock_layout(viewport(
    "docker_dock", "Docker",
    layout(
        split(dir::left, 0.62f,
            split(dir::down, 0.24f, area({console_root_key_}), area({internal_root_key_})),
            area({logs_root_key_, inspect_root_key_}, logs_root_key_)),
        /*version=*/1, /*target=*/"docker_dock")));
```

Pass the same id string as both `viewport()`'s `id` and the wrapped
`layout()`'s `target`: `DockLayout.target` resolves a named (non-ambient)
dock id by hashing the string against the enclosing window's `ID`
(`ImHashStr(target, 0, window->ID)`) — the same seed `ImGui::GetID(id)`
uses immediately after that window's `Begin()`, which is exactly what the
`DockSpaceViewport` published for its own nested `ImGui::DockSpace()`
call one level up. Hashing against `window->ID` specifically (rather than
whatever is on top of the ID stack at the point the `DockLayout` happens
to render) matters because the `DockLayout` sits several levels into the
tree-walk dispatch, behind other elements' own `PushID()` scopes; giving
`viewport()` and `layout()` different id strings also targets the wrong
node.

`viewport()`'s `title` is purely cosmetic — the name shown on the tile's
tab/title bar (e.g. "Docker") — and never affects `id`. Internally, the
window's ImGui label is built as `"Title###id"`, the same pattern
`with_id()` uses for ordinary windows: `ImHashStr()` resets and hashes
only what follows `"###"`, so `ImGui::GetID("Docker###docker_dock")` and
`ImGui::GetID("docker_dock")` land on the identical id. Change `title`
freely; `id` (and therefore `layout()`'s `target`) must stay untouched.

**Limitation:** only windows *listed in the wrapped layout's `DockArea`
nodes* are guaranteed to land inside the nested dockspace — `DockLayout`
assigns them explicitly (`DockBuilderDockWindow(path, target_id)`),
independent of render order. A window created dynamically at runtime and
never added to any `DockArea` (e.g. docker's per-container Logs window)
still falls back to the **outer/host** ambient dockspace on its first
appearance, landing as its own tile next to the app's shell rather than
inside it — the app's nested dockspace id is only ambient while the
`DockSpaceViewport` element itself is rendering, and is restored to the
outer id immediately afterward. Solving this in general needs either
per-window explicit dock targets or making such windows real tree
children of the shell; until then, list every window you can predict
ahead of time in the `DockArea` tree, and treat windows outside it as
independently placed.

---

## From a client-registered template

A client that builds its UI as a template descriptor needs **no C++
helper** — the element classes are the shared representation. Put a
`DockLayout` node in the descriptor you register; the server resolves and
instantiates it with the rest of the tree.

```cpp
client.register_template_from_json("main"_key, R"({
  "type": "DockSpaceViewport", "id": "main",
  "children": {
    "explorer": { "type": "Window", "title": "Explorer" },
    "editor":   { "type": "Window", "title": "Editor" },
    "output":   { "type": "Window", "title": "Output" },
    "layout": { "type": "DockLayout", "version": 1, "children": [
      { "type": "DockSplit", "dir": "left", "ratio": 0.25, "children": [
        { "type": "DockArea", "windows": "explorer" },
        { "type": "DockSplit", "dir": "down", "ratio": 0.30, "children": [
          { "type": "DockArea", "windows": "output" },
          { "type": "DockArea", "windows": "editor" }
        ] }
      ] }
    ] }
  }
})").get();

client.instantiate_template("main"_key).get();
```

Here the window paths are the descriptor's own child names
(`explorer`, `editor`, `output`). For a nested window the path is
dot-joined (`panels.output`).

The `DockLayout` can be a child of a `DockSpaceViewport` (as above) or a
plain sibling of the windows when the app runs inside the host chrome's
ambient dockspace.

---

## The tree grammar

```
DockLayout   := { target?, version?, children: [ Node ] }     // exactly one child
Node         := DockSplit | DockArea
DockSplit    := { dir, ratio, children: [ Node, Node ] }       // exactly two, ordered
DockArea     := { windows, focused? }
```

| Field | On | Meaning |
|---|---|---|
| `target` | `DockLayout` | Dockspace id to seed. Empty ⇒ the ambient dockspace. |
| `version` | `DockLayout` | Revision; see [Versioning](#versioning-and-re-applying). |
| `dir` | `DockSplit` | `left` / `right` / `up` / `down`. First child goes here, second opposite. |
| `ratio` | `DockSplit` | 0..1 — the first child's share of the parent. |
| `windows` | `DockArea` | **Newline-separated** `Window` paths, in tab order. |
| `focused` | `DockArea` | Which of `windows` is the active tab (default: the first). |

`windows` is a newline-delimited string, **not** a JSON array — the client
descriptor importer silently drops array- and object-valued scalar fields,
so an array would vanish in a template.

---

## Versioning and re-applying

A `DockLayout` is applied **at most once per `version`** — full stop. Once
applied, nothing brings it back: not a drag, not floating a window out
standalone, not a sibling wish tool that shares the same host dockspace
(docker / kubectl / git all dock into the host chrome's one `HostDockSpace`)
rebuilding it wholesale with its own windows in between. From that point on
`imgui.ini` — ImGui's own window and dock-node persistence — owns the
arrangement, exactly like any window the user dragged by hand.

That means running a different tool that shares your dockspace, then coming
back to this one, does **not** restore this tool's split arrangement the way
an earlier design attempted — the earlier design inferred "a sibling rebuilt
the dockspace" from transient docking state, which also misfired on
ordinary drags and floats, so it was removed. If a sibling app displaces
your windows, they come back as plain floating windows on your next run
rather than being silently rebuilt back to the hard-coded default over
anything you'd customized. Give each tool with meaningfully different window
sets a distinct `target` if this matters for your app.

To push a *changed* default to users who never customized theirs, **bump
`version`** (`dock::layout(tree, 2)`, or `"version": 2` in a descriptor).
That forces one re-apply even over a user's own arrangement, so do it
deliberately.

**Changing `target` counts as a change, even at the same `version`.**
Moving a layout from the ambient dockspace to a named one (or from one
named `target` to another -- e.g. adopting `dock::viewport(...)`, see
above) is tracked as a distinct layout from its old target, so it applies
on the next run even though its window set and `version` didn't change.
You don't need to bump `version` for this specific case.

State is persisted per layout (keyed by its window-path list, not the
dockspace) in `imgui.ini`:

```
[WishDockLayout][0x0913a362]
Version=1
```

Deleting `imgui.ini` resets everything to a fresh first run.

---

## Render-order hazard: DockLayout must render before its own windows

`session::top_level_objects` is an `unordered_map`, so the order a
session's top-level objects render in each frame is **arbitrary**, not
insertion order and not sorted by key. That matters because a `Window`'s
very first `ImGui::Begin()` call resolves
`SetNextWindowDockID(ambient, ImGuiCond_FirstUseEver)` immediately, while
`DockLayout`/`DockSpaceViewport` reassign windows to their real split
nodes later, via `DockBuilderDockWindow()`. If one of an app's own
windows happens to render (and call that first-ever `Begin()`) *before*
its sibling `DockLayout`/`DockSpaceViewport` renders in the same frame —
which `unordered_map` iteration order can produce on any given run — the
window's placement for that frame is already resolved before
`DockBuilderDockWindow()` ever runs. ImGui's own end-of-frame dock-node
garbage collection then prunes the freshly-built, still-unhosted split
nodes, permanently collapsing the intended split into one shared tabbed
node — because `DockLayout` only ever applies once per version (see
above), there is no later frame where it gets a second chance.

This is a real, previously-shipped bug (not specific to nested
viewports): it reproduced reliably running `docker` standalone, with all
of docker's windows tabbed into a single node instead of the intended
62/38 split with a console strip. It was root-caused by instrumenting
the render path and observing, in a live server log, that all of
docker's `Window` top-levels called `Begin()` before its
`DockSpaceViewport`/`DockLayout` rendered that frame — and confirmed
fixed the same way, by observing the corrected ordering and the
resulting widget tree (via `docs/automation.md`'s Playwright
`getTree()` probe) live afterward.

The fix: both application render loops
([src/standalone/standalone.cpp](../src/standalone/standalone.cpp) and
[src/server/server.cpp](../src/server/server.cpp)) render a session's
top-level objects in **two passes** every frame — any
`DockLayout`/`DockSpaceViewport` top-level first, then everything else —
so `DockBuilderDockWindow()` assignments always land before sibling
windows' `Begin()` calls that same frame, regardless of
`top_level_objects`'s hash order. If you add a new place that iterates
and renders a session's top-level objects, you must preserve this
two-pass ordering or the same collapse can reappear.

---

## Gotchas

- **No ambient dockspace ⇒ nothing happens.** `DockLayout` needs either the
  host chrome's dockspace (`wish standalone`, `wish server` CLI, or a C-ABI
  server wrapped in `dockspace_renderer`) or a `DockSpaceViewport` in the
  tree, or an explicit `target`. A bare `wish server` with no host chrome
  ignores it.
- **`imgui.ini` is one shared file** next to the executable, keyed by hashed
  window ids. Give windows app-distinctive paths (`__mytool_0_logs`, not
  `logs`) so two tools' windows and layouts never collide.
- **Re-instantiating the same template twice** collides on window paths (a
  pre-existing template limitation); the second `DockLayout` sees its
  windows already live and is a no-op.
- **Docking still needs Shift.** wish sets `io.ConfigDockingWithShift`, so
  *manual* re-docking is Shift+drag. `DockLayout` is programmatic and
  unaffected.
- **Window size after undock.** Keep `width`/`height` on your `Window`s —
  they're the size a pane restores to when the user drags it out of the
  dock.
