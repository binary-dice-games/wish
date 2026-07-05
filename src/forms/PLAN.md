# wish Forms — Implementation Plan

Each step produces a self-contained, reviewable deliverable. Steps are ordered so every dependency is in place before it is needed. Complete one step fully (code + tests passing) before moving to the next.

Read `DESIGN.md` in this directory before starting any step.

---

## Step 1 — CMake scaffolding

**Goal:** The build system is aware of the forms directory and the optional desktop module, with no implementation files yet.

**Deliverables:**
- Root `CMakeLists.txt` — append `src/forms/form.cpp` to the `wish` target sources. Add a new CMake option:
  ```cmake
  option(WISH_MODULE_DESKTOP "Include desktop utility forms (Calculator, Notepad, ...)" OFF)
  if(WISH_MODULE_DESKTOP)
    target_sources(wish PRIVATE
      src/forms/desktop/calculator.cpp
      src/forms/desktop/notepad.cpp
    )
    target_compile_definitions(wish PRIVATE WISH_MODULE_DESKTOP)
  endif()
  ```
- `src/registry.cpp` — add a forward declaration and call site for `register_open_file_dialog()` (unconditional) and `register_desktop_module()` (inside `#ifdef WISH_MODULE_DESKTOP`). Both functions do not exist yet; this step only adds the call site and guards so the registry structure is visible in review before any form is built.
- `src/forms/form.cpp` — empty translation unit with the MIT license header and a `namespace bdg::wish {}` block. Exists solely to satisfy the CMakeLists source entry added above.

**Tests:**
- `cmake -S . -B build && cmake --build build` succeeds with `WISH_MODULE_DESKTOP=OFF` (the `register_*` declarations are guarded or not linked yet — add them as `// TODO` comments if needed to keep the build clean).
- `cmake -S . -B build -DWISH_MODULE_DESKTOP=ON && cmake --build build` fails with a linker error about missing `calculator.cpp` / `notepad.cpp` — this is expected and documents that Step 9 must supply those files. Capture this expected failure in a build note, not a ctest test.

---

## Step 2 — `form` base class

**Goal:** The `form` base class is defined and its contract (context injection, event emission, internal root key) is testable in isolation.

**Deliverables:**
- `src/form.hpp` — declares:
  ```cpp
  namespace bdg::wish {

  /// @brief Base class for all wish form objects.
  ///
  /// A form is a bison RMI object (not a ui_element) that manages an internal
  /// ui_element tree. The server calls init() once after creation to supply the
  /// per-session context; subclasses override on_init() to build their UI.
  class form : public bison::dynamic {
   public:
    explicit form(bison::dynamic&& base);

    /// @brief Inject session context. Called once by server::on_create_object.
    void init(bison::rmi::context& ctx, std::shared_ptr<session> sess);

   protected:
    /// @brief Build the internal ui_element tree. ctx() and sess() are valid.
    virtual void on_init() = 0;

    /// @brief Emit a high-level event to the client.
    /// @param event_name  Hashed event key (e.g. "on_open"_key).
    /// @param payload     Optional event payload fields.
    void emit(bison::key_t event_name, bison::dynamic payload = {});

    bison::rmi::context& ctx() { return *ctx_; }
    session&             sess() { return *sess_; }

    /// @brief Key under which the internal Window is stored in session.objects.
    /// Set by on_init() implementations; used by ~form() for cleanup.
    std::string internal_root_key_;

   private:
    bison::rmi::context*     ctx_{nullptr};
    std::shared_ptr<session> sess_;
  };

  } // namespace bdg::wish
  ```
- `src/forms/form.cpp` — replaces the empty stub from Step 1:
  - `form::form(dynamic&& base)` — delegates to `dynamic(std::move(base))`.
  - `form::init(ctx, sess)` — stores `ctx` and `sess`, then calls `on_init()`.
  - `form::emit(event_name, payload)` — calls `ctx_->emit_event(id(), event_name, payload)`.
- `src/wish.hpp` — add `#include <form.hpp>`.

**Tests** (`tests/test_form_base.cpp`):
- A concrete `stub_form : public form` that overrides `on_init()` with `init_called_ = true` and stores `&ctx()` + `&sess()`.
- `init()` calls `on_init()` exactly once.
- After `init()`, `ctx()` returns the injected context reference.
- After `init()`, `sess()` returns the injected session reference.
- `emit()` forwards to `ctx.emit_event` with the correct object ID and event name (verified with a mock context that records calls).
- Constructing a `stub_form` from a `dynamic&&` base does not throw.

---

## Step 3 — Server context injection for forms

**Goal:** `server::on_create_object` automatically injects session context into any `form` subclass, using the same `dynamic_cast` pattern already used for `ui_template`.

**Deliverables:**
- `src/server.cpp` — in `on_create_object`, after the existing `ui_template` branch, add:
  ```cpp
  if (auto* f = dynamic_cast<form*>(obj.get())) {
    f->init(ctx, sess);
  }
  ```
  This single branch handles every current and future `form` subclass with no per-class changes to the server.
- `src/server.hpp` — add `#include <form.hpp>`.

**Tests** (`tests/test_form_base.cpp` — extend):
- Register `stub_form` as a bison class `"__StubForm"` in `"wish"`.
- Start a server with `memory_server_transport` and `null_renderer`.
- Connect a client; call `instantiate("wish", "__StubForm")`.
- Verify that `stub_form::on_init()` was called (i.e., `init_called_` is true and the session pointer is non-null).
- Verify a second `stub_form` instantiation produces a second independent instance (different session pointers would occur in a multi-session scenario; for this test, verify two instances each have `init_called_` set).

---

## Step 4 — `OpenFileDialog` class skeleton and field registration

**Goal:** `OpenFileDialog` exists as an instantiable bison class with all its prototype fields registered and their default values in place. No internal UI is built yet.

**Deliverables:**
- `src/forms/open_file_dialog.hpp` — declares:
  ```cpp
  class open_file_dialog : public form {
   public:
    explicit open_file_dialog(bison::dynamic&& base);

   protected:
    void on_init() override;
  };

  void register_open_file_dialog();
  ```
- `src/forms/open_file_dialog.cpp` — implements `register_open_file_dialog()`:
  - Registers `"OpenFileDialog"` in `"wish"` with parent `"__WishForm"` (the form base prototype, if one is needed) or directly without a shared prototype.
  - Prototype fields with `DisplayName` attributes:

    | Field key | Type | Default | Notes |
    |-----------|------|---------|-------|
    | `title` | string | `"Open File"` | Window title. |
    | `files` | dynamic | `{}` | Ordered list of `{name, type}` entries. |
    | `filename` | string | `""` | Current value of the filename input. |
    | `filters` | dynamic | `{}` | Optional filter strings. Empty = no filter combo. |
    | `confirm_label` | string | `"Open"` | Label on the confirm button. |

  - `open_file_dialog::open_file_dialog(dynamic&& base)` — delegates to `form(std::move(base))`.
  - `on_init()` — empty stub; will be filled in Step 5.
- `src/registry.cpp` — `#include "forms/open_file_dialog.hpp"` and call `register_open_file_dialog()` unconditionally in `register_all()`.
- Root `CMakeLists.txt` — add `src/forms/open_file_dialog.cpp` to `wish` sources.

**Tests** (`tests/test_open_file_dialog.cpp`):
- `instantiate("wish", "OpenFileDialog")` does not throw.
- The instantiated object has a `title` field with string type and value `"Open File"`.
- The instantiated object has a `files` field with dynamic type.
- The instantiated object has a `filename` field with string type and value `""`.
- The instantiated object has a `filters` field with dynamic type.
- The instantiated object has a `confirm_label` field with string type and value `"Open"`.
- `server::on_create_object` calls `form::init()` → `on_init()` (the stub) without throwing.

---

## Step 5 — `OpenFileDialog` internal Window construction

**Goal:** `on_init()` builds the full internal `ui_element` tree (Window → layout → Table → InputText → Buttons) and registers its root in `session.objects` so the renderer draws it.

**Deliverables:**
- `src/forms/open_file_dialog.cpp` — implement `on_init()`:
  1. Build the internal tree using `ui_importer::import_json` with a hardcoded descriptor string:
     ```
     Window (title from field, width=480, height=360)
       VerticalLayout
         Label           id="path_label"    text=""
         Table           id="file_table"    (headers: Name, Type; zero rows initially)
         HorizontalLayout
           Label                            text="File name:"
           InputText       id="filename_input"  value="" hint="filename"
         HorizontalLayout  (visible=false initially; shown when filters non-empty)
           Label                            text="Filter:"
           Combo           id="filter_combo"
         HorizontalLayout
           Button          id="btn_open"    label from confirm_label field
           Button          id="btn_cancel"  label="Cancel"
     ```
  2. Assign `internal_root_key_` a unique name derived from the form object's bison ID (e.g. `"__form_" + std::to_string(id().id)`).
  3. Store the root `ui_element_ptr` in `sess().objects[internal_root_key_]`.
  4. Store named descendants in `sess().objects` under `internal_root_key_ + "." + child_name` to follow the session name_map convention.
- Destructor or `on_destroy()` hook (if needed): remove `internal_root_key_` and all sub-keys from `sess().objects` when the form is destroyed. Consider whether the session lifetime already handles cleanup (it does on disconnect; but explicit dialog close needs cleanup too). Add a `close()` method that removes from `session.objects` and emits `on_cancel`.

**Tests** (`tests/test_open_file_dialog.cpp` — extend):
- After instantiation via in-memory server, `sess().objects` contains an entry whose key starts with `"__form_"`.
- The entry is a `Window` node (`__class == "Window"_key`).
- The Window has a `VerticalLayout` child.
- The tree contains a node named `"btn_open"` (reached via the name_map).
- The tree contains a node named `"btn_cancel"`.
- The tree contains a node named `"filename_input"`.
- The tree contains a node named `"file_table"`.

---

## Step 6 — `OpenFileDialog` file list synchronization

**Goal:** Setting the `files` field on the form proxy updates the internal Table widget rows on the next render frame. Single-clicking a file row populates the `filename` field.

**Deliverables:**
- `src/forms/open_file_dialog.cpp`:
  - Register a custom setter method for the `files` field on the `OpenFileDialog` prototype. When the setter fires, iterate the new `files` dynamic and rebuild the Table's row data (via the Table's own `rows` field or equivalent mechanism). Store a pointer to the internal Table `ui_element` so the setter can reach it without re-walking the session map.
  - Single-click handler on Table rows: the Table emits a `"row_selected"` event with the row index; the form's internal handler reads the corresponding entry from `files`, copies `name` into the `filename` field, and updates the `filename_input` InputText value field.

**Tests** (`tests/test_open_file_dialog.cpp` — extend):
- Set `files` to `{0: {name:"a.txt", type:"file"}, 1: {name:"docs", type:"dir"}}`.
- After the setter fires, the internal Table has 2 rows with the correct names.
- Simulate a `"row_selected"` event with index 0; verify the `filename` field on the form object becomes `"a.txt"`.
- Simulate a `"row_selected"` event with index 1; verify the `filename` field becomes `"docs"`.
- Setting `files` to an empty dynamic clears the Table rows.

---

## Step 7 — `OpenFileDialog` events

**Goal:** User interactions on the internal widget tree emit the three high-level events (`on_open`, `on_cancel`, `on_navigate`) to the client with the correct payloads.

**Deliverables:**
- `src/forms/open_file_dialog.cpp` — register internal event handlers in `on_init()`:
  - `btn_open` `"clicked"` → read the current `filename` field; call `emit("on_open"_key, {{"path"_key, filename_value}})`.
  - `btn_cancel` `"clicked"` → call `emit("on_cancel"_key, {})`, then remove internal root from `session.objects`.
  - `file_table` `"row_activated"` (double-click) → read the corresponding entry from `files`; if `type == "dir"`, call `emit("on_navigate"_key, {{"name"_key, name}, {"type"_key, "dir"}})`. If `type == "file"`, treat as confirm: call `emit("on_open"_key, {{"path"_key, name}})`.
  - Path in `on_open` payload must be validated against `file_service::resolve_path(name, sess().resource_dir, sess().allow_absolute_paths)`. If validation fails, do not emit and optionally set an error state on the form.

**Tests** (`tests/test_open_file_dialog.cpp` — extend; use a mock context that records emitted events):
- Simulate `btn_open` click with `filename = "report.pdf"` → `on_open` event fires with `path == "report.pdf"`.
- Simulate `btn_cancel` click → `on_cancel` event fires; internal Window removed from `session.objects`.
- Simulate double-click on a `"dir"` row → `on_navigate` fires with correct `name` and `type == "dir"`.
- Simulate double-click on a `"file"` row → `on_open` fires with `path` equal to the file's `name`.
- Path with `..` component in `filename` (set by client) does not emit `on_open`; no crash.
- Absolute path in `filename` is rejected when `allow_absolute_paths == false`.

---

## Step 8 — `OpenFileDialog` `filename` two-way binding and `filters` Combo

**Goal:** Keyboard input in the filename InputText updates the `filename` field in real time. The filters Combo becomes visible and functional when the `filters` field is non-empty.

**Deliverables:**
- `src/forms/open_file_dialog.cpp`:
  - `filename_input` `"changed"` event handler: copy the new string value into the form's `filename` field so that a subsequent `proxy.get()` reflects what the user typed. This is a one-line handler: `as<std::string>(event, "value"_key)` → store into `"filename"_key` field.
  - `filters` setter: when the client sets `filters`, populate the `filter_combo` Combo's items and set `filter_combo` visibility to `true`. When `filters` is set to an empty dynamic, hide the Combo (`visible = false`).
  - `filter_combo` `"changed"` event handler: store the selected filter index in an internal field (no client-visible event required for this step; future extension can add `on_filter_changed`).

**Tests** (`tests/test_open_file_dialog.cpp` — extend):
- Simulate `filename_input` `"changed"` event with `value = "foo.txt"`; verify form's `filename` field is `"foo.txt"`.
- Set `filters` to `{0: "*.txt", 1: "*.md"}`; verify `filter_combo` `visible` field is `true` and the Combo's item list matches.
- Set `filters` to `{}`; verify `filter_combo` `visible` field is `false`.
- `confirm_label` field setter: changing `confirm_label` after construction updates `btn_open`'s `label` field.

---

## Step 9 — Desktop module CMake scaffolding

**Goal:** The `WISH_MODULE_DESKTOP` CMake option is fully wired: building with it ON compiles the desktop source files, building with it OFF excludes them entirely with no link errors.

**Deliverables:**
- `src/forms/desktop/calculator.cpp` — empty translation unit with MIT header, `namespace bdg::wish {}`, and a stub `void register_calculator() {}`.
- `src/forms/desktop/notepad.cpp` — empty translation unit with the same stub `void register_notepad() {}`.
- `src/forms/desktop/desktop_module.hpp` — declares `void register_desktop_module()`.
- `src/forms/desktop/desktop_module.cpp` — implements `register_desktop_module()` by calling `register_calculator()` and `register_notepad()`. Add this file to the `if(WISH_MODULE_DESKTOP)` sources block in CMakeLists.
- `src/registry.cpp`:
  ```cpp
  #ifdef WISH_MODULE_DESKTOP
  #include "forms/desktop/desktop_module.hpp"
  #endif
  // ... inside register_all():
  #ifdef WISH_MODULE_DESKTOP
    register_desktop_module();
  #endif
  ```

**Tests:**
- `cmake -S . -B build && cmake --build build` passes with `WISH_MODULE_DESKTOP=OFF` (default).
- `cmake -S . -B build -DWISH_MODULE_DESKTOP=ON && cmake --build build` passes (stubs are valid objects).
- `ctest --test-dir build` passes in both configurations; no new tests required for the stub module.

---

## Step 10 — Calculator form (desktop module)

> **Superseded:** implemented directly in `src/forms/calculator.hpp/.cpp` as
> an unconditional built-in (not under `src/forms/desktop/` /
> `WISH_MODULE_DESKTOP`), matching `FileDialog`. Fields/events differ
> slightly from the spec below — see the source and `DESIGN.md`'s
> "Planned Forms" note.

**Goal:** A self-contained four-function calculator form that maintains all arithmetic state on the server. The client needs no calculator logic — only event listeners and a connection.

**Deliverables:**
- `src/forms/desktop/calculator.hpp` — declares `class calculator : public form` and `void register_calculator()`.
- `src/forms/desktop/calculator.cpp` — implement:
  - Prototype fields: `expression` (string, default `"0"`) — the current display value.
  - Prototype events: `on_result` — fired after `=` with payload `{{"value"_key, result_string}}`.
  - Internal UI: `Window (title="Calculator", 280×380) → VerticalLayout [Label (display), five HorizontalLayout rows of Buttons]`. Button layout matches the calculator example in the root `PLAN.md` Step 16.
  - Internal state (C++ member fields): `std::string display_`, `double operand_`, `char op_`, `bool fresh_`.
  - Button click handlers update state and push `display_` into the form's `expression` field and into the Label's `text` field.
  - `=` triggers result computation, emits `on_result`, updates display.
  - `C` resets all state to initial values.
- `register_calculator()` — registers the prototype and adds `calculator` to the bison class map.

**Tests** (`tests/test_desktop_calculator.cpp`; only compiled when `WISH_MODULE_DESKTOP=ON`):
- `instantiate("wish", "Calculator")` succeeds and `expression` defaults to `"0"`.
- Simulate pressing `3`, `+`, `4`, `=`; verify `on_result` fires with `value == "7"`.
- Simulate pressing `C`; verify `expression` resets to `"0"`.
- Simulate pressing `1`, `0`, `÷` (or `/`), `2`, `=`; verify result is `"5"`.
- Division by zero: pressing `5`, `/`, `0`, `=` does not crash; `expression` shows an error string (e.g., `"Error"`).

---

## Step 11 — Notepad form (desktop module)

> **Superseded:** implemented directly in `src/forms/notepad.hpp/.cpp` as an
> unconditional built-in (not under `src/forms/desktop/` /
> `WISH_MODULE_DESKTOP`). The design below predates the `TextEditor` element
> (`src/ui_elements/text_editor.cpp`), which already reads/writes the
> sandbox file and handles load/save itself — so the shipped `Notepad` has
> no `load()`/`save()` methods. Instead it manages multiple tabs (one
> `TextEditor` each) and bridges the sandbox to the client's real filesystem
> via `client::upload_file`/`client::download_file`, since a text editor's
> files conceptually belong to the client, not the server. See `DESIGN.md`'s
> "Notepad" subsection under "Built-in Forms" for the actual fields,
> methods, and events, and `app/wish_cli/client/apps/notepad.cpp` for the
> reference client integration.

**Goal (original, superseded):** A single-file text editor whose content is persisted to the session sandbox. The form exposes a high-level API for loading and saving; all file I/O goes through `file_service::resolve_path()`.

**Deliverables:**
- `src/forms/desktop/notepad.hpp` — declares `class notepad : public form` and `void register_notepad()`.
- `src/forms/desktop/notepad.cpp` — implement:
  - Prototype fields:
    - `file_path` (string, default `""`) — relative path within the session sandbox.
    - `title` (string, default `"Notepad"`) — Window title.
  - Prototype methods:
    - `load()` — reads `file_path` from sandbox via `file_service::resolve_path`; populates the internal `TextEditor` content; emits `on_loaded` with `{{"path"_key, file_path}}`.
    - `save()` — writes the TextEditor's current content to `file_path` in the sandbox; emits `on_saved` with `{{"path"_key, file_path}}`.
  - Prototype events: `on_loaded`, `on_saved`, `on_error` (payload: `{{"message"_key, error_string}}`).
  - Internal UI: `Window → VerticalLayout [HorizontalLayout [Button "Load", Button "Save"], TextEditor]`.
  - `file_path` setter: store value; do not load automatically.
  - File I/O security: all paths go through `file_service::resolve_path(path, sess().resource_dir, sess().allow_absolute_paths)`. If the result is empty, emit `on_error` and return without accessing the filesystem.
- `register_notepad()` — registers the prototype.

**Tests** (`tests/test_desktop_notepad.cpp`; only compiled when `WISH_MODULE_DESKTOP=ON`):
- `instantiate("wish", "Notepad")` succeeds; `file_path` defaults to `""`.
- Set `file_path = "notes.txt"`, call `load()` when the file does not exist: `on_error` fires; no crash.
- Upload `"notes.txt"` to the session via `file_service`; set `file_path = "notes.txt"`, call `load()`; `on_loaded` fires; TextEditor content matches the uploaded text.
- Edit TextEditor content, call `save()`; `on_saved` fires; `file_service::download("notes.txt")` returns the new content.
- Set `file_path = "../escape.txt"`, call `load()`: `on_error` fires; no file is accessed outside the sandbox.
- Set `file_path = "/etc/passwd"`, call `load()` with `allow_absolute_paths = false`: `on_error` fires.

---

## Completion Criteria

The forms feature is complete when:

1. `cmake -S . -B build && cmake --build build` succeeds with `WISH_MODULE_DESKTOP=OFF`.
2. `cmake -S . -B build -DWISH_MODULE_DESKTOP=ON && cmake --build build` succeeds.
3. `ctest --test-dir build` passes all tests in both configurations.
4. `OpenFileDialog` can be instantiated, populated with a file list, and emits the three events (`on_open`, `on_cancel`, `on_navigate`) correctly.
5. Path traversal attacks (`../`, absolute paths) are rejected by `OpenFileDialog` and `Notepad` without crashing the server.
6. `Calculator` form performs correct four-function arithmetic and emits `on_result`.
7. `Notepad` form loads and saves files within the session sandbox and emits the correct lifecycle events.
8. All public form headers have Doxygen `@brief` comments.
9. `clang-format` reports no diff against the committed sources.
