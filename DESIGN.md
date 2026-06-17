# wish — Architecture & Design

## Overview

wish is a remote UI framework. A **wish server** owns a native window and an imgui rendering context. **Client applications** connect over a bison RMI transport and build a UI by creating a tree of remote objects — one per UI element. The server traverses that tree on every imgui frame and calls the appropriate imgui draw primitives. User interactions (clicks, edits, drags) are emitted back to the client as named events.

The core transport, serialization, and remote-object protocol are entirely provided by the [bison](../extern/bison) library. wish adds only:
1. A registry of imgui-backed UI element classes in the `"wish"` bison namespace.
2. A per-frame renderer that traverses the live object tree.
3. Thin `server` and `client` base classes that wire bison's PTY/socket app scaffolds to the above.

---

## Architecture

```
+-------------------------+          bison RMI (PTY or TCP)
|      Client App         |  ---------------------------------------->+
|  bdg::wish::client      |  <-- events (clicked, changed, ...)        |
+-------------------------+                                             |
                                                               +--------+--------+
                                                               | bdg::wish::server|
                                                               |                  |
                                                               | bison rmi::server|
                                                               |   (per session)  |
                                                               |        |         |
                                                               | object tree      |
                                                               |  (dynamic_ptr[]) |
                                                               |        |         |
                                                               | wish::renderer   |
                                                               |   imgui backend  |
                                                               +------------------+
```

---

## Key Abstractions

### `bdg::wish::server`

Extends `bdg::bison::pty_server_app` (or can be paired with a `socket_server_transport` at runtime). Responsibilities:

- Calls `register_classes()` on startup to populate the `"wish"` bison namespace.
- Owns the imgui context, native window (e.g. SDL2/GLFW), and the frame loop thread.
- On every frame: locks the per-session object tree, traverses it depth-first via `wish::renderer`, calls `ImGui::Render()`, presents.
- Subclasses override `register_classes()` to add application-specific bison classes on top of the built-in wish classes.

### `bdg::wish::registry`

A free function (called by `server::register_classes()`) that registers every built-in UI element class in the `"wish"` bison namespace using `bison::dynamic::addClass("wish"_key, proto, parent_key)`.

Built-in classes and their key fields:

| Class | Key fields |
|-------|-----------|
| `Window` | `title`, `width`, `height`, `visible`, `children` |
| `Label` | `text`, `visible` |
| `Button` | `label`, `visible` |
| `Checkbox` | `label`, `value` (bool), `visible` |
| `SliderFloat` | `label`, `value` (float), `min`, `max`, `visible` |
| `SliderInt` | `label`, `value` (int32), `min`, `max`, `visible` |
| `InputText` | `label`, `value` (string), `hint`, `visible` |
| `Separator` | *(no fields)* |
| `Image` | `texture_id` (int32), `width`, `height`, `visible` |

All classes inherit from a base `Element` prototype that provides the `visible` field and the `children` indexed map.

Each class registers a `__setter` hook that sets a dirty flag on the session, ensuring the next imgui frame re-renders.

Event-emitting classes (Button, Checkbox, SliderFloat, SliderInt, InputText) call `context.emit_event(object_id, event_name, payload)` from inside the renderer when imgui reports user interaction.

### `bdg::wish::renderer`

Stateless per-frame visitor. Takes the root `bison::dynamic` object of the session and walks the tree recursively:

```
renderer::render(node)
  dispatch by node["__class"_key]:
    "Window"_key  -> ImGui::Begin / render children / ImGui::End
    "Button"_key  -> if ImGui::Button(...) -> emit "clicked" event
    "Label"_key   -> ImGui::Text(...)
    ...
  for each child in node["children"_key]:
    renderer::render(child)
```

Adding a new backend means subclassing `wish::renderer` and overriding the dispatch methods — the tree traversal logic is shared.

### `bdg::wish::client`

Extends `bdg::bison::pty_client_app`. Provides:

- `on_session(bison::rmi::client& c)` — the entry point the user overrides.
- Optional typed factory helpers that call `c.instantiate("wish"_key, ClassKey)` and return a `bison::rmi::proxy::dynamic`.

---

## Object Model

UI state lives in a tree of `bison::dynamic` instances owned by the RMI session context on the server.

```
Window (dynamic)
  ["title"_key]    = "My App"
  ["width"_key]    = 800
  ["height"_key]   = 600
  ["children"_key] = dynamic {
    [0]  -> Label  { ["text"_key] = "Hello" }
    [1]  -> Button { ["label"_key] = "OK" }
  }
```

- **Parent-child links** use the numeric-indexed field map (keys < 0x80000000) of the `children` field.
- **Field types** are the bison variant types: `bool`, `int32_t`, `float`, `std::string`, nested `dynamic`.
- **Class identity** is the `__class` reserved field, set automatically by `bison::dynamic::instantiate`.
- **Prototype inheritance** is handled by bison: field and method lookups walk the `__parent` chain, so `Element`-level fields like `visible` are available on all subclasses.

### Setter / Getter Flow

```
Client: proxy.set({{"text"_key, "Hello"}})   [oneway=true, no roundtrip]
  -> bison RMI OP_SET
  -> server __setter hook: mark session dirty
  -> next imgui frame: renderer reads node["text"_key] -> ImGui::Text("Hello")

Client: proxy.get({{"text"_key, {}}})        [projected get]
  -> bison RMI OP_GET
  -> server __getter hook (optional transform)
  -> returns {"text"_key: "Hello"}
```

### Event Flow

```
imgui frame: ImGui::Button("OK") returns true
  -> server renderer calls context.emit_event(btn_id, "clicked"_key, {})
  -> bison RMI KIND_EVENT envelope sent to client
  -> client: proxy.onEvent("clicked"_key, handler) fires handler
```

---

## Transport

The transport is selected at runtime via server/client launch arguments, not at compile time.

| Transport | Class | Use case |
|-----------|-------|---------|
| PTY | `pty_server_transport` / `pty_client_transport` | Local: client launches server as a subprocess via a PTY |
| TCP socket | `socket_server_transport` / `socket_client_transport` | Network: server runs as a standalone daemon |
| In-memory | `memory_server_transport` / `memory_client_transport` | Testing: no serialization overhead |

`bdg::wish::server` accepts a transport reference in its constructor, defaulting to PTY mode when built as a `pty_server_app`.

---

## Namespace Convention

| Layer | Identifier |
|-------|-----------|
| C++ namespace | `bdg::wish` |
| Bison class namespace key | `"wish"_key` (FNV-1a hash of `"wish"`) |
| Public header | `include/wish/wish.hpp` |

---

## Design Decisions

**Bison dynamic objects as UI state.**
Using bison's existing `dynamic` object model for the UI tree means no separate schema system is needed. The same serialization, inheritance, and RMI machinery bison already provides is reused for the UI hierarchy. Field type constraints (the variant type is locked on first assignment) give lightweight validation for free.

**imgui as the first renderer backend.**
Immediate-mode rendering maps directly onto a per-frame depth-first traversal of the object tree: there is no retained widget state to keep synchronized with the remote object state. Adding a retained-mode backend (Qt, Win32 controls) would require a reconciliation pass, but the renderer interface is designed to accommodate that.

**`oneway=true` for property sets.**
Visual updates from client to server do not need a response. Marking `set()` calls as one-way eliminates a round-trip on the hot path (e.g. dragging a slider should update the server at frame rate without waiting for an ack).

**No object ID knowledge on the client.**
Clients hold `proxy::dynamic` handles, not raw object IDs. The proxy is move-only and owns the lifetime contract. Passing proxies between client subsystems uses `std::move`.
