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
- [From a client-registered template](#from-a-client-registered-template)
- [The tree grammar](#the-tree-grammar)
- [Versioning and re-applying](#versioning-and-re-applying)
- [Gotchas](#gotchas)

---

## How it works

`DockLayout` is an ordinary wish element (`DockLayout` › `DockSplit` ›
`DockArea`). It renders nothing. When the renderer reaches it and a
dockspace is available, it checks whether to apply:

- the target dock node doesn't exist yet (a fresh `imgui.ini`), **or**
- the `version` on the element is higher than the one last recorded as
  applied (persisted in `imgui.ini` under `[WishDockLayout]`).

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

The renderer applies a `DockLayout` when **any** of:

1. it has never been applied at its current `version` — the first run of a
   build carrying this layout, or after you bump the number;
2. the target dockspace has no dock-node tree yet (a fresh `imgui.ini`);
3. its tree exists but **this layout's windows aren't the ones currently
   arranged under the target** — e.g. another wish tool that shares the same
   dockspace (docker / kubectl / git all dock into the host chrome's one
   `HostDockSpace`) ran in between and rebuilt it.

Once a layout's windows *are* live under the target, it is left alone — so a
user's own rearrangement (which keeps every window docked under the target)
survives restarts. Re-open the same tool and you get your layout back; run a
different tool and it lays itself out; come back to the first and it
restores itself.

To push a *changed* default to users who never customized theirs, **bump
`version`** (`dock::layout(tree, 2)`, or `"version": 2` in a descriptor).
That forces one re-apply even over a user's own arrangement, so do it
deliberately.

State is persisted per layout (keyed by its window-path list, not the
dockspace) in `imgui.ini`:

```
[WishDockLayout][0x0913a362]
Version=1
```

Deleting `imgui.ini` resets everything to a fresh first run.

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
