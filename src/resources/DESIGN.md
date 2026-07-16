# wish Embedded Resources — Architecture & Design

## Overview

The wish server is designed to ship as a single, self-contained binary. Some UI elements require binary assets that would normally live on disk — file-type icons used by `OpenFileDialog`, TrueType fonts for text rendering, cursor images, and so on. Requiring these to exist as loose files alongside the binary would break the single-binary guarantee and complicate deployment.

The embedded resource system solves this by compiling those assets into the `wish` binary as a single compressed zip archive (one static C++ byte array), generated during the CMake build from a source asset folder. At runtime, each session unpacks that archive into its own sandboxed `resource_dir/res/` subdirectory when the session is constructed. From that point on, built-in assets are ordinary files on disk, resolved through the exact same `file_service` path-resolution and sandboxing that already handles client-uploaded files — there is no separate URI scheme or in-memory lookup path to keep in sync with the renderer.

---

## Design Goals

1. **No build-time bloat from per-file arrays.** All assets are packed into a single zip archive and embedded as one byte array, rather than one `static const unsigned char[]` per file plus a lookup table.
2. **Assets behave like regular files at runtime.** Once unpacked into a session's `resource_dir/res/`, built-in icons and fonts are indistinguishable from client-uploaded files as far as any renderer or widget code is concerned — both resolve through `file_service::resolve_path()`.
3. **Read-only.** Extracted files are chmod'd read-only (owner/group/other read, no write) so `file_service::upload()` cannot silently overwrite a built-in asset. See "Security Considerations" below for the one accepted limitation to this.
4. **Zero maintenance overhead.** Adding or removing an asset from the source folder automatically updates the binary on the next build — no manual bookkeeping in C++ source required.
5. **Minimal API surface.** The public API is a single `resource_store` header with one function: `extract_to`.

---

## Architecture

```
Build time
  resources/embedded/               ← source asset tree (committed to repo)
    fonts/
      default.ttf
      mono.ttf
    icons/
      file.png
      folder.png
      audio.png
      image.png
      code.png
      document.png
      ↓ cmake -E tar --format=zip (add_custom_command)
  build/wish_embedded_resources.zip ← intermediate build artifact (not committed)
      ↓ cmake/GenerateResource.cmake (add_custom_command)
  src/resources/embedded_resources.cpp   ← generated (not committed)
      extern const unsigned char g_resource_archive_data[] = { 0x50, 0x4b, ... };
      extern const std::size_t   g_resource_archive_size = sizeof(...);

Runtime
  session::session(id)
    → std::filesystem::create_directories(resource_dir)
    → resource_store::extract_to(resource_dir / "res")
        → mz_zip_reader_init_mem(g_resource_archive_data, g_resource_archive_size)
        → for each entry: extract to resource_dir/res/<entry path>, chmod read-only

  Widget renderer resolving Image::src = "icons/folder.png"
    → file_service::resolve_path("res/icons/folder.png", resource_dir, ...)
        (same sandbox check used for every other session file)
    → ordinary file read; no special case anywhere in renderer code
```

---

## Source Asset Layout

Built-in assets live in `resources/embedded/` at the repository root, **not** inside `src/`. The `embedded/` subdirectory makes it explicit which assets are compiled into the binary; applications using the wish server will typically use `resources/` for their own runtime assets that are not embedded.

```
wish/
  resources/
    embedded/         ← assets compiled into the binary (source of truth for resource_store)
      fonts/
      icons/
  src/resources/      ← C++ code for the resource_store API + generated file
    resource_store.hpp
    resource_store.cpp
    embedded_resources.cpp   ← generated; excluded from version control
```

The CMake logic accepts the source asset folder as a configurable variable (`WISH_RESOURCE_DIR`, default: `${CMAKE_SOURCE_DIR}/resources/embedded`). This allows downstream projects that embed wish as a library to substitute their own asset tree.

---

## CMake Build Steps

Two chained `add_custom_command`s in the root `CMakeLists.txt` produce `src/resources/embedded_resources.cpp`:

1. **Zip the asset tree.** `${CMAKE_COMMAND} -E tar cf <zip> --format=zip -- <relative asset names>`, run with `WORKING_DIRECTORY` set to `WISH_RESOURCE_DIR`, so zip entry names are the same relative paths used at runtime (`icons/folder.png`, `fonts/default.ttf`, ...). This is a genuine build-time `COMMAND` (not a configure-time `file(ARCHIVE_CREATE)` call), so it participates correctly in Ninja/Make `OUTPUT`/`DEPENDS` incremental tracking — it only reruns when a listed asset file actually changes. `cmake -E tar --format=zip` ships with CMake itself (bundled libarchive); no new build-time dependency is introduced by this step.
2. **Embed the zip as a byte array.** `cmake/GenerateResource.cmake` reads the zip file's bytes via `file(READ ... HEX)`, converts them to `0x..` tokens, and writes `src/resources/embedded_resources.cpp` from the `embedded_resources.cpp.in` template via `configure_file()`.

```cmake
# cmake/GenerateResource.cmake
file(READ "${WISH_EMBEDDED_ZIP}" hex_content HEX)
string(REGEX REPLACE "(..)" "0x\\1, " CPP_CONTENT "${hex_content}")
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)
```

The generated file has the MIT license header and a `// GENERATED — do not edit` banner at the top. It is listed in `.gitignore`.

If `WISH_RESOURCE_DIR` is empty or does not exist, the build emits a zero-length array directly at configure time instead of running either custom command, so the build still succeeds (useful for library-only builds that provide no built-in assets).

### CMake dependency tracking

`file(GLOB_RECURSE ALL_ASSETS CONFIGURE_DEPENDS ...)` re-runs the CMake configure step whenever a file is added or removed under `WISH_RESOURCE_DIR`, keeping the `DEPENDS` list of the zip-creation command accurate. Combined with the two `add_custom_command`s' own `DEPENDS`, this means:

| Scenario | Outcome |
|----------|---------|
| No asset changed | Custom commands not invoked → no rebuild |
| Asset content changed | Zip step reruns → codegen step reruns → recompile |
| Asset added or removed | `CONFIGURE_DEPENDS` re-runs CMake → updated `DEPENDS` list → zip step reruns |

---

## Public API

```cpp
// src/resources/resource_store.hpp

namespace bdg::wish::resource_store {

/// @brief Unpack the embedded resource archive into @p dir.
///
/// Creates @p dir if it does not already exist. Every regular-file entry
/// becomes a file under @p dir (creating subdirectories as needed), then has
/// its permissions set to read-only (owner/group/other read, no write), so
/// `file_service::upload()` cannot silently overwrite a built-in asset.
///
/// Never throws. Any miniz or filesystem failure is logged to stderr and
/// that entry is skipped; extraction continues with the remaining entries.
///
/// @param out_crc32  When non-null, populated with one entry per
///                    successfully-extracted file: its archive-relative path
///                    mapped to the zip's own per-file CRC-32.
/// @return true if every entry was extracted successfully.
bool extract_to(const std::filesystem::path& dir, std::unordered_map<std::string, uint32_t>* out_crc32 = nullptr);

} // namespace bdg::wish::resource_store
```

Unpacking is implemented with [miniz](https://github.com/richgel999/miniz), vendored via `FetchContent` (see root `CMakeLists.txt`), the same mechanism already used for imgui/SDL3.

**CRC-32 exposure (browser resource cache).** `mz_zip_reader_file_stat` already computes a per-file CRC-32 while `extract_to()` walks the archive; the optional `out_crc32` out-param simply surfaces a field that was previously read and discarded. `context::context()` collects it into `context::embedded_crc32s` (re-keyed with a `"res/"` prefix) when extracting embedded assets into a session's `resource_dir/res/`. `web_renderer::get_or_load_texture()` (see `src/web/DESIGN.md`'s "Persistent Browser Resource Cache" section) consumes this map to version its browser-side texture cache without re-hashing bytes miniz already checksummed. Session-**uploaded** files never appear in the embedded zip and never carry a client-supplied checksum (`file_service::upload()`'s RMI payload is raw `{name, data}`), so they fall back to an on-the-fly `mz_crc32` computation at the same call site, using the same primitive for one consistent versioning scheme regardless of a resource's origin. This is the only consumer of `out_crc32` — `resource_store`'s own extraction logic and its `bool` success contract are otherwise unchanged.

---

## Runtime Extraction and Failure Handling

`extract_to()` is called once, from `session::session()`, immediately after `resource_dir` itself is created:

```cpp
session::session(bison::key_t id_) : id(id_) {
  resource_dir = std::filesystem::temp_directory_path() / ("wish_" + std::to_string(...));
  std::filesystem::create_directories(resource_dir);
  resource_store::extract_to(resource_dir / "res");
}
```

`session::session()` runs on the per-connection worker thread with **no surrounding try/catch** — an exception escaping it would call `std::terminate()` and kill the entire server process, not just the one connection. For this reason `extract_to()` never throws: every failure mode (archive corruption, a per-entry extract/chmod failure, an unwritable destination) is logged to stderr and folded into a `false` return, which the caller intentionally ignores. A session whose built-in icons/fonts failed to extract is degraded, not broken — this matches the renderer's existing fail-soft philosophy for missing assets (`render_image()` silently no-ops when a texture fails to load), as opposed to `file_service`'s fail-loud `std::runtime_error`s, which are safe only because RMI method dispatch runs inside the server's own per-message catch blocks — a fundamentally different call context than session construction.

No explicit cleanup step is needed: `session::~session()` already unconditionally `remove_all(resource_dir, ec)`s the whole tree (including `res/`) on disconnect. Extracted files are read-only but the directories containing them are left normally writable, so POSIX unlink (governed by the containing directory's permissions, not the file's own mode) removes them without any special-casing — this holds whether extraction fully succeeded or partially failed.

---

## Security Considerations

- **No path traversal risk from the archive itself.** Zip entry names come from the build-controlled asset tree, not from any client input — there is nothing to sandbox-check on the way in.
- **Read-only against overwrite, not against deletion.** Extracted files are chmod'd read-only, so `file_service::upload("res/icons/folder.png", ...)` fails (the underlying `std::ofstream` cannot open a read-only file for writing). `file_service::erase("res/icons/folder.png")` can still succeed, because POSIX unlink permission is governed by the containing directory (left writable), not the file's own mode. This is an accepted, deliberate trade-off: it only affects the *current session's* private, temp-directory copy (never the embedded archive itself), and is cleaned up unconditionally on disconnect regardless.
- **No collisions with uploads.** Extraction targets `resource_dir/res/`, a name reserved by convention, not `resource_dir` itself — so a client's uploaded file names (via `file_service::upload()`, which writes directly under `resource_dir`) can never collide with an embedded asset's path.

---

## What Belongs in `resources/embedded/`

Only binary assets that are needed by the server's built-in UI at runtime belong in `resources/embedded/`:

| Category | Examples |
|----------|---------|
| Fonts | Default UI font (.ttf), monospace font for text editor |
| Icons | File-type icons (folder, audio, image, document, code, ...) for `OpenFileDialog`; severity icons (info, warning, error, question) for `MessageBox` |

**Do not add:**
- Test data or fixtures (use the `tests/` tree).
- Documentation images (use `docs/` or the repo root for README assets — not `resources/embedded/`).
- Assets for optional modules unless the module is compiled unconditionally. Module-specific assets should be gated with the same CMake option as their module's source.

---

## Relationship to `file_service`

`file_service` manages per-session files in `resource_dir` — both client-uploaded content and, since this refactor, the unpacked built-in assets under `resource_dir/res/`. There is no longer a second, parallel resolution path distinguished by a URI scheme: `resource_store` has exactly one runtime touchpoint (the one-time `extract_to()` call in `session::session()`), after which `file_service` uniformly owns everything under `resource_dir`, with the only distinction being the read-only permission bit on extracted files.

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
| `icons/msgbox_info.png` | Info severity icon for `MessageBox` |
| `icons/msgbox_warning.png` | Warning severity icon for `MessageBox` |
| `icons/msgbox_error.png` | Error severity icon for `MessageBox` |
| `icons/msgbox_question.png` | Question severity icon for `MessageBox` |
