# wish — Architecture & Design

## Overview

wish is a remote UI framework. A **wish server** owns a native window and a UI rendering context. **Client applications** connect over a bison RMI transport and build a UI by defining a hierarchy of remote objects — one per UI element. The server traverses that tree on every render frame and calls the appropriate UI backend draw primitives. User interactions (clicks, edits, drags) flow back to the client as named events.

The core transport, serialization, and remote-object protocol are entirely provided by the [bison](../extern/bison) library. wish adds:

1. A **registry** of UI element classes in the `"wish"` bison namespace.
2. A **renderer** abstract interface, with an imgui backend as the initial implementation.
3. A **JSON/YAML importer** that parses a UI hierarchy descriptor and instantiates the full tree.
4. A **template registry** for named, reusable UI blueprints.
5. A **file service** for per-session sandboxed resource storage.
6. A **style service** for per-session visual theme configuration.
7. Thin `wish::server` and `wish::client` base classes that wire bison transports to the above.

---

## Architecture

```
+-------------------------+            bison RMI (TCP or in-memory)
|      Client App         |  ------------------------------------------>+
|  bdg::wish::client      |  <-- events (clicked, changed, ...)          |
|  - register_template()  |  <-- file_service (upload / download)        |
|  - upload_file()        |  <-- style_service (set/get/preset)          |
|  - set_style_preset()   |                                               |
+-------------------------+                                               |
                                                    +-----------------------+----------+
                                                    |  bdg::wish::server               |
                                                    |  : public bison::rmi::server     |
                                                    |                                  |
                                                    |  per-client wish::session        |
                                                    |   - object tree                  |
                                                    |   - template registry            |
                                                    |   - resource folder              |
                                                    |   - style_service (theme fields) |
                                                    |         |                        |
                                                    |  wish::renderer                  |
                                                    |   (abstract iface)               |
                                                    |    imgui_renderer (headless)     |
                                                    |      render_session: RAII style  |
                                                    |    sdl3_renderer  (windowed)     |
                                                    +----------------------------------+
```

Multiple clients connect to the same server. Each client gets an independent `wish::session` with its own object tree, template registry, and sandboxed resource folder. Sessions cannot access each other's state.

---

## Key Abstractions

### `bdg::wish::server`

Inherits from `bison::rmi::server` directly, so all bison RMI server capabilities (accept loop, worker threads, session context management, `session_contexts()` accessor) are available without delegation or wrapping.

The constructor accepts a `server_transport_iface&` so the same server logic works with TCP socket, or any other bison transport.

`wish::server` overrides two protected virtual hooks on `bison::rmi::server` — `on_session_created(context&)` and `on_session_destroyed(context&)` — as `final` methods that bridge into wish session management. Subclasses use the wish-level hooks `on_session_created(session&)` and `on_session_destroyed(session&)` instead.

Responsibilities:
- Calls `wish::registry::register_all()` on startup to populate the `"wish"` namespace.
- Owns the renderer (abstract `wish::renderer*`) and calls `renderer->render_node(root, session)` on each frame tick (~5 ms).
- Creates and destroys `wish::session` objects as clients connect and disconnect.
- Runs the bison RMI accept loop and the render loop on separate threads.

### `bdg::wish::registry`

Free function module (`wish/registry.hpp`). Registers every built-in UI element class in the `"wish"` bison namespace via `bison::dynamic::addClass("wish"_key, proto, parent_key)`.

This module is independent of any server or transport class and can be included in any application that wants to host a wish UI server.

Partial built-in class hierarchy (illustrative — see below for the full list):

```
Element  (visible, children)
  Window          (title, width, height, pos_x, pos_y, flags)
  Layout          (spacing: float)
    VerticalLayout
    HorizontalLayout
    Splitter        (orientation, thickness, min_pane_size)
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

The registry has grown well beyond this illustrative subset — `register_all()`
in `src/server/registry.cpp` is the authoritative, always-current list (menus,
tabs, tree/collapsing headers, combo/radio/selectable, tables, a text editor,
docking, and the full 2D (`Plot*`) and 3D (`Plot3D*`) plotting families). The
per-tab widget table in [docs/examples.md](docs/examples.md#demo-examplesdemo)
enumerates every widget currently exercised by the `demo` example, grouped by
category.

Layouts can be nested arbitrarily: a `VerticalLayout` can contain `HorizontalLayout` children (to build row-based grids), other `VerticalLayout` children (to create subsections), or any leaf elements.

Event-emitting classes (Button, Checkbox, Slider*, InputText) call `context.emit_event(object_id, event_name, payload)` from inside the renderer when the UI backend reports user interaction.

### Adding a new UI element class

Every server-side UI element class follows this pattern (see
`src/ui_elements/*.cpp` for reference implementations):

1. **Inherit `bison::dynamic`** and declare a constructor that accepts a `bison::dynamic&&` base.
2. **Register the prototype** in a free `register_*()` function called from `registry.cpp`. Build the prototype, add all methods and fields with `DisplayName` attributes, then call `dynamic::addClass()`.
3. **Put methods on the prototype, not in the constructor.** Methods registered on the prototype are visible to `build_display_dict()` (which powers trace output) and are found by dispatch via prototype-chain lookup.
4. **Access instance state via `static_cast<T&>(self)`** — prototype method lambdas receive the actual instance as `dynamic& self`, not `this`.
5. **Describe parameter fields with input/output specs** on the method constructor. This is how `build_display_dict()` resolves param key hashes to human-readable names in logs.

```cpp
// register_*(): prototype declares the full schema
void register_my_service() {
  auto proto = dynamic_ptr{"__MyService"_key, {}};

  auto call_in = std::make_shared<dynamic>();
  call_in->addField("value"_key, field{0, attr<DisplayName>("value")});

  proto->addMethod("doThing"_key, bison::method{
    [](dynamic& s, const dynamic& p) -> dynamic {
      static_cast<my_service&>(s).do_thing(p.as<int>("value"_key));
      return dynamic{};
    },
    dynamic_ptr{call_in}, nullptr,
    attr<DisplayName>("doThing")});

  dynamic::addClass("wish"_key, std::move(proto));
}

// Constructor: only initialization, no addMethod calls
my_service::my_service(bison::dynamic&& base) : dynamic(std::move(base)) {}
```

For classes that need a factory (server instantiates via RMI `INSTANTIATE`), see `register_ui_template()` in `src/ui/ui_template.cpp` for the `make_factory<T>` pattern.

### `bdg::wish::renderer` (abstract interface)

```cpp
class renderer {
public:
  virtual ~renderer() = default;

  // Lifecycle -- called from the render thread, not the constructor.
  virtual void setup()             {}              // before first frame
  virtual void teardown()          {}              // after last frame
  virtual bool should_quit() const { return false; }

  // Per-frame
  virtual void begin_frame() = 0;
  virtual void render_node(const ui_element& node, wish::session& s) = 0;
  virtual void end_frame()   = 0;

  // Per-session entry point (default: delegates to render_node).
  // Backends that support per-session style override this to apply a
  // session-scoped visual theme around the render_node call.
  virtual void render_session(const ui_element& root, wish::session& s) {
    render_node(root, s);
  }
};
```

**Lifecycle contract:** `setup()` is called once from the render thread before the first frame; `teardown()` is called once after the loop exits. After each `end_frame()` the render loop checks `should_quit()`; when `true` it sets `running_` to false and calls `teardown()`.

`render_node` is called recursively for each node in the object tree. The dispatch key is the `__class` reserved field. After handling the node itself, `render_node` calls `node.for_each_child_ordered(...)` to recurse into children in render order (ascending `order` field, using the cache built at import time).

Layout classes control how their children are arranged before recursing:

- **`VerticalLayout`** — renders children sequentially in the default imgui top-to-bottom flow, inserting `spacing` pixels of padding between items via `ImGui::SetCursorPosY`.
- **`HorizontalLayout`** — wraps children in `ImGui::BeginGroup()` / `ImGui::EndGroup()` and calls `ImGui::SameLine(0, spacing)` between items so they share a horizontal line.
- **`Splitter`** — same family, but interleaves a draggable `ImGui::InvisibleButton()` bar between each pair of children instead of fixed `spacing`, and writes the drag result back into each pane's own `width`/`height` field (the same field the two layouts above already read as a size hint on any child). See `render_splitter()` in `src/imgui/imgui_ui_renderer.cpp` for the drag/clamp/persist algorithm.

Non-layout containers (`Window`) render their children using the default vertical flow. A backend that is not immediate-mode would implement the same two layout types as a two-pass measure-then-place operation.

Concrete implementations:

- **`wish::imgui_renderer`** — Dear ImGui draw calls only; no windowing backend. Used in headless tests by providing a manually created `ImGuiContext`.
- **`wish::sdl3_renderer`** — extends `imgui_renderer`; creates an SDL3 window and SDL renderer inside `setup()` (which runs on the render thread). See below.

Adding a further backend (Qt, terminal/TUI) means implementing `wish::renderer` without changing any other wish component.

### `bdg::wish::sdl3_renderer`

Inherits `imgui_renderer`. Creates a real platform window via SDL3 and draws Dear ImGui's output using the SDL renderer backend (`imgui_impl_sdlrenderer3`).

**Thread model:** SDL3 requires that the window, renderer, and event pump all belong to the same thread. `sdl3_renderer` defers all SDL object creation to `setup()`, which `wish::server` calls from the render thread before the first frame. The same thread drives `begin_frame()` (which calls `SDL_PollEvent`), `end_frame()` (which calls `SDL_RenderPresent`), and `teardown()` (which destroys SDL objects). No additional locking is required.

**Window close:** When `SDL_PollEvent` returns an `SDL_EVENT_QUIT` event, `sdl3_renderer` sets an internal `quit_` flag. The render loop reads this via `should_quit()`, sets `running_` to false, and lets the loop exit naturally so `teardown()` cleans up on the same thread.

**Texture loading:** `get_or_load_texture(src, resource_dir)` loads `resource_dir / src` as a BMP via `SDL_LoadBMP`, uploads it via `SDL_CreateTextureFromSurface`, and caches the resulting `SDL_Texture*` as an `ImTextureID`. PNG loading would require SDL3_image (not currently a dependency). Cached textures are destroyed in `teardown()`.

### `bdg::wish::session`

Per-client state, owned by the server, created on connect and destroyed on disconnect.

```cpp
struct session {
  bison::key_t                                       id;            // assigned by bison RMI layer
  wish::name_map                                     objects;       // live ui_element tree (name -> ptr)
  std::unordered_map<bison::key_t, std::string, ...> templates;    // named UI blueprints (JSON/YAML)
  std::filesystem::path                              resource_dir;  // sandboxed folder
  std::atomic<bool>                                  dirty{false};  // application-managed flag
  file_service_ptr                                   file_service;  // per-session file sandbox
  style_service_ptr                                  style_service; // per-session visual theme
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

This pattern — subclassing `bison::dynamic` to attach typed behaviour to a registered class — is the standard way wish components add logic to data objects. Future wish objects that carry non-trivial behaviour (e.g. `file_service`, `ui_template`) should follow the same approach.

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

### `bdg::wish::object_inspector`

`ObjectInspector` (`src/ui/ui_elements/object_inspector.{hpp,cpp}`) is a Unity/Visual-Studio-style property inspector: given a `"target"` object, it reflects over `target`'s registered class (walking the full `PARENT` chain, via bison's attribute system — `DisplayName`, `Description`, `Range`, `Enum`, and five new attributes added alongside it: `Hidden`, `Order`, `ColorField`, `Multiline`, `DropTarget`) to build a `Table` of field rows (name + a type-appropriate editor) plus a description panel below. Full field → widget dispatch table and usage: [docs/object-inspector.md](docs/object-inspector.md).

It's registered as a real `"wish"`-namespace class like any other element, but is architecturally distinct from a plain leaf widget (`Checkbox`, `Table`) in one important way: **it cannot self-populate from its render function.** The render loop only ever holds a session's *read* lock (see this repo's `CLAUDE.md`, "Session threading model"), and creating new child elements — what building its table requires — is, everywhere else in this codebase, only ever done under the *write* lock (`ui_template::instantiate_prototype()`, `message_box::rebuild()`). So `ObjectInspector`'s table is built by an explicit `set_target()` call instead: either the `"__construct"`/`"set_target"` RMI methods (dispatched under the write lock automatically, following the same `HOOK_CONSTRUCT` mechanism `message_box` uses to apply `instantiate()`-time params), or server-side C++ code that already holds `context_wlock{*sync_ctx_}` — the same requirement `stamp_widget()`-style direct-construction code already follows elsewhere. Once built, it renders exactly like a `VerticalLayout` (its children are always `[Table, Label]`) — no bespoke render function needed, just an alias entry in `built_in_render_fns()`.

Row selection and field edits are **not** self-contained the way `message_box`'s own event handling is: a plain `ui_element` (unlike `form`) has no `on_event()` hook of its own — only whichever `form` the `ObjectInspector` instance is nested under gets that call (see "Event Flow" below: every event is routed to the enclosing top-level object's handler, regardless of nesting depth). `ObjectInspector` minimizes what that owning form needs to do: forward every event to `handle_row_event()` (it self-updates its own description panel, and is a no-op for anything that isn't its own `Table`), and to `handle_changed()`/`handle_dropped()`, which resolve and type-coerce the edit/drop and hand back a small `field_edit`/`field_drop` struct for the owning app's own command/apply logic — the same type-directed coercion (including `Enum`/`EnumFlags` string round-tripping) genie's original hand-rolled inspector used to duplicate per call site.

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

Registered as a bison class (`"__WishFileSystem"_key`) in the `"wish"` namespace. Exposes methods:

| Method | Parameters | Description |
|--------|-----------|-------------|
| `upload` | `name` (string), `data` (bytes as string) | Write file into `session.resource_dir` in one call |
| `download` | `name` (string) | Read file from `session.resource_dir`, return bytes in one call |
| `list` | — | Return list of uploaded file names |
| `delete` | `name` (string) | Remove a file from the session folder |
| `upload_chunk` | `name`, `data`, `first` (bool), `eof` (bool) | Append one chunk of a large upload; see below |
| `download_chunk` | `name`, `offset` (int32), `max_size` (int32) | Read one chunk of a large download at `offset`; see below |
| `unpack` | `zip_name` (string), `dest` (string) | Extract a previously-uploaded zip archive into a sandboxed directory |

Path traversal is blocked: `name` is validated to contain no directory separators or `..` components. The service only exposes files within `session.resource_dir`. The entire folder is removed when the client disconnects.

An `Image` element's `src` field is resolved relative to `session.resource_dir` by the renderer at draw time.

**Chunked transfer.** `upload`/`download` move an entire file in one RMI call, buffering the whole content in memory on both ends — fine for small files, wasteful for large ones. `upload_chunk`/`download_chunk` are a stateless alternative: no per-transfer session state is kept on the server, so calls can be retried or interleaved safely.
- `upload_chunk` writes each chunk to a staging file (`<resolved path>.wishpart`); `first=true` creates/truncates it, every call appends, and `eof=true` atomically renames it onto the final path — a transfer interrupted mid-stream never leaves a corrupt file at the user-visible name.
- `download_chunk` is pull-based: each call independently seeks to `offset` and reads up to `max_size` bytes, returning `{data, eof}`. No server-side read handle is kept between calls.
- `unpack` extracts a zip previously written into the sandbox (typically via `upload_chunk`) into a destination directory, using the same `resolve_path()` sandboxing for both `zip_name` and `dest`. Because the archive's *entries* are client-supplied content (unlike the build-controlled embedded resource archive `resource_store` unpacks), every entry's target path is independently re-validated against `dest` to block zip-slip (`../`-escaping entries). Extraction merges into `dest`; the staging archive is deleted on success.

`bdg::wish::client` exposes this as overloaded `upload_file`/`download_file` (a `std::string` overload for the simple case, and `std::istream&`/`std::ostream&` overloads that drive the chunked protocol under the hood) plus `upload_package(dest_path, istream&)`, which uploads a zip via the chunked path and calls `unpack` — e.g. `upload_package("my_folder/my_package", zip_stream)` extracts into `my_folder/my_package/` in the sandbox.

### `bdg::wish::style_service`

Registered in the `”wish”` bison namespace as `”__WishStyle”`. One instance is created per connected session in `server::on_session_created`; the session singleton is returned when the client calls `dynamic::instantiate(“wish”_key, “__WishStyle”_key)`.

**RMI methods:**

| Method   | Params                                      | Effect                                           |
|----------|---------------------------------------------|--------------------------------------------------|
| `set`    | flat dynamic (float / string fields)        | Merge style field overrides into the active style |
| `get`    | --                                          | Return current style fields as a flat dynamic     |
| `preset` | `”name”`: `”dark”` / `”light”` / `”classic”` | Reset to ImGui built-in preset, clear overrides |

Scalar float field keys: `alpha`, `disabled_alpha`, `window_rounding`, `window_border_size`, `child_rounding`, `frame_rounding`, `scrollbar_rounding`, `grab_rounding`, `tab_rounding`, etc. Vec2 fields are stored as `_x` / `_y` float pairs (e.g. `item_spacing_x`, `item_spacing_y`). Color fields use `#RRGGBBAA` hex strings (e.g. `color_button`, `color_window_bg`).

The `set` method **merges** into the existing style (partial update); `preset` **replaces** the entire style with a clean preset baseline.

**Renderer integration:** `imgui_renderer::render_session` reads `session.style_service->current_style()`, applies it to `ImGui::GetStyle()` using a RAII style guard, calls `render_node`, then restores the original style on return (even if rendering throws). This gives each session an independent visual theme without permanently mutating the global ImGui state across sessions or frames.

### `bdg::wish::client`

Thin wrapper around `bison::rmi::client`. Adds:

- `register_template(name, descriptor)` -- stores a named JSON/YAML blueprint on the server.
- `instantiate_template(name)` -- parses and instantiates a registered template, returns `std::future<proxy_map>`.
- `upload_file(name, bytes)` / `download_file(name)` -- whole-file service calls.
- `upload_file(name, istream&)` / `download_file(name, ostream&)` -- streaming, chunked equivalents for large files; never buffer the full content in memory.
- `upload_package(dest_path, istream&)` -- streams a zip archive to the server and has it unpacked into `dest_path` inside the sandbox.
- `set_style_preset(name)` -- apply a named ImGui theme preset (`”dark”`, `”light”`, `”classic”`).
- `set_style(params)` -- merge per-field style overrides (floats, `#RRGGBBAA` color strings).
- `get_style()` -- retrieve the current session style as a flat dynamic.

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
| TCP socket | `socket_server_transport` / `socket_client_transport` | Linux + MSYS2 | Network or local daemon |
| TLS-secured TCP | `tls_socket_server_transport` / `tls_socket_client_transport` | Linux + MSYS2 | Network or local daemon, over an untrusted network (`--transport=tls`; see [docs/cli.md](docs/cli.md#tls-flags---transport-tls)) |
| In-memory | `memory_server_transport` / `memory_client_transport` | Linux + MSYS2 | Unit tests and self-contained examples (e.g. calculator) |

`bdg::wish::server` accepts a `server_transport_iface&` reference, so transport selection is a runtime decision at the call site. No `#ifdef` inside the server or renderer.

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
| Public header | `src/wish.hpp` |

---

## `wish` Server Binary

`app/wish_cli/` builds the `wish` executable — a single binary dispatching to `server`, `client`, `standalone`, and `desktop` subcommands (`main.cpp`). `wish server` opens an SDL3 window or a `--renderer web` browser endpoint and listens for client connections on the transport selected at launch.

### CLI

See [docs/cli.md](docs/cli.md) for the full, authoritative flag reference (all four subcommands, transports, renderer options, `wish desktop`'s downstream/upstream flag sets, etc.) — kept here only at the level of "a CLI exists with subcommands" to avoid the two copies drifting out of sync.

### Lifecycle

`wish::server::start()` is non-blocking — it spawns the render loop thread and the bison accept loop thread, then returns immediately.  The binary polls `srv.should_quit()` (added to `wish::server`) every 50 ms; that flag is set by the render loop when `renderer_->should_quit()` fires (SDL window close).  After the flag is seen, `srv.stop()` closes the accept loop and joins threads.

---

## C Client ABI (`wish_client_c.h` / `libwish_client`)

`src/wish_client_c.h` and `src/wish_client_c.cpp` provide a stable C ABI for the client side.  The `WISH_BUILD_SHARED` CMake option (default OFF) builds `libwish_client.so` / `wish_client.dll`.

### Design

| Concept | C ABI | C++ equivalent |
|---------|-------|----------------|
| Session lifetime | `wish_client_tcp_create` / `wish_client_tls_create` / `wish_client_stream_create` / `wish_client_pipe_create` / `wish_client_term_create`, then `wish_client_run` / `wish_client_destroy` | `wish::client` subclass + `run()` |
| UI templates | `wish_register_template` + `wish_instantiate_template` | `register_template` + `instantiate_template` |
| Proxy access | `wish_proxy_get(c, "btns.ok")` | `pm.at("btns.ok")` |
| Field update | `wish_proxy_set_string(p, wish_key("text"), "Hi")` | `proxy.set({{"text"_key, "Hi"}})` |
| Events | `wish_proxy_on_event(p, "clicked", cb, ud)` | `proxy.onEvent("clicked"_key, handler)` |
| Wait for quit | `wish_client_wait(c)` inside session callback | `while (!rend->should_quit()) sleep(16ms)` |
| Automation (tree/screenshot/input, `WISH_ENABLE_AUTOMATION` + a renderer that supports it, e.g. SDL3) | `wish_automation_get_tree` / `wish_automation_get_logs` / `wish_automation_screenshot` / `wish_automation_mouse_move` / `wish_automation_mouse_button` / `wish_automation_key_event` / `wish_automation_text_input` | `client::get_automation_tree()` / `get_automation_logs()` / `take_screenshot()` / `inject_mouse_move()` / `inject_mouse_button()` / `inject_key()` / `inject_text()` |

Keys are computed via `wish_key(name)` which implements the same FNV-1a 32-bit hash (with MSB forced to 1) as the C++ `"name"_key` user-defined literal.

`wish_proxy_t` handles are non-owning pointers into a `std::unordered_map` owned by the `wish_client_s` struct.  They remain valid until the next `wish_instantiate_template` call or the session ends.

`config_panel` (`examples/config_panel/config_panel.c`) is the canonical demonstration: a pure-C settings panel application that connects to a running `wish` server and renders a configuration UI.

The automation row rides this same ABI and connection -- there is no separate socket or subprocess. It returns `WISH_ERR_NOT_FOUND` unless the connected server's active renderer implements native automation (currently only `sdl3_renderer`); see `src/automation/DESIGN.md`'s "Native (ABI-based) automation" section for the full architecture, including the browser-driven alternative used by the web renderer.

---

## Design Decisions

**Transport-agnostic server.**
`wish::server` inherits from `bison::rmi::server` and takes a `server_transport_iface&` at construction time rather than embedding a concrete transport type. This lets the same business logic (class registry, renderer, file service) run over TCP, or any future transport without code duplication.

**Abstract renderer interface.**
Decoupling the render pipeline from imgui means backends can be swapped without touching client code or the class registry. Immediate-mode (imgui) and retained-mode (Qt) backends share the same interface; the retained-mode case would add a reconciliation pass inside `render_node`.

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

The same pattern applies to any wish component that needs non-trivial logic on a registered bison class: `file_service` and `ui_template` each subclass `dynamic`, get constructed from a `dynamic&&` base (via `bison::dynamic::instantiate<T>()`), and expose their behaviour as member functions. `dynamic_cast<T*>` is the safe downcast path; bison's `forEachChild<T>` and `instantiate<T>` template helpers eliminate the boilerplate at call sites.

**Layouts as first-class container nodes.**
Layout behaviour (vertical vs. horizontal arrangement) belongs in the object tree rather than as a property on a generic container. This lets the renderer dispatch purely on `__class` — no conditional field checks — and lets the JSON/YAML descriptor express layout intent declaratively. It also allows arbitrary nesting: a `HorizontalLayout` row is itself a node whose children can be `VerticalLayout` columns, with no limit on depth.

**Per-session style service with RAII isolation.**
ImGui's style state is global (`ImGui::GetStyle()`). Applying a per-session theme naively would leak the style of the last session rendered into the next frame's default state. `imgui_renderer::render_session` solves this with a RAII guard: it saves the global `ImGuiStyle` before each session's render, applies that session's theme (read from `sess.style_service->current_style()`), calls `render_node`, and restores the original style on scope exit -- even if rendering throws. This way, each session sees its own theme throughout its render pass while the global style is always restored before the next session or frame begins. Storing style as a flat `bison::dynamic` field map (no ImGui types in `session.hpp`) keeps `style_service.hpp` free of ImGui dependencies and the field map accessible to the renderer's merge/apply logic via `findField`.

**Renderer lifecycle hooks (`setup`/`teardown`) instead of constructor init.**
SDL3 (and most GPU APIs) require that the window, renderer, and event pump all be created and driven by the same OS thread. The `wish::server` render loop runs on a dedicated render thread spawned by `server::start()`. If `sdl3_renderer` initialized SDL in its constructor (which runs on the main thread), SDL objects would be shared across threads with no synchronization. Deferring initialization to `setup()` — called from the render thread immediately before the first frame — keeps all SDL state on one thread for its entire lifetime. `teardown()` mirrors this by running on the render thread after the loop exits. The `should_quit()` hook gives the render thread a way to signal the rest of the application that the window has been closed, without requiring any shared condition variable or cross-thread call into the server.

