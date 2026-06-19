# wish — Architecture & Design

## Overview

wish is a remote UI framework. A **wish server** owns a native window and a UI rendering context. **Client applications** connect over a bison RMI transport and build a UI by defining a hierarchy of remote objects — one per UI element. The server traverses that tree on every render frame and calls the appropriate UI backend draw primitives. User interactions (clicks, edits, drags) flow back to the client as named events.

The core transport, serialization, and remote-object protocol are entirely provided by the [bison](../extern/bison) library. wish adds:

1. A **registry** of UI element classes in the `"wish"` bison namespace.
2. A **renderer** abstract interface, with an imgui backend as the initial implementation.
3. A **JSON/YAML importer** that parses a UI hierarchy descriptor and instantiates the full tree.
4. A **template registry** for named, reusable UI blueprints.
5. A **file service** for per-session sandboxed resource storage.
6. Thin `wish::server` and `wish::client` base classes that wire bison transports to the above.

---

## Architecture

```
+-------------------------+            bison RMI (PTY or TCP)
|      Client App         |  ------------------------------------------>+
|  bdg::wish::client      |  <-- events (clicked, changed, ...)          |
|  - register_template()  |  <-- file_service (upload / download)        |
|  - upload_file()        |                                               |
+-------------------------+                                               |
                                                    +-----------------------+----------+
                                                    |  bdg::wish::server               |
                                                    |  : public bison::rmi::server     |
                                                    |                                  |
                                                    |  per-client wish::session        |
                                                    |   - object tree                  |
                                                    |   - template registry            |
                                                    |   - resource folder              |
                                                    |         |                        |
                                                    |  wish::renderer                  |
                                                    |   (abstract iface)               |
                                                    |    imgui backend                 |
                                                    +----------------------------------+
```

Multiple clients connect to the same server. Each client gets an independent `wish::session` with its own object tree, template registry, and sandboxed resource folder. Sessions cannot access each other's state.

---

## Key Abstractions

### `bdg::wish::server`

Inherits from `bison::rmi::server` directly, so all bison RMI server capabilities (accept loop, worker threads, session context management, `session_contexts()` accessor) are available without delegation or wrapping.

The constructor accepts a `server_transport_iface&` so the same server logic works with PTY, TCP socket, or any other bison transport.

`wish::server` overrides two protected virtual hooks on `bison::rmi::server` — `on_session_created(context&)` and `on_session_destroyed(context&)` — as `final` methods that bridge into wish session management. Subclasses use the wish-level hooks `on_session_created(session&)` and `on_session_destroyed(session&)` instead.

Responsibilities:
- Calls `wish::registry::register_all()` on startup to populate the `"wish"` namespace.
- Owns the renderer (abstract `wish::renderer*`) and calls `renderer->render_node(root, session)` on each frame tick (~5 ms).
- Creates and destroys `wish::session` objects as clients connect and disconnect.
- Runs the bison RMI accept loop and the render loop on separate threads.

### `bdg::wish::registry`

Free function module (`wish/registry.hpp`). Registers every built-in UI element class in the `"wish"` bison namespace via `bison::dynamic::addClass("wish"_key, proto, parent_key)`.

This module is independent of any server or transport class and can be included in any application that wants to host a wish UI server.

Built-in class hierarchy:

```
Element  (visible, children)
  Window          (title, width, height, pos_x, pos_y, flags)
  Layout          (spacing: float)
    VerticalLayout
    HorizontalLayout
  Label           (text)
  Button          (label)
  Checkbox        (label, value: bool)
  SliderFloat     (label, value: float, min, max, format)
  SliderInt       (label, value: int32, min, max)
  InputText       (label, value: string, hint, max_length)
  Image           (src: string, width, height, tint_r/g/b/a)
  Separator
```

`Layout` is an intermediate base class that contributes the `spacing` field and the children iteration contract. `VerticalLayout` and `HorizontalLayout` add no fields of their own; they differ only in how the renderer arranges their children.

Layouts can be nested arbitrarily: a `VerticalLayout` can contain `HorizontalLayout` children (to build row-based grids), other `VerticalLayout` children (to create subsections), or any leaf elements.

Event-emitting classes (Button, Checkbox, Slider*, InputText) call `context.emit_event(object_id, event_name, payload)` from inside the renderer when the UI backend reports user interaction.

### `bdg::wish::renderer` (abstract interface)

```cpp
class renderer {
public:
  virtual ~renderer() = default;
  virtual void begin_frame() = 0;
  virtual void render_node(const ui_element& node,
                           wish::session& session) = 0;
  virtual void end_frame() = 0;
};
```

`render_node` is called recursively for each node in the object tree. The dispatch key is the `__class` reserved field. After handling the node itself, `render_node` calls `node.for_each_child_ordered(...)` to recurse into children in render order (ascending `order` field, using the cache built at import time).

Layout classes control how their children are arranged before recursing:

- **`VerticalLayout`** — renders children sequentially in the default imgui top-to-bottom flow, inserting `spacing` pixels of padding between items via `ImGui::SetCursorPosY`.
- **`HorizontalLayout`** — wraps children in `ImGui::BeginGroup()` / `ImGui::EndGroup()` and calls `ImGui::SameLine(0, spacing)` between items so they share a horizontal line.

Non-layout containers (`Window`) render their children using the default vertical flow. A backend that is not immediate-mode would implement the same two layout types as a two-pass measure-then-place operation.

The **imgui backend** (`wish::imgui_renderer`) is the only concrete implementation initially. Adding a new backend (Qt, Win32 controls, terminal/TUI) means implementing `wish::renderer` without changing any other wish component.

### `bdg::wish::session`

Per-client state, owned by the server, created on connect and destroyed on disconnect.

```cpp
struct session {
  bison::key_t                                       id;           // assigned by bison RMI layer
  wish::name_map                                     objects;      // live ui_element tree (name → ptr)
  std::unordered_map<bison::key_t, std::string, ...> templates;   // named UI blueprints (JSON/YAML)
  std::filesystem::path                              resource_dir; // sandboxed folder
  std::atomic<bool>                                  dirty{false}; // application-managed flag
  file_service_ptr                                   file_service; // set by register_file_service()
};
```

`resource_dir` is a temporary directory created at session start and deleted at session end. Clients can only read and write within this directory via the file service.

`dirty` is an application-managed flag — `wish::server` does not read or write it. The render loop renders every session every tick; callers may use `dirty` for their own throttling or change-detection logic.

### `bdg::wish::ui_element`

`ui_element` is the C++ class for all wish UI objects. It inherits from `bison::dynamic`, so every element is a fully functional bison object (can be registered, inherited, stored in the field map, serialized over RMI) while also carrying wish-specific behaviour as member functions.

```cpp
class ui_element : public bison::dynamic {
public:
  explicit ui_element(bison::dynamic&& base);

  // Rebuild the render-order cache from each child's 'order' field.
  // Call after import or after mutating a child's 'order' at runtime.
  void refresh_children_order();

  // Iterate children in render order (ascending 'order' field).
  // fn receives (key_t, ui_element&); non-ui_element children are skipped.
  void for_each_child_ordered(
      const std::function<void(bison::key_t, ui_element&)>& fn) const;
};

using ui_element_ptr = std::shared_ptr<ui_element>;
```

`ui_element_ptr` is stored in the `name_map` and in `session`. The underlying bison field system stores all dynamic children as `dynamic_ptr`; the shared ownership and virtual dispatch of `shared_ptr<ui_element>` is preserved by an implicit upcast at assignment time.

This pattern — subclassing `bison::dynamic` to attach typed behaviour to a registered class — is the standard way wish components add logic to data objects. Future wish objects that carry non-trivial behaviour (e.g. `file_service_node`, `template_handler`) should follow the same approach.

### `bdg::wish::ui_importer`

Parses a JSON or YAML string and produces a tree of `ui_element` instances. Returns a flat `name_map` from dot-path name to `ui_element_ptr`.

```cpp
using name_map = std::unordered_map<std::string, ui_element_ptr>;

// Server side: instantiate locally from a descriptor string.
auto map = wish::import_json(descriptor);
map[""].   // root ui_element
map["body.ok"]-> // named descendant

// Client side: register a template and instantiate it by name.
client.register_template("ui"_key, descriptor).get();
auto handles = client.instantiate_template("ui"_key).get();
handles["ok"].onEvent("clicked"_key, handler);
```

The format mirrors the bison object model:

```json
{
  "type": "Window",
  "title": "My App",
  "width": 800,
  "height": 600,
  "children": {
    "header":  { "type": "Label",  "text": "Welcome" },
    "submit":  { "type": "Button", "label": "Submit" },
    "items": {
      "0": { "type": "Label", "text": "Item A" },
      "1": { "type": "Label", "text": "Item B" }
    }
  }
}
```

Named children (`"header"`, `"submit"`) use hashed string keys in the bison dynamic map. Numeric children (`"0"`, `"1"`) use numeric indices.

### `bdg::wish::template_registry` (per-session)

Stores named UI blueprint strings (JSON or YAML). Templates are registered by the client and instantiated on demand.

```
client: register_template("ConfirmDialog", descriptor_string)
  -> stored in session.templates["ConfirmDialog"_key]

client: instantiate_template("ConfirmDialog")
  -> server calls ui_importer::import(session.templates[name], session)
  -> returns name->id map to client
```

Templates are scoped to a session and are deleted with it.

### `bdg::wish::file_service`

Registered as a bison class (`"__WishFS"_key`) in the `"wish"` namespace. Exposes methods:

| Method | Parameters | Description |
|--------|-----------|-------------|
| `upload` | `name` (string), `data` (bytes as string) | Write file into `session.resource_dir` |
| `download` | `name` (string) | Read file from `session.resource_dir`, return bytes |
| `list` | — | Return list of uploaded file names |
| `delete` | `name` (string) | Remove a file from the session folder |

Path traversal is blocked: `name` is validated to contain no directory separators or `..` components. The service only exposes files within `session.resource_dir`. The entire folder is removed when the client disconnects.

An `Image` element's `src` field is resolved relative to `session.resource_dir` by the renderer at draw time.

### `bdg::wish::client`

Thin wrapper around `bison::rmi::client` (or `bison::pty_client_app` on Linux). Adds:

- `register_template(name, descriptor)` — stores a named JSON/YAML blueprint on the server.
- `instantiate_template(name)` — parses and instantiates a registered template, returns `std::future<proxy_map>`.
- `upload_file(name, bytes)` / `download_file(name)` — file service calls.

---

## Object Model

### Tree Structure

All nodes in the tree are `ui_element` instances (a `bison::dynamic` subclass). The `children` container itself is a plain `dynamic` that maps keys to `ui_element_ptr` values.

```
ui_element (Window, __class="Window")
  ["title"_key]    = "My App"
  ["width"_key]    = 800
  ["children"_key] = dynamic {
    ["body"_key] -> ui_element (VerticalLayout)    // named child: main column
      ["spacing"_key] = 4.0f
      ["children"_key] = dynamic {
        [0] -> ui_element (HorizontalLayout)        // row 0: two items side by side
          ["children"_key] = dynamic {
            ["label"_key]  -> ui_element (Label)  { ["text"_key]  = "Name:" }
            ["input"_key]  -> ui_element (InputText) { ["value"_key] = "" }
          }
        [1] -> ui_element (HorizontalLayout)        // row 1: action buttons
          ["children"_key] = dynamic {
            ["ok"_key]     -> ui_element (Button) { ["label"_key] = "OK"     }
            ["cancel"_key] -> ui_element (Button) { ["label"_key] = "Cancel" }
          }
      }
  }
```

Named children (`"body"`, `"label"`, `"ok"`) use hashed string keys (MSB set in the bison map). Numeric children (`[0]`, `[1]`) use integer indices. Both coexist in the same `children` dynamic container and are iterated by `for_each_child_ordered` in ascending `order` field sequence.

### Property Synchronization

```
Client: proxy.set({{"text"_key, "Hello"}})    // oneway, no round-trip
  -> bison RMI OP_SET (oneway=true)
  -> bison __setter hook on Element prototype applies the field
  -> render loop picks it up on the next tick (~5 ms)

Client: proxy.get({{"text"_key, {}}})         // projected get, has response
  -> bison RMI OP_GET
  -> returns {"text"_key: "Hello"}
```

### Event Flow

```
render frame: imgui Button("Submit") returns true
  -> renderer calls rmi_ctx.emit_event(node_id, "clicked"_key, {})
  -> bison RMI KIND_EVENT to client
  -> client proxy.onEvent("clicked"_key, handler) fires
```

---

## Transport & Platform

| Transport | Class | Platform | Use case |
|-----------|-------|----------|---------|
| PTY | `pty_server_transport` / `pty_client_transport` | Linux only | Client launches server as a subprocess via a PTY |
| TCP socket | `socket_server_transport` / `socket_client_transport` | Windows + Linux | Network or local daemon |
| In-memory | `memory_server_transport` / `memory_client_transport` | Windows + Linux | Unit tests |

`bdg::wish::server` accepts a `server_transport_iface&` reference, so transport selection is a runtime decision at the call site. No `#ifdef` inside the server or renderer. PTY-specific code lives only in the PTY transport and the `pty_client_app`/`pty_server_app` scaffolds, which are guarded by `#if defined(__linux__)`.

---

## Multi-Client Isolation

Each connected client receives its own `wish::session`. Sessions are stored in a map keyed by bison session ID:

```
server.sessions["session_id_1"_key] = wish::session { ... }
server.sessions["session_id_2"_key] = wish::session { ... }
```

No session can read or write another session's objects, templates, or resource folder. The renderer processes each session's object tree independently on each frame.

---

## Namespace Convention

| Layer | Identifier |
|-------|-----------|
| C++ namespace | `bdg::wish` |
| Bison class namespace key | `"wish"_key` (FNV-1a hash of `"wish"`) |
| Public header | `include/wish/wish.hpp` |

---

## Design Decisions

**Transport-agnostic server.**
`wish::server` inherits from `bison::rmi::server` and takes a `server_transport_iface&` at construction time rather than embedding a concrete transport type. This lets the same business logic (class registry, renderer, file service) run over PTY, TCP, or any future transport without code duplication. PTY is available as a launch wrapper on Linux.

**Abstract renderer interface.**
Decoupling the render pipeline from imgui means backends can be swapped without touching client code or the class registry. Immediate-mode (imgui) and retained-mode (Qt, Win32) backends share the same interface; the retained-mode case would add a reconciliation pass inside `render_node`.

**Bison dynamic objects as UI state.**
No separate schema system is needed. The same serialization, prototype inheritance, and RMI machinery bison provides is reused for the UI tree. Field type constraints (the variant is locked on first assignment) give lightweight validation automatically.

**Template-based descriptor import.**
Requiring clients to instantiate objects one by one is verbose and produces many round-trips. The template API (`register_template` + `instantiate_template`) sends a descriptor once, stores it server-side, and instantiates the full tree on demand — returning a name-to-id map. The same template can be instantiated multiple times without retransmitting the descriptor. The manual RMI API remains available for dynamic modifications after instantiation.

**Named and numeric children in the same map.**
Bison's dynamic map already supports both key kinds in one container (hashed names with MSB set vs. numeric indices below 0x80000000). wish uses this directly: named slots (e.g. `"ok"`, `"cancel"`) for structural child references, numeric slots for ordered lists. The renderer iterates both transparently.

**Per-session sandboxed file service.**
Allowing clients to reference arbitrary server-side paths would be a security risk. Scoping the file service to a temporary per-session directory and validating all names against path traversal attacks keeps the server safe. Automatic cleanup on disconnect prevents resource leaks.

**One-way property sets.**
Visual updates (set a label text, change a slider value) do not need acknowledgement. Marking these calls `oneway=true` removes a round-trip on the hot path, keeping the UI responsive at frame rate.

**Typed `dynamic` subclasses for wish objects.**
`bison::dynamic` is the data layer; wish adds behaviour by subclassing it. `ui_element : public bison::dynamic` is the canonical example: it is a fully-functional bison object (registered in the registry, stored in the field map, moved over RMI) while also owning wish-specific methods (`refresh_children_order`, `for_each_child_ordered`). This replaces the alternative of free functions that take a `dynamic&` parameter, which gives no type safety and scatters behaviour away from the data.

The same pattern applies to any wish component that needs non-trivial logic on a registered bison class: `file_service_node` and `template_handler` each subclass `dynamic`, get constructed from a `dynamic&&` base (via `bison::dynamic::instantiate<T>()`), and expose their behaviour as member functions. `dynamic_cast<T*>` is the safe downcast path; bison's `forEachChild<T>` and `instantiate<T>` template helpers eliminate the boilerplate at call sites.

**Layouts as first-class container nodes.**
Layout behaviour (vertical vs. horizontal arrangement) belongs in the object tree rather than as a property on a generic container. This lets the renderer dispatch purely on `__class` — no conditional field checks — and lets the JSON/YAML descriptor express layout intent declaratively. It also allows arbitrary nesting: a `HorizontalLayout` row is itself a node whose children can be `VerticalLayout` columns, with no limit on depth.
