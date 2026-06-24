# wish Forms — Architecture & Design

## Overview

**Forms** are high-level UI components that the wish server manages end-to-end. Where low-level wish elements (Button, InputText, Table, ...) expose raw drawing primitives that the client wires together, a form encapsulates an entire interaction pattern — its internal layout, widget logic, and state machine all live on the server. The client sees only a simplified, domain-level API: a handful of fields to populate, methods to call, and events to react to.

A form is a bison RMI object just like any other wish class. The client instantiates it, sets fields, and subscribes to events. Internally the form creates and owns a `ui_element` subtree (a Window with its children), registers it in the session's object map so the renderer draws it, and translates low-level widget events into high-level events that the client receives.

The motivation is consistency: an open-file dialog built on top of raw wish elements would require every client to re-implement selection logic, keyboard navigation, path display, and filtering. Building it once as a form gives all clients an identical, polished experience with a three-line integration.

---

## Design Goals

1. **Server-side logic, client-side data.** The server owns UI state (which row is selected, whether the filename field is focused). The client owns application data (which files exist in a folder). High-level events bridge the two.
2. **Hidden internals.** The client cannot directly access or modify a form's internal `ui_element` tree. Only the form's declared fields, methods, and events are part of its public contract.
3. **Composable with low-level elements.** A form's Window is a first-class node in the session's object tree. It can coexist with client-managed elements in the same session.
4. **Extensible via compile-time modules.** Optional sets of forms (desktop utilities, process monitors, ...) are compiled in via CMake options, not loaded as DLL plugins, for reasons detailed in [Module System](#module-system).
5. **Sandbox-safe.** Any form that reads or writes files must go through the same `file_service::resolve_path()` / `resolve_widget_path()` path as all other wish components.

---

## Architecture

```
Client App
  └─ form proxy (bison RMI object handle)
       ├─ set({files: [...], title: "..."})        ← client populates data fields
       ├─ onEvent("on_open",    handler)           ← client reacts to high-level events
       ├─ onEvent("on_navigate", handler)
       └─ onEvent("on_cancel",  handler)

wish server
  └─ form instance (bison::dynamic subclass)
       ├─ fields: files, title, ...                ← merged from client set() calls
       ├─ internal ui_element tree (Window + children)
       │    registered in session.objects, rendered every frame
       └─ internal event handlers
            Button "Open"  clicked → validate selection → emit("on_open", {path})
            Table  row dbl-click  → if dir → emit("on_navigate", {name, type})
            Button "Cancel" click → emit("on_cancel", {})
```

### Relationship to `ui_element`

A form is **not** a `ui_element`. It inherits from `bison::dynamic` directly and is never itself a node in the rendered tree. Instead, `on_init()` (called after the session context is injected) constructs an internal `ui_element` tree and registers its root at a private name in `session.objects`. The renderer picks it up on the next tick exactly as it would any client-built window. When the form is destroyed (client disconnects or explicitly closes it), the internal tree is removed from `session.objects`.

This separation keeps the renderer unaware of forms and lets form logic evolve independently of renderer backends.

---

## The `form` Base Class

```cpp
/// @brief Base class for all wish form objects.
///
/// Subclasses override on_init() to build their internal ui_element tree and
/// register prototype fields/methods/events. The server calls init() once,
/// immediately after the object is created, to supply the session context.
class form : public bison::dynamic {
 public:
  explicit form(bison::dynamic&& base);

  /// @brief Inject session context. Called exactly once by server::on_create_object.
  /// Calls on_init() after storing ctx and sess.
  void init(bison::rmi::context& ctx, std::shared_ptr<session> sess);

 protected:
  /// @brief Build the internal UI tree and set up prototype field/event handlers.
  /// ctx() and sess() are valid when this is called.
  virtual void on_init() = 0;

  /// @brief Emit a high-level event to the client.
  void emit(bison::key_t event_name, bison::dynamic payload = {});

  bison::rmi::context& ctx() { return *ctx_; }
  session&             sess() { return *sess_; }

 private:
  bison::rmi::context*  ctx_{nullptr};
  std::shared_ptr<session> sess_;
  std::string           internal_root_key_;  // key used in session.objects
};
```

`init()` is called from `server::on_create_object` via `dynamic_cast<form*>`, the same pattern used for `template_handler`. Every concrete form class must register itself in `registry.cpp` so the server knows the cast target.

---

## Form Lifecycle

```
client: instantiate("wish", "OpenFileDialog")
  → server: on_create_object() creates instance, calls form::init(ctx, sess)
    → on_init(): build Window + Table + Buttons, register in session.objects
    → fields are set to defaults (empty file list, default title)

client: form.set({files: [...], title: "Open..."})
  → server: bison __setter hook merges into the form's field map
  → render loop picks up changed fields on the next tick

client: form.onEvent("on_open", handler)
  → handler fires when user clicks Open

(user double-clicks a directory row)
  → form internal handler emits on_navigate({name: "docs", type: "dir"})
  → client handler: fetch new file list → form.set({files: [...]})

(user selects a file and clicks Open)
  → form emits on_open({path: "report.pdf"})
  → client handler reads the path and acts on it

client disconnects (or session ends)
  → form destroyed → internal Window removed from session.objects
```

---

## Client-Facing API Contract

Each form declares its public API in three categories:

| Category | Direction | Mechanism |
|----------|-----------|-----------|
| **Fields** | client → server (or server → client for outputs) | `proxy.set()` / `proxy.get()` |
| **Methods** | client → server | `proxy.call()` |
| **Events** | server → client | `proxy.onEvent()` |

Fields are the primary way the client feeds data into a form (e.g., a file list). Events are the primary output. Methods are for imperative control (e.g., `close()`, `focus()`).

All fields, methods, and events must be registered on the **prototype** (not in the constructor) so that `build_display_dict()` and the bison dispatch chain see them. Follow the same registration pattern as `template_handler` and other wish classes (see `register_*` free functions in `src/`).

---

## Module System

### Compile-time inclusion, not runtime plugins

The natural extension mechanism for a framework like wish is a runtime plugin loaded from a DLL. wish cannot use this approach because bison's class registry is a **process-global singleton** (`bison::dynamic::addClass` writes into a single in-process table). When a DLL is loaded, its static storage is separate from the hosting binary's static storage. Calls to `addClass` from inside the DLL write into the DLL's own copy of the registry, which the wish server never consults — the server's registry remains empty for those classes.

Optional form modules are therefore **compiled in** as additional source files, selected at CMake configuration time:

```cmake
# CMakeLists.txt (forms)
target_sources(wish PRIVATE
  src/forms/open_file_dialog.cpp   # always compiled: part of the base server
)

option(WISH_MODULE_DESKTOP "Include desktop utility forms (Calculator, Notepad, ...)" OFF)
if(WISH_MODULE_DESKTOP)
  target_sources(wish PRIVATE
    src/forms/desktop/calculator.cpp
    src/forms/desktop/notepad.cpp
  )
  target_compile_definitions(wish PRIVATE WISH_MODULE_DESKTOP)
endif()
```

`register_all()` in `registry.cpp` calls the module registration hooks behind the same `#ifdef`:

```cpp
void register_all() {
  // ... existing element/service registrations ...
  register_open_file_dialog();      // always present

#ifdef WISH_MODULE_DESKTOP
  register_desktop_module();        // Calculator, Notepad, ...
#endif
}
```

Because each module's code is statically linked into the server binary, `addClass` and `addField` write into exactly the same singleton that the server reads. No plugin loading machinery is required.

### Adding a new module

1. Create `src/forms/<module_name>/` with a `.cpp` per form and a `register_<module_name>_module()` free function.
2. Add a `CMakeLists.txt` `option()` and a corresponding `target_sources` + `target_compile_definitions` block.
3. Add the `#ifdef WISH_MODULE_<NAME>` guard and the `register_` call in `registry.cpp`.
4. Document the module in `README.md` under "Further Documentation" and in `docs/building.md` under CMake options.

---

## Built-in Forms

### `OpenFileDialog`

**Bison class name:** `"OpenFileDialog"` in the `"wish"` namespace.

**Purpose:** A modal file-picker dialog. The server manages all selection, filtering, and keyboard navigation logic. The client is responsible for providing the list of files available in the current directory and for acting on the selected path.

#### Fields

| Field | Type | Direction | Description |
|-------|------|-----------|-------------|
| `title` | string | client → form | Dialog window title. Default: `"Open File"`. |
| `files` | dynamic | client → form | Ordered list of entries. Each entry is a dynamic with `name` (string) and `type` (`"file"` or `"dir"`). Client repopulates this on every `on_navigate` event. |
| `filename` | string | client ↔ form | Current value of the filename input field. Client may preset it; form updates it as the user types or clicks. |
| `filters` | dynamic | client → form | Optional list of filter strings shown in a combo box (e.g., `{0: "*.txt", 1: "*.md"}`). Empty means no filter UI is shown. |
| `confirm_label` | string | client → form | Label on the confirm button. Default: `"Open"`. |

#### Events

| Event | Payload fields | Description |
|-------|---------------|-------------|
| `on_open` | `path` (string) | User confirmed selection. `path` is the value of the filename field at the moment of confirmation. It is always relative unless `allow_absolute_paths` is enabled server-side. |
| `on_navigate` | `name` (string), `type` (string) | User double-clicked an entry. For `type == "dir"` the client should update `files` with the new directory's contents. For `type == "file"` the client may treat this as a confirm (equivalent to `on_open`). |
| `on_cancel` | — | User dismissed the dialog without selecting a file. |

#### Internal UI structure (informative)

```
Window (title from field)
  VerticalLayout
    Label          ← current path display (updated by the client via on_navigate flow)
    Table          ← file list (rows from `files` field; single-click fills filename, dbl-click emits on_navigate)
    HorizontalLayout
      Label        ← "File name:"
      InputText    ← filename field (two-way bound to the `filename` field)
    HorizontalLayout (if filters non-empty)
      Label        ← "Filter:"
      Combo        ← filter list
    HorizontalLayout
      Button       ← confirm_label  → emits on_open
      Button       ← "Cancel"       → emits on_cancel
```

The internal tree is private. Clients must not attempt to access its nodes by name.

#### Example (C++)

```cpp
auto dlg = c.instantiate("wish"_key, "OpenFileDialog"_key).get();

// Populate the initial file list.
bison::dynamic entries;
int i = 0;
for (auto& e : list_sandbox_dir()) {
  bison::dynamic entry;
  entry["name"_key] = e.name;
  entry["type"_key] = e.is_dir ? std::string{"dir"} : std::string{"file"};
  entries[i++] = entry;
}
dlg["dlg"].set({{"files"_key, entries}, {"title"_key, std::string{"Open File"}}}).get();

dlg["dlg"].onEvent("on_navigate"_key, [&](bison::dynamic payload) {
  // User navigated into a directory — repopulate the file list.
  auto name = payload.as<std::string>("name"_key);
  // ... update entries for new directory ...
  dlg["dlg"].set({{"files"_key, new_entries}});
});

dlg["dlg"].onEvent("on_open"_key, [&](bison::dynamic payload) {
  auto selected = payload.as<std::string>("path"_key);
  // Use selected path (always relative to session sandbox unless
  // allow_absolute_paths is enabled).
});
```

---

## Sandbox and Security

The same rules that govern low-level wish elements apply to forms without exception:

- **Relative paths only by default.** A form that constructs or emits a file path must pass it through `file_service::resolve_path(name, sess().resource_dir, sess().allow_absolute_paths)`. If the result is empty, the path is rejected.
- **Client-provided data is untrusted.** Field values set by the client (file names, filter strings, dialog titles) are untrusted input. Forms must not use them to construct file paths without sandbox validation.
- **`OpenFileDialog` does not read the filesystem.** The dialog only displays what the client provides in the `files` field. The `on_navigate` event gives the client an entry name (relative, never an absolute path) and the client decides what to load. This design avoids any server-side directory traversal.
- **Forms that do access files** (e.g., a Notepad form in `WISH_MODULE_DESKTOP`) must call `file_service::resolve_path()` for every read and write, and must document this in the form's registration attributes (see `src/ui_elements/text_editor.cpp` for the reference pattern).
- **No cross-session leakage.** A form holds a `std::shared_ptr<session>` to its own session. It must not store or access any other session's state.

---

## Writing a New Form

1. **Create `src/forms/<name>.hpp` and `src/forms/<name>.cpp`.**
   - Inherit from `form` (which inherits from `bison::dynamic`).
   - Override `on_init()` to build the internal UI tree and register prototype fields/events.
   - Follow the RMI registration pattern in `template_handler.cpp`.

2. **Register the class.** Add a `register_<name>()` free-function declaration in the header and its definition in the `.cpp`. Call it from `registry.cpp` (or from the module's registration function).

3. **Inject context in `server::on_create_object`.** Add a `dynamic_cast<YourForm*>` branch alongside the existing `template_handler` branch.

4. **Declare the public API** (fields, methods, events) on the prototype in `register_<name>()`. Use `DisplayName` attributes so `build_display_dict()` resolves key hashes in logs.

5. **Write tests.** Use the `memory_transport` path (as in existing wish tests) to instantiate the form and verify field binding and event emission without a real window.

---

## Planned Forms

| Form | Module | Description |
|------|--------|-------------|
| `OpenFileDialog` | built-in | File picker (described above) |
| `SaveFileDialog` | built-in | Variant of OpenFileDialog with a "Save" confirm path |
| `Calculator` | `WISH_MODULE_DESKTOP` | Four-function calculator; demonstrates self-contained form logic |
| `Notepad` | `WISH_MODULE_DESKTOP` | Text editor backed by a file in the session sandbox |
| `ProcessMonitor` | `WISH_MODULE_DESKTOP` | Tabular view of running processes (read-only, OS-specific) |

New built-in forms go in `src/forms/`. Module-specific forms go in `src/forms/<module_name>/`.
