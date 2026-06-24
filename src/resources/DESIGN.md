# wish Embedded Resources — Architecture & Design

## Overview

The wish server is designed to ship as a single, self-contained binary. Some UI elements require binary assets that would normally live on disk — file-type icons used by `OpenFileDialog`, TrueType fonts for text rendering, cursor images, and so on. Requiring these to exist as loose files alongside the binary would break the single-binary guarantee and complicate deployment.

The embedded resource system solves this by compiling those assets directly into the `wish` binary as static C++ arrays, generated during the CMake build from a source asset folder. At runtime, the server resolves `res://`-prefixed paths against the in-memory resource store rather than the filesystem, with no external files required.

---

## Design Goals

1. **No runtime file I/O for built-in assets.** Every built-in icon, font, and texture is available in any deployment, including those where the working directory or the server's own directory is inaccessible.
2. **Transparent URI scheme.** Callers (renderers, widget field setters) use a `res://` prefix to request an embedded resource. Paths without a prefix — or with `file://` — go through the existing `resolve_widget_path()` / `file_service::resolve_path()` chain unchanged.
3. **Read-only.** The resource store is a static, immutable view. No runtime addition, removal, or modification of embedded resources.
4. **Zero maintenance overhead.** Adding or removing an asset from the source folder automatically updates the binary on the next build — no manual bookkeeping in C++ source required.
5. **Minimal API surface.** The public API is a single `resource_store` header with two functions: `find` and `is_resource_path`.

---

## Architecture

```
Build time
  resources/                     ← source asset tree (committed to repo)
    fonts/
      default.ttf
    icons/
      file.png
      folder.png
      audio.png
  cmake/embed_resources.cmake    ← CMake custom command / Python script
      ↓ (runs before wish target compiles)
  src/resources/embedded_resources.cpp   ← generated (not committed)
      static const unsigned char res_fonts_default_ttf[] = { 0x00, ... };
      static const resource_entry g_resource_table[] = { ... };

Runtime
  resource_store::find("fonts/default.ttf")
    → scans g_resource_table → returns resource_view{data, size}

  Widget renderer resolving "res://icons/folder.png"
    → resource_store::is_resource_path() → true
    → resource_store::find("icons/folder.png") → resource_view
    → passes raw bytes to imgui texture loader
```

---

## Source Asset Layout

Built-in assets live in `resources/` at the repository root, **not** inside `src/`. This separates art assets from generated and hand-written C++ source.

```
wish/
  resources/          ← committed art assets (source of truth for embedded resources)
    fonts/
    icons/
    cursors/
  src/resources/      ← C++ code for the resource_store API + generated file
    resource_store.hpp
    resource_store.cpp
    embedded_resources.cpp   ← generated; excluded from version control
```

The CMake script accepts the source asset folder as a configurable variable (`WISH_RESOURCE_DIR`, default: `${CMAKE_SOURCE_DIR}/resources`). This allows downstream projects that embed wish as a library to substitute their own asset tree.

---

## CMake Code Generation Step

A small Python script (`cmake/embed_resources.py`) is invoked as a CMake `add_custom_command` with `OUTPUT` set to `src/resources/embedded_resources.cpp`. CMake re-runs the script whenever any file under `WISH_RESOURCE_DIR` changes.

The script:

1. Walks `WISH_RESOURCE_DIR` recursively, collecting every file.
2. Sorts paths for deterministic output.
3. For each file, writes a `static const unsigned char` array named after the file's path with non-alphanumeric characters replaced by `_`. These arrays are `static` (internal linkage) because they are only referenced within this translation unit:
   ```cpp
   // icons/folder.png  →  res_icons_folder_png
   static const unsigned char res_icons_folder_png[] = {
     0x89, 0x50, 0x4e, 0x47, ...
   };
   ```
4. Emits a `resource_entry` table and count **without** `static`, giving them external linkage so that `resource_store.cpp` can reference them directly across translation units:
   ```cpp
   // No static — external linkage required so resource_store.cpp can form a
   // live reference and the linker cannot dead-strip the table.
   const resource_entry g_resource_table[] = {
     { "fonts/default.ttf",  res_fonts_default_ttf,  sizeof(res_fonts_default_ttf)  },
     { "icons/audio.png",    res_icons_audio_png,    sizeof(res_icons_audio_png)    },
     { "icons/file.png",     res_icons_file_png,     sizeof(res_icons_file_png)     },
     { "icons/folder.png",   res_icons_folder_png,   sizeof(res_icons_folder_png)   },
   };
   const std::size_t g_resource_count = sizeof(g_resource_table) / sizeof(g_resource_table[0]);
   ```
5. `resource_store.cpp` declares these symbols with `extern` and references them in every public lookup function.
6. **Compares before writing.** The script builds the full output in memory, then reads the existing `embedded_resources.cpp` (if it exists). If the content is identical it exits without touching the file:
   ```python
   new_content = generate_cpp(resource_dir)
   out_path = Path(output_file)
   if out_path.exists() and out_path.read_text(encoding="utf-8") == new_content:
       sys.exit(0)   # file unchanged — preserve mtime, skip recompile
   out_path.write_text(new_content, encoding="utf-8")
   ```
   Because CMake and Ninja use file modification timestamps to decide whether to recompile, leaving the file untouched when content has not changed prevents the C++ compiler from processing the file unnecessarily — which matters because the generated file can be large.

The generated file has the MIT license header and a `// GENERATED — do not edit` banner at the top. It is listed in `.gitignore`.

If `WISH_RESOURCE_DIR` is empty or does not exist, the script emits an empty table and logs a CMake warning; the build still succeeds (useful for library-only builds that provide no built-in assets).

### CMake dependency tracking

`add_custom_command` lists every file under `WISH_RESOURCE_DIR` as a dependency so the script is re-run whenever an asset is modified. `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` extends this to also re-run the CMake configure step when assets are added or removed, keeping the dependency list accurate:

```cmake
file(GLOB_RECURSE WISH_RESOURCE_FILES
  CONFIGURE_DEPENDS
  "${WISH_RESOURCE_DIR}/*"
)

add_custom_command(
  OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/embedded_resources.cpp"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/cmake/embed_resources.py"
            --input  "${WISH_RESOURCE_DIR}"
            --output "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/embedded_resources.cpp"
  DEPENDS ${WISH_RESOURCE_FILES}
  COMMENT "Embedding resources"
)
```

The two mechanisms complement each other:

| Scenario | Outcome |
|----------|---------|
| No asset changed | `DEPENDS` timestamps unchanged → script not invoked → no recompile |
| Asset content changed | Script invoked → content differs → file written → recompile |
| Asset added or removed | `CONFIGURE_DEPENDS` re-runs CMake → updated `DEPENDS` list → script invoked → compare-before-write decides |
| CMake reconfigure only | Script may be invoked → compare-before-write leaves file unchanged → no recompile |

---

## Preventing Dead-Code Elimination

Modern linkers remove unreferenced symbols by default (`--gc-sections` on GCC/Clang, `/OPT:REF` on MSVC). Because `embedded_resources.cpp` is a generated translation unit with no callers inside its own file, the linker could in principle strip its contents entirely.

The design avoids this without compiler-specific attributes by ensuring a **live reference chain**:

```
renderer calls resource_store::find()          ← used, cannot be stripped
  → find() references g_resource_table         ← external linkage, referenced by find()
    → table entries hold addresses of byte     ← static arrays, referenced by table
         arrays (res_icons_folder_png, ...)
```

`resource_store.cpp` holds the `extern` declarations that anchor the chain:

```cpp
// resource_store.cpp
extern const resource_entry g_resource_table[];
extern const std::size_t    g_resource_count;

std::optional<resource_view> resource_store::find(std::string_view path) {
  for (std::size_t i = 0; i < g_resource_count; ++i) {
    if (path == g_resource_table[i].path)
      return resource_view{g_resource_table[i].data, g_resource_table[i].size};
  }
  return std::nullopt;
}
```

Because `find` is reachable (called by the renderer), the linker must keep it. Keeping `find` requires keeping `g_resource_table` and `g_resource_count` (external linkage, referenced directly). Keeping the table requires keeping every byte array whose address is stored in a table entry (referenced by the table initialiser within the same TU).

The individual byte arrays are `static` (internal linkage) — they cannot be referenced from outside `embedded_resources.cpp`. This is intentional: it prevents symbol-table bloat from hundreds of generated names while still keeping them alive through the table.

No `[[gnu::used]]`, `__declspec(selectany)`, or linker scripts are required.

---

## Public API

```cpp
// src/resources/resource_store.hpp

namespace bdg::wish {

/// @brief Immutable view over a single embedded resource.
struct resource_view {
  const unsigned char* data;  ///< Pointer into static storage. Never null if valid.
  std::size_t          size;  ///< Byte length.
};

/// @brief Read-only store of binary resources compiled into the wish binary.
///
/// All methods are free functions (no instantiation needed). The backing table
/// is generated at build time from the asset tree in WISH_RESOURCE_DIR.
namespace resource_store {

  /// @brief Return true if @p path starts with the "res://" scheme.
  bool is_resource_path(std::string_view path);

  /// @brief Strip the "res://" prefix from @p path.
  /// @return The bare path (e.g. "icons/folder.png"), or @p path unchanged if
  ///         it does not start with "res://".
  std::string_view strip_scheme(std::string_view path);

  /// @brief Look up an embedded resource by its bare path (no "res://" prefix).
  /// @param path  Case-sensitive path relative to the asset root
  ///              (e.g. "icons/folder.png").
  /// @return A populated resource_view, or std::nullopt if not found.
  std::optional<resource_view> find(std::string_view path);

} // namespace resource_store

} // namespace bdg::wish
```

`is_resource_path` and `strip_scheme` are pure string operations with no table access; they are safe to call from any thread at any time. `find` performs a linear scan of the statically initialised table; the table is immutable after process start, so `find` is also thread-safe without locking.

---

## Integration with Widget Rendering

Widget fields that accept file paths (e.g. `Image::src`, font path fields) currently go through `resolve_widget_path()` (defined in `src/imgui/imgui_ui_renderer.hpp`), which enforces the session sandbox for filesystem access.

`res://` paths bypass the sandbox check entirely — they reference data already compiled into the binary, not the filesystem. The renderer checks `resource_store::is_resource_path()` **before** calling `resolve_widget_path()`:

```
renderer receives path string
  ├─ starts with "res://"?
  │    yes → resource_store::find(strip_scheme(path))
  │           → load texture / font from raw bytes in memory
  │           → return; no filesystem access
  └─ no  → resolve_widget_path(path, resource_dir, allow_absolute_paths)
             → existing sandbox-validated filesystem load
```

This precedence rule means `res://` paths are unaffected by `allow_absolute_paths` or the per-session `resource_dir` — they are always available and never sandboxed.

### Texture loading from memory

ImGui's `ImGui::GetIO().Fonts->AddFontFromMemoryTTF` and the SDL3/OpenGL texture-from-memory path accept a raw byte buffer. `resource_view.data` and `resource_view.size` are passed directly to these APIs. The caller must not free or modify the buffer.

---

## Security Considerations

- **No path traversal risk.** The resource table keys are normalised at build time by the generator script and are never influenced by client input. A `res://` path from a client is matched against this fixed table; if no match exists, `find` returns `nullopt` and the caller falls back to a default or renders nothing.
- **No writable surface.** `resource_view` exposes a `const unsigned char*`. Nothing in the public API allows writing to the backing arrays.
- **No filesystem access.** `resource_store` never opens files at runtime. The risk surface is limited to the generator script at build time, which only reads files from `WISH_RESOURCE_DIR`.

---

## What Belongs in `resources/`

Only binary assets that are needed by the server's built-in UI at runtime belong in the committed `resources/` tree:

| Category | Examples |
|----------|---------|
| Fonts | Default UI font (.ttf), monospace font for text editor |
| Icons | File-type icons (folder, audio, image, document, code, ...) for `OpenFileDialog` |
| Cursors | Custom cursor images, if not provided by the OS |

**Do not add:**
- Test data or fixtures (use the `tests/` tree).
- Documentation images (use `docs/` or `resources/` in the repo root for README assets).
- Assets for optional modules unless the module is compiled unconditionally. Module-specific assets should be gated with the same CMake option as their module's source.

---

## Relationship to `file_service`

`file_service` manages per-session, client-uploaded files in a temporary directory that is deleted on disconnect. It is the right mechanism for user-provided images, fonts, or data that the client uploads at runtime.

`resource_store` manages immutable, server-provided assets compiled into the binary. It is the right mechanism for icons, default fonts, and other assets that the server always needs regardless of what clients connect.

The two systems are intentionally separate. A path starting with `res://` always resolves through `resource_store`; all other paths resolve through `file_service::resolve_path()`.

---

## Planned Built-in Assets

| Path | Purpose |
|------|---------|
| `fonts/default.ttf` | Primary UI font for imgui rendering |
| `fonts/mono.ttf` | Monospace font for the TextEditor element |
| `icons/file.png` | Generic file icon for `OpenFileDialog` |
| `icons/folder.png` | Directory icon for `OpenFileDialog` |
| `icons/audio.png` | Audio file icon |
| `icons/image.png` | Image file icon |
| `icons/code.png` | Source/code file icon |
| `icons/document.png` | Document/text file icon |
