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
client: instantiate("wish", "FileDialog")
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
  src/forms/file_dialog.cpp   # always compiled: part of the base server
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
  register_file_dialog();           // always present

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

### `FileDialog`

**Bison class name:** `"FileDialog"` in the `"wish"` namespace.

**Purpose:** A modal file-picker dialog usable as both an Open File and a Save As dialog (controlled by the `confirm_label` field). The server manages all selection, filtering, and navigation logic. The client is responsible for providing the list of files available in the current directory and for acting on the selected path.

#### Fields

| Field | Type | Direction | Description |
|-------|------|-----------|-------------|
| `title` | string | client → form | Dialog window title. Default: `"Open File"`. |
| `files` | dynamic | client → form | Ordered list of entries. Each entry is a dynamic with `name` (string) and `type` (`"file"` or `"dir"`). Client repopulates this on every `on_navigate` event. |
| `filename` | string | client ↔ form | Current value of the filename input field. Client may preset it; form updates it as the user types or clicks. |
| `filters` | dynamic | client → form | Optional list of filter entries. Each entry is a dynamic with `label` (string, shown in the combo box) and an optional `regex` (string, applied case-insensitively to filenames; empty or absent means match all). If the list is empty, the filter row is hidden. |
| `confirm_label` | string | client → form | Label on the confirm button. Default: `"Open"`. Set to `"Save"` for a Save As dialog. |
| `path` | string | client ↔ form | Current directory path shown in the editable path bar. Client updates on `on_navigate`. User may also type a path and press Enter. |

#### Events

| Event | Payload fields | Description |
|-------|---------------|-------------|
| `on_open` | `path` (string) | User confirmed selection. `path` is the value of the filename field at the moment of confirmation. It is always relative unless `allow_absolute_paths` is enabled server-side. |
| `on_navigate` | `name` (string), `type` (string) | User navigated. For `type == "dir"` the client should update `files` for the new directory. For `type == "file"` the client may treat this as a confirm. For `type == "path"` the user typed a path directly into the path bar and pressed Enter; `name` contains the full typed string. |
| `on_cancel` | — | User dismissed the dialog without selecting a file. |

#### Internal UI structure (informative)

```
Window (title from field)
  VerticalLayout
    InputText      ← editable path bar; Enter emits on_navigate(type="path")
    Table          ← file list (rows from `files` field; single-click fills filename, dbl-click emits on_navigate)
    InputText      ← filename field (full width; two-way bound to the `filename` field)
    HorizontalLayout (if filters non-empty)
      Combo        ← filter list
    HorizontalLayout (right-aligned, spacing=8)
      Button       ← confirm_label  → emits on_open
      Button       ← "Cancel"       → emits on_cancel
```

The internal tree is private. Clients must not attempt to access its nodes by name.

#### Example (C++)

```cpp
auto dlg = c.instantiate("wish"_key, "FileDialog"_key).get();

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
  auto name = payload.as<std::string>("name"_key);
  auto type = payload.as<std::string>("type"_key);
  // type="dir"  — user double-clicked a directory row
  // type="path" — user typed a path in the path bar and pressed Enter
  // ... navigate to the new directory and update files ...
  dlg["dlg"].set({{"files"_key, new_entries}});
});

dlg["dlg"].onEvent("on_open"_key, [&](bison::dynamic payload) {
  auto selected = payload.as<std::string>("path"_key);
  // Use selected path (always relative to session sandbox unless
  // allow_absolute_paths is enabled).
});
```

### `Notepad`

**Bison class name:** `"Notepad"` in the `"wish"` namespace. Built-in (registered unconditionally in `register_all()`, alongside `FileDialog` and `Calculator` — not gated behind `WISH_MODULE_DESKTOP`; see the note at the end of [Planned Forms](#planned-forms)).

**Purpose:** A multi-file, syntax-highlighted text editor. Each open file is one closable tab (`TabItem`) containing a `TextEditor` — the existing `TextEditor` element already reads/writes the session sandbox directly and provides highlighting, so `Notepad` itself only manages tabs; it does not duplicate load/save logic.

Files edited by a form live in the session's sandboxed `resource_dir`, but a text editor's files conceptually belong to the *client* (its local disk). `Notepad` bridges the two by treating "open" and "close" as an explicit handshake with the client rather than doing any file listing itself:

- **Open** — the client must call `client::upload_file` to place the file's bytes in the sandbox *before* calling the `open_file` method with the resulting sandbox-relative path. `Notepad` cannot offer a client-side directory listing itself, so its own "Open" button emits `on_request_open`, asking the connected client to present its own picker (typically by driving a client-owned `FileDialog` instance populated from a local `directory_iterator` — see `app/wish_cli/client/apps/notepad.cpp` for the reference client).
- **Close** — closing a tab (or the whole window) emits `on_file_closed`; the client is expected to call `client::download_file` for that path and persist it locally before discarding its own bookkeeping.
- **Sync** — the "Sync" toolbar button emits `on_sync_requested` with every currently open path, asking the client to download and overwrite its local copies without closing anything.

#### Fields

| Field | Type | Direction | Description |
|-------|------|-----------|--------------|
| `title` | string | client → form | Window title. Default: `"Notepad"`. |

#### Methods

| Method | Params | Description |
|--------|--------|--------------|
| `open_file` | `path` (string, required), `title` (string, optional) | Registers an already-uploaded sandbox file as a new tab. `path` is validated with `file_service::resolve_path`; an invalid or sandbox-escaping path emits `on_error` instead. A `path` that is already open is a no-op. |

#### Events

| Event | Payload fields | Description |
|-------|---------------|--------------|
| `on_request_open` | — | The "Open" button was clicked. The client should present its own file picker and, once a file is chosen, `upload_file` it and call `open_file`. |
| `on_file_opened` | `path`, `title` | A new tab was created. |
| `on_file_closed` | `path` | A tab was closed (individually, or as part of the whole window closing). The client should `download_file(path)` and persist it locally. |
| `on_file_saved` | `path` | The user pressed Ctrl+S inside a tab's editor. The client should `download_file(path)` without closing the tab. |
| `on_sync_requested` | `paths` (list of string) | The "Sync" button was clicked. The client should `download_file` every listed path. |
| `on_error` | `message` | `open_file` was called with an invalid or sandbox-escaping path. |
| `closed` | — | The whole Notepad window was closed. `on_file_closed` fires for every file that was still open, before this event. |

#### Internal UI structure (informative)

```
Window (title from field)
  VerticalLayout
    HorizontalLayout
      Button "Open"  → emits on_request_open
      Button "Sync"  → emits on_sync_requested
    TabBar             ← one TabItem per open file, added/removed at runtime
      TabItem (closable) ← emits "closed" when its X is clicked
        TextEditor       ← file_path = sandbox-relative path; language inferred from extension
```

The internal tree is private, like `FileDialog`'s.

---

## Sandbox and Security

The same rules that govern low-level wish elements apply to forms without exception:

- **Relative paths only by default.** A form that constructs or emits a file path must pass it through `file_service::resolve_path(name, sess().resource_dir, sess().allow_absolute_paths)`. If the result is empty, the path is rejected.
- **Client-provided data is untrusted.** Field values set by the client (file names, filter strings, dialog titles) are untrusted input. Forms must not use them to construct file paths without sandbox validation.
- **`FileDialog` does not read the filesystem.** The dialog only displays what the client provides in the `files` field. The `on_navigate` event gives the client an entry name (relative, never an absolute path) and the client decides what to load. This design avoids any server-side directory traversal.
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
| `FileDialog` | built-in | File picker / Save As dialog (described above) |
| `Calculator` | built-in | Four-function calculator; demonstrates self-contained form logic |
| `Notepad` | built-in | Multi-file, syntax-highlighted text editor bridged to the client via upload_file/download_file (described above) |
| `ProcessMonitor` | `WISH_MODULE_DESKTOP` | Tabular view of running processes (read-only, OS-specific) |

New built-in forms go in `src/forms/`. Module-specific forms go in `src/forms/<module_name>/`.

> **Note:** `Calculator` and `Notepad` were originally planned to live under
> `src/forms/desktop/` behind `WISH_MODULE_DESKTOP` (see the CMake snippet in
> [Module System](#module-system)). In practice both were implemented
> directly in `src/forms/` as unconditional built-ins, matching `FileDialog`
> — `src/forms/desktop/` does not exist, so `WISH_MODULE_DESKTOP=ON` currently
> fails to link. Any future form that genuinely needs to be optional (e.g.
> `ProcessMonitor`, which is OS-specific) should use that module mechanism;
> forms with no such constraint should just go in `src/forms/` like these two.
