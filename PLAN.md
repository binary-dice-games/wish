# wish â€” Implementation Plan

Each step produces a self-contained, testable deliverable. Steps are ordered so that every dependency is in place before it is needed. An AI agent should complete one step fully (code + tests passing) before moving to the next.

---

## Step 1 â€” CMake scaffolding and build skeleton

**Goal:** The project builds and the test harness runs, even with empty implementation stubs.

**Deliverables:**
- `include/wish/wish.hpp` â€” top-level umbrella header (empty `bdg::wish` namespace declaration).
- `src/wish.cpp` â€” empty translation unit that includes `wish.hpp`.
- `tests/CMakeLists.txt` â€” links against `wish` and `bison`; discovers tests with `gtest_discover_tests`.
- `tests/test_stub.cpp` â€” single smoke test `TEST(Stub, Builds) { SUCCEED(); }`.
- `examples/CMakeLists.txt` â€” placeholder (no targets yet).
- Update root `CMakeLists.txt` to correctly link `wish` against the bison target from `extern/bison`.

**Tests:**
- `cmake -S . -B build && cmake --build build` succeeds with no errors on both Windows and Linux.
- `ctest --test-dir build` runs and the stub test passes.

---

## Step 2 â€” `wish::registry`: built-in UI class registration

**Goal:** All built-in UI element classes exist in the `"wish"` bison namespace and can be instantiated.

**Deliverables:**
- `include/wish/registry.hpp` â€” declares `void register_all()`.
- `src/registry.cpp` â€” implements `register_all()` by calling one `register_*` function per element type (defined in the files below). Registration order must respect the parent-before-child dependency: `Element` first, then `Layout`, then all leaf classes.
- `src/ui_elements/element.cpp` â€” registers the `Element` base prototype: fields `visible` (bool, true), `children` (dynamic, {}), and `order` (int32, 0). The `order` field controls render sequence within a parent's children; lower values render first. Also declares and exposes `bison::key_t element_key()` so child registrations can reference the parent key without hard-coding it.
- `include/wish/ui_element.hpp` + `src/ui_element.cpp` â€” the `ui_element` C++ class:
  - `class ui_element : public bison::dynamic` â€” all wish UI objects are instances of this type.
  - `explicit ui_element(bison::dynamic&& base)` â€” constructs from a base `dynamic` value produced by `dynamic::instantiate`.
  - `void refresh_children_order()` â€” reads each child's `order` field, stable-sorts ascending, and caches the sorted key sequence in a reserved `__children_order__` field on `*this`. Call once after import and again whenever an `order` field is mutated at runtime.
  - `void for_each_child_ordered(fn)` â€” iterates children via the cache when present; falls back to `forEachChild<ui_element>` when no cache exists.
  - `using ui_element_ptr = std::shared_ptr<ui_element>` â€” canonical pointer type for wish elements throughout the codebase.
- `src/ui_elements/layout.cpp` â€” registers the `Layout` intermediate prototype (parent: `Element`) with field `spacing` (float, 0.0f). Registers `VerticalLayout` and `HorizontalLayout` (parent: `Layout`), which add no fields of their own.
- `src/ui_elements/window.cpp` â€” registers `Window` (parent: `Element`) with fields `title` (string), `width` (int32), `height` (int32), `pos_x` (int32), `pos_y` (int32), `flags` (int32).
- `src/ui_elements/label.cpp` â€” registers `Label` (parent: `Element`) with field `text` (string).
- `src/ui_elements/button.cpp` â€” registers `Button` (parent: `Element`) with field `label` (string).
- `src/ui_elements/checkbox.cpp` â€” registers `Checkbox` (parent: `Element`) with fields `label` (string), `value` (bool).
- `src/ui_elements/slider.cpp` â€” registers `SliderFloat` (parent: `Element`) with fields `label` (string), `value` (float), `min` (float), `max` (float), `format` (string, "%.2f"); and `SliderInt` (parent: `Element`) with fields `label` (string), `value` (int32), `min` (int32), `max` (int32). Both slider variants live in one file because they share structure.
- `src/ui_elements/input_text.cpp` â€” registers `InputText` (parent: `Element`) with fields `label` (string), `value` (string), `hint` (string), `max_length` (int32, 256).
- `src/ui_elements/image.cpp` â€” registers `Image` (parent: `Element`) with fields `src` (string), `width` (int32), `height` (int32).
- `src/ui_elements/separator.cpp` â€” registers `Separator` (parent: `Element`), no additional fields.
- Each `src/ui_elements/*.cpp` file declares a corresponding `void register_<name>()` free function (e.g. `register_window()`, `register_label()`). These are internal to the library (not in any public header); only `registry.hpp` / `register_all()` is public.
- All class keys use `"ClassName"_key` (FNV-1a hashing from bison).
- All registrations use `bison::dynamic::addClass("wish"_key, proto, parent_key)`.
- Root `CMakeLists.txt` lists every `src/ui_elements/*.cpp` file explicitly in the `wish` target sources.

**Tests** (`tests/test_registry.cpp`):
- `register_all()` does not throw.
- Calling `register_all()` twice is idempotent (no duplicate registration error).
- `bison::dynamic::instantiate("wish"_key, "Window"_key)` returns a valid object.
- Instantiated `Window` has field `visible` (inherited from `Element`) with value `true`.
- Instantiated `Window` has field `title` with string type.
- Instantiated `VerticalLayout` has field `spacing` (inherited from `Layout`).
- Instantiated `HorizontalLayout` has field `spacing` (inherited from `Layout`).
- Instantiated `Button` has field `label` with string type.
- Instantiated `Checkbox` has field `value` with bool type.

---

## Step 3 â€” `wish::ui_importer`: JSON/YAML to object tree

**Goal:** A descriptor string (JSON or YAML) is parsed into a live tree of bison `dynamic` instances, all registered in the `"wish"` namespace.

**Deliverables:**
- `include/wish/ui_importer.hpp` â€” declares:
  ```cpp
  // name_map: flat map from element path ("body.row.ok") to the ui_element_ptr
  using name_map = std::unordered_map<std::string, ui_element_ptr>;
  name_map import_json(const std::string& json);
  name_map import_yaml(const std::string& yaml);
  ```
- `src/ui_importer.cpp` â€” implementation using `nlohmann::ordered_json` (preserves JSON object key declaration order) and `libyaml` (already in `extern/bison/extern/yaml`):
  - Reads `"type"` key to determine the class name.
  - Instantiates each element via `bison::dynamic::instantiate<ui_element>("wish"_key, type_key)`, returning a `ui_element_ptr` directly.
  - Sets all non-reserved fields (`type`, `children`, and `__`-prefixed keys excluded) on the new instance.
  - Recurses into `"children"`: string keys become named bison children (`"name"_key`); numeric string keys (`"0"`, `"1"`) become indexed bison children (`0U`, `1U`). Children are stored as `dynamic_ptr` (implicit upcast from `ui_element_ptr`) so the bison field system can hold them; `dynamic_cast` recovers the typed pointer when needed.
  - Stamps each child's `order` field with a monotonic counter (0, 1, 2 â€¦ in declaration order) unless the descriptor provides an explicit `order` value, enabling user-defined render sequence overrides.
  - Calls `obj->refresh_children_order()` on the parent after all children are built so the renderer can use `for_each_child_ordered`.
  - Collects every named node into the flat `name_map` (path = dot-joined ancestor names, e.g. `"body.row.ok"`).
  - Returns the name map; the root node is accessible as `name_map[""]` (empty string key).

**Tests** (`tests/test_ui_importer.cpp`):
- Parsing a JSON `Window` with no children produces a `Window` instance with correct field values.
- Parsing a JSON `Window` with a named `Button` child stores the button at `"ok"` in the name map.
- Parsing a JSON `Window` â†’ `VerticalLayout` â†’ two `HorizontalLayout` rows each containing a `Label` and a `Button` produces the correct tree depth and all named nodes in the map.
- Setting `"visible": false` in the JSON results in `visible` field = false on the instance.
- Numeric children `"0"` and `"1"` are accessible via integer index on the parent's `children` field.
- Unknown `"type"` value throws `std::runtime_error`.
- Invalid JSON throws `std::runtime_error`.
- YAML round-trip: the same hierarchy expressed in YAML produces the same name map as the JSON version.

---

## Step 4 â€” `wish::session`: per-client state container

**Goal:** A session object holds a client's object tree, template registry, and resource directory, and manages their lifetimes.

**Deliverables:**
- `include/wish/session.hpp` â€” declares:
  ```cpp
  struct session {
    bison::key_t                                   id;
    wish::name_map                                 objects;     // name -> ui_element_ptr (root at "")
    std::unordered_map<bison::key_t, std::string>  templates;   // name_key -> descriptor
    std::filesystem::path                          resource_dir;
    std::atomic<bool>                              dirty{false};

    explicit session(bison::key_t id);
    ~session();  // deletes resource_dir
    session(const session&) = delete;
    session& operator=(const session&) = delete;
  };
  ```
  The root `ui_element_ptr` is stored at `objects[""]`. All named descendants are also in the map, so the server renderer can walk the session tree via `objects[""]->for_each_child_ordered(...)` without a separate root pointer.
- `src/session.cpp` â€” constructor creates a unique temporary directory under the system temp path; destructor removes it recursively.

**Tests** (`tests/test_session.cpp`):
- Constructor creates `resource_dir` on disk.
- Destructor removes `resource_dir`.
- Two sessions have different `id` values and different `resource_dir` paths.
- `dirty` flag defaults to false and can be set atomically.
- Move construction transfers `resource_dir` ownership (destructor of moved-from does not delete).

---

## Step 5 â€” `wish::renderer`: abstract renderer interface

**Goal:** Define the contract that all rendering backends must satisfy; provide a null renderer for use in tests that don't need actual drawing.

**Deliverables:**
- `include/wish/renderer.hpp` â€” declares:
  ```cpp
  class renderer {
  public:
    virtual ~renderer() = default;
    virtual void begin_frame() = 0;
    virtual void render_node(const ui_element& node, session& s) = 0;
    virtual void end_frame() = 0;
  };

  // Walks the children map in render order and recurses into each child.
  // Concrete backends call this after drawing the node itself.
  void render_children(renderer& r, const ui_element& node, session& s);

  // No-op renderer for testing.
  class null_renderer : public renderer {
  public:
    void begin_frame() override {}
    void render_node(const ui_element&, session&) override {}
    void end_frame() override {}
  };
  ```
- `src/renderer.cpp` â€” implements `render_children`: calls `node.for_each_child_ordered(...)` to iterate children in ascending `order` field sequence (using the cache built at import time); calls `r.render_node(child, s)` for each.

**Tests** (`tests/test_renderer.cpp`):
- `null_renderer::render_node` can be called with a `Window` `ui_element` instance without throwing.
- `render_children` with a parent having two indexed children calls `render_node` exactly twice in index order (verified with a counting renderer subclass).
- `render_children` with a parent having two named children calls `render_node` exactly twice in declaration order (i.e. `order` field sequence, not hash sequence).
- `render_children` with no children calls `render_node` zero times.
- A counting renderer correctly accumulates calls across a three-level nested tree.

---

## Step 6 â€” `wish::file_service`: sandboxed resource store

**Goal:** Clients can upload and download files through a bison RMI object; files are stored in the session's resource directory and cleaned up automatically.

**Deliverables:**
- `include/wish/file_service.hpp` â€” declares `void register_file_service(session& s)`.
- `src/file_service.cpp` â€” implements `register_file_service`:
  - Registers a `"__WishFileSystem"_key` class in `"wish"_key` (once, idempotent).
  - Instantiates a `file_service : public bison::dynamic` (follow the `ui_element` pattern: subclass `dynamic`, construct from `dynamic&&`, add member functions for the service logic). This gives the file service typed methods and clean encapsulation rather than free functions that take `dynamic*`.
  - Exposes methods: `upload(name, data)`, `download(name)`, `list()`, `delete(name)`.
  - `upload`: validates `name` (no `/`, `\`, or `..`); writes binary content to `session.resource_dir / name`.
  - `download`: reads and returns the file content as a string field.
  - `list`: returns a dynamic with indexed string fields, one per file.
  - `delete`: removes the named file.
  - All methods capture a reference to the session's `resource_dir` at construction time.

**Tests** (`tests/test_file_service.cpp`):
- Upload then download round-trips file content correctly.
- `list()` returns the uploaded file names.
- `delete()` removes the file; subsequent `download` throws `std::runtime_error`.
- Path traversal attempt (`"../evil.txt"`) throws `std::runtime_error`.
- Name with a directory separator (`"sub/file.txt"`) throws `std::runtime_error`.
- Uploading to a deleted session resource directory (simulated) throws.

---

## Step 7 â€” `wish::server`: RMI server and session lifecycle

**Goal:** A transport-agnostic server accepts client connections, manages sessions, drives the render loop, and routes RMI operations to the correct session.

**Deliverables:**
- `include/wish/server.hpp` â€” declares:
  ```cpp
  class server : public bison::rmi::server {
  public:
    explicit server(bison::rmi::transport::server_transport_iface& transport,
                    std::unique_ptr<renderer> r);

    void start();   // register_all(), start render loop, begin accepting
    void stop();    // stop accept loop, render loop, join all threads

  protected:
    virtual void on_session_created(session& s) {}
    virtual void on_session_destroyed(session& s) {}
  };
  ```
- `src/server.cpp` â€” implementation:
  - `wish::server` inherits `bison::rmi::server` directly; no pimpl.
  - `start()` calls `wish::registry::register_all()`, starts the render thread, then calls `bison::rmi::server::listen()`.
  - `stop()` stops the render thread and calls `bison::rmi::server::stop()`.
  - Overrides two new protected virtual hooks on `bison::rmi::server` (`on_session_created(context&)` and `on_session_destroyed(context&)`) as `final` methods to manage `wish::session` objects. These bridge into the wish-level `on_session_created(session&)` / `on_session_destroyed(session&)` hooks for subclasses.
  - Session IDs are taken directly from `bison::rmi::context::session_id`; no separate counter.
  - Render loop renders every active session every tick (~5 ms sleep); no dirty gate.
  - On client disconnect, `on_session_destroyed(session&)` fires and the session is destroyed (triggers `resource_dir` cleanup).

**Tests** (`tests/test_server.cpp`) â€” use `memory_server_transport` and `null_renderer`:
- `start()` then `stop()` does not hang or throw.
- A client connecting via in-memory transport triggers `on_session_created`.
- A client disconnecting triggers `on_session_destroyed`.
- `OP_INSTANTIATE("wish"_key, "Window"_key)` from a memory client succeeds and returns a valid object ID.
- `OP_SET` on a `Window`'s `title` field applies the value (verified by a subsequent `OP_GET`).
- Two simultaneous in-memory clients each receive their own session.

---

## Step 8 â€” `wish::client`: client-side base class

**Goal:** A base class inheriting `bison::rmi::client` that adds wish-specific helpers; concrete
applications subclass it and override `on_session()`.

**Bison change:** `bison::rmi::client` destructor made `virtual` (committed to `d:\github\bison`
at `00f9203`; submodule updated). No other bison changes required.

**Deliverables:**
- `include/wish/client.hpp` â€” declares:
  ```cpp
  class client : public bison::rmi::client {
  public:
    explicit client(
        std::unique_ptr<bison::rmi::transport::client_transport_iface> transport);

    /** Connect, call on_session(), then disconnect. */
    void run();

    std::future<void> register_template(bison::key_t name, const std::string& descriptor);
    std::future<wish::proxy_map> instantiate_template(bison::key_t name);
    std::future<void> upload_file(const std::string& name, const std::string& data);
    std::future<std::string> download_file(const std::string& name);

  protected:
    /** Called after connect(); subclass performs all UI interaction here. */
    virtual void on_session() = 0;
  };
  ```
- `src/client.cpp` â€” implementation:
  - `run()`: calls `connect()`, then `on_session()`, then `disconnect()` (exception-safe).
  - `register_template`: stores the descriptor on the server by calling the `__WishTemplate` object's `register` method.
  - `instantiate_template`: calls `__WishTemplate`'s `instantiate` method; returns handle map.
  - `upload_file` / `download_file`: delegate to the `__WishFileSystem` object.

**Tests** (`tests/test_client.cpp`) â€” use `memory_client_transport` paired with a test server:
- `register_template` + `instantiate_template` with a valid JSON `Window` descriptor returns a non-empty name map.
- The name map entry for `""` (root) is a valid proxy.
- Named children appear in the name map with their dot-path keys.
- `register_template` + `instantiate_template` with an invalid descriptor propagates `std::runtime_error` via the future (error surfaces on `instantiate_template`).
- `upload_file` / `download_file` round-trips content via the server file service.
- Calling `instantiate_template` with an unregistered name propagates `std::runtime_error`.

---

## Step 9 â€” `wish::imgui_renderer`: imgui backend â€” leaf elements

**Goal:** A concrete `wish::renderer` that draws all non-layout UI elements using Dear ImGui. No windowing backend yet (uses an offscreen context for testing).

**Deliverables:**
- `include/wish/imgui_renderer.hpp` â€” declares `class imgui_renderer : public renderer`.
- `src/imgui_renderer.cpp` â€” implements `render_node(const ui_element& node, session& s)` for all leaf classes. Dispatch is on `node.as<bison::key_t>(bison::dynamic::CLASS)`:
  - `Window` â†’ `ImGui::Begin` / `ImGui::End`; after `Begin`, calls `render_children(r, node, s)`.
  - `Label` â†’ `ImGui::Text`.
  - `Button` â†’ `ImGui::Button`; on `true`, emits `"clicked"` event via `session.rmi_ctx.emit_event`.
  - `Checkbox` â†’ `ImGui::Checkbox`; on change, emits `"changed"` event with `{"value": bool}` payload; sets `value` field.
  - `SliderFloat` â†’ `ImGui::SliderFloat`; on change, emits `"changed"` with `{"value": float}`.
  - `SliderInt` â†’ `ImGui::SliderInt`; on change, emits `"changed"` with `{"value": int32}`.
  - `InputText` â†’ `ImGui::InputText`; on change, emits `"changed"` with `{"value": string}`.
  - `Image` â†’ resolves `src` relative to `session.resource_dir`; loads texture if not cached; calls `ImGui::Image`.
  - `Separator` â†’ `ImGui::Separator`.
  - Unknown `__class` â†’ logs warning and calls `render_children` as a passthrough.
- CMake: add `imgui` as an external dependency in `extern/` (or `FetchContent`); link `wish` against it; guard with `WISH_ENABLE_IMGUI` CMake option (default ON).

**Tests** (`tests/test_imgui_renderer.cpp`) â€” use a headless imgui context (`ImGui::CreateContext` without a platform backend):
- `begin_frame` / `end_frame` do not throw when called with a valid imgui context.
- `render_node` on a `Label` node does not throw and does not call `emit_event`.
- `render_node` on a `Button` node calls `emit_event("clicked", ...)` when `ImGui::Button` returns true (inject via imgui IO simulation).
- `render_node` on a `Checkbox` node emits `"changed"` with the correct boolean payload on state toggle.
- `render_node` on an unknown class does not throw.
- Rendering a `Window` containing two `Label` children calls the imgui Text function twice (verified via a render-count wrapper).

---

## Step 10 â€” `wish::imgui_renderer`: layout support

**Goal:** `VerticalLayout` and `HorizontalLayout` arrange their children correctly in the imgui render pass.

**Deliverables:**
- Extend `src/imgui_renderer.cpp`:
  - `VerticalLayout` â†’ iterates children; between each pair inserts `ImGui::SetCursorPosY(ImGui::GetCursorPosY() + spacing)` if `spacing > 0`.
  - `HorizontalLayout` â†’ calls `ImGui::BeginGroup()`; iterates children; between each pair calls `ImGui::SameLine(0.0f, spacing)`; calls `ImGui::EndGroup()`.

**Tests** (`tests/test_imgui_renderer.cpp` â€” extend existing file):
- `render_node` on a `VerticalLayout` with three `Label` children does not throw.
- `render_node` on a `HorizontalLayout` with three `Button` children does not throw.
- A nested tree `Window â†’ VerticalLayout â†’ [HorizontalLayout(Label, Button), HorizontalLayout(Label, Button)]` renders without error.
- `spacing = 0` and `spacing = 8` both render without error.

---

## Step 11 â€” Server-side template handler

**Goal:** The server exposes `__WishTemplate` as the sole descriptor RMI object so the client's `register_template` and `instantiate_template` calls work end-to-end.

**Deliverables:**
- `src/template_handler.hpp` + `src/template_handler.cpp`:
  - Registers `"__WishTemplate"_key` class in the `"wish"` namespace.
  - Implemented as `template_handler : public bison::dynamic` (same pattern as `ui_element`): construct from `dynamic&&`, hold `ctx_` and `sess_` directly (no intermediate base class).
  - Contains the internal `apply_descriptor(ctx, sess, descriptor)` free function: auto-detects JSON/YAML, parses with `import_json`/`import_yaml`, registers elements in the session and RMI context, returns an indexed dynamic result.
  - Member methods `register(name, descriptor)` and `instantiate(name)` operate on `session.templates` and `session.objects` via `apply_descriptor`.
  - `wish::server::on_create_object` injects context via `dynamic_cast<template_handler*>` and calls `init()` once per instance.
- No `import_handler` or `wish_handler` files; the descriptor-parsing path lives entirely in `template_handler.cpp`.

**Tests** (`tests/test_handlers.cpp`) â€” use in-memory transport:
- Client calls `register_template` then `instantiate_template` â†’ server parses, returns map â†’ client holds valid proxies.
- Calling `instantiate_template` with an unregistered name returns an error response (not a crash).
- Importing a hierarchy then calling `proxy.get()` on a named child returns the correct field values.

---

## Step 12 â€” End-to-end integration test

**Goal:** A full server + client test over in-memory transport exercises the entire stack: registration, import, property set, event emission, file upload, and clean disconnect.

**Deliverables:**
- `tests/test_integration.cpp`:
  - Starts a `wish::server` with `memory_server_transport` and `null_renderer`.
  - Connects a `wish::client` with `memory_client_transport`.
  - Registers a named template and instantiates a `Window â†’ VerticalLayout â†’ [Label, Button]` from JSON.
  - Sets the `Label`'s `text` to `"Hello"` via `proxy.set`; verifies the server-side field value.
  - Registers a `"clicked"` event handler on the `Button` proxy; simulates the event from the server side via `emit_event`; verifies the handler fires.
  - Uploads a file, lists files, downloads and verifies content, deletes the file.
  - Disconnects the client; verifies `session.resource_dir` no longer exists.

**Tests:** The `test_integration.cpp` file itself is the test. All assertions must pass in a single `TEST_F` suite.

---

## Step 13 â€” PTY transport integration (Linux only)

**Goal:** The server and client both work with `pty_server_transport` and `pty_client_transport` on Linux builds.

**Deliverables:**
- `examples/pty_server/main.cpp` â€” a minimal wish server using PTY transport and `null_renderer` (imgui windowing is a separate concern).
- `examples/pty_client/main.cpp` â€” a minimal wish client using PTY transport that imports a `Window â†’ Button` UI, prints a message on `"clicked"`, and exits on Enter.
- Both examples are conditionally compiled with `if(UNIX)` in `examples/CMakeLists.txt`.

**Tests:**
- On Linux: `cmake --build build --target pty_server pty_client` succeeds.
- Manual smoke test (not automated): running `pty_server` and connecting with `pty_client` produces a working session with event delivery.
- On Windows: the conditional compilation means `pty_server` and `pty_client` targets do not exist; the rest of the build is unaffected.

---

## Step 14 â€” TCP socket integration example

**Goal:** The same server logic works over a TCP socket for network deployments.

**Deliverables:**
- `examples/socket_server/main.cpp` â€” wish server using `socket_server_transport` on port 7070, `null_renderer`.
- `examples/socket_client/main.cpp` â€” wish client using `socket_client_transport` connecting to `127.0.0.1:7070`; imports the same `Window â†’ Button` UI as the PTY example.
- Both target Windows and Linux.

**Tests** (`tests/test_socket_transport.cpp`):
- Server listens on an ephemeral port; client connects; `register_template` + `instantiate_template` round-trip succeeds; client disconnects; server session is cleaned up.
- Two clients connect simultaneously; each receives an independent session; one disconnecting does not affect the other.

---

## Completion Criteria

The project is complete when:
1. `cmake -S . -B build && cmake --build build` succeeds on both Windows (MSVC or MinGW) and Linux (GCC or Clang).
2. `ctest --test-dir build` passes all tests with no failures.
3. The socket example server and client can exchange a `"clicked"` event end-to-end.
4. On Linux, the PTY example also works.
5. All public headers have Doxygen `@brief` comments.
6. `clang-format` reports no diff against the committed sources.

