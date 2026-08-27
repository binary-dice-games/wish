<img src="resources/Wish.png" alt="Wish" height="200"/>

A remote UI framework built on [bison](https://github.com/binary-dice-games/bison). A wish server hosts a UI rendering loop and exposes a set of UI element classes over bison RMI. Client applications connect, define a UI hierarchy (in code or via JSON/YAML), and receive user-interaction events — all without owning a window or a graphics context.

## Features

- **Remote, declarative UI** — build a widget tree from a separate client process; the server owns rendering, the client owns application logic.
- **~50 built-in widgets** — layouts, inputs, tables, docking/tabs, and full 2D/3D plotting, all backed by Dear ImGui.
- **Two renderer backends** — windowed (SDL3) or browser-based (`--renderer web`, no GPU/window system required).
- **JSON/YAML templates** — describe a UI hierarchy as data and instantiate it by name, or build it object-by-object in code.
- **Multiple transports** — TCP, TLS-secured TCP, named pipe/Unix socket, an interactive terminal hop, or in-process (no serialization) for single-binary demos.
- **Per-session sandboxing** — isolated object tree, template registry, and file-service resource folder per connected client.
- **Automation-ready** — an optional query/screenshot/input-injection API lets Playwright (or an AI agent) drive and introspect a running UI like a browser page; the SDL3 renderer gets the same capabilities natively over the C ABI, no browser required.
- **C++, Python, C#, and Android clients** — a native C++ API, a header-only C++ binding (`bindings/cpp/`, C ABI only, no bison/wish source to compile), plus generated Python/C#/Java bindings, all layered on the same bison RMI proxy semantics.
- **AI-agent-assisted app/UI building** — describe a UI in plain English and have a Claude Code skill build and screenshot-verify it, with no need to know the widget catalog up front. See [docs/ai-assisted-development.md](docs/ai-assisted-development.md).

## Architecture

```
Client App
  └─ wish client lib (bdg::wish::client)
       └─ bison RMI transport (TCP socket or pipes)
            └─ wish server (bdg::wish::server)
                 ├─ bison RMI server    <- object create / set / get / call / event
                 ├─ wish::renderer      <- abstract; imgui backend by default
                 └─ wish::file_service  <- per-session sandboxed resource store
```

- The **server** is transport-agnostic. The same UI class registry and renderer work with TCP socket, or any other bison transport.
- The **renderer** is an abstract interface (`wish::renderer`). The initial implementation uses imgui; other backends can be added without changing client code.
- **UI hierarchies** can be built object-by-object in code, or described in JSON/YAML and loaded via the template system.
- **UI templates** are registered on the server and instantiated by name — the standard way to define a UI hierarchy from a JSON or YAML string.
- The **file service** lets clients upload resources (images, fonts) to a sandboxed per-session folder. The folder is deleted when the client disconnects.
- **Multiple clients** can be connected simultaneously; each has an isolated session, object tree, and resource folder.

## Quick Build

```sh
cmake -S . -B build
cmake --build build
```

See [docs/building.md](docs/building.md) for prerequisites, CMake options, and platform notes.

## Quick Start

### Option A — register and instantiate a UI template

```cpp
// Register a named template (JSON is parsed client-side into a descriptor,
// then sent to the server; register_template also accepts an already-built
// bison::dynamic descriptor directly, and register_template_from_yaml exists
// for YAML text):
c.register_template_from_json("ConfirmDialog"_key, R"({
  "type": "Window", "title": "Confirm", "width": 300, "height": 120,
  "children": {
    "msg":    { "type": "Label",  "text": "" },
    "ok":     { "type": "Button", "label": "OK" },
    "cancel": { "type": "Button", "label": "Cancel" }
  }
})").get();

// Instantiate by name — can be called multiple times for independent copies:
auto dlg = c.instantiate_template("ConfirmDialog"_key).get();
dlg["msg"].set({{"text"_key, std::string{"Delete file?"}}}).get();
dlg["ok"].onEvent("clicked"_key, [](bison::dynamic) { /* ... */ });
```

### Option B — build the hierarchy manually

```cpp
auto win = c.instantiate("wish"_key, "Window"_key).get();
win.set({{"title"_key, std::string{"Hello"}}, {"width"_key, 400}}).get();

auto btn = c.instantiate("wish"_key, "Button"_key).get();
btn.set({{"label"_key, std::string{"OK"}}}).get();

// Named child reference (string key).
bison::dynamic children;
children["ok"_key] = btn.object_id();
win.set({{"children"_key, children}}).get();
```

### Uploading a resource

```cpp
// Upload logo.png; the server stores it in the session resource folder.
c.upload_file("logo.png", file_bytes).get();

// Reference it in an Image element.
auto img = c.instantiate("wish"_key, "Image"_key).get();
img.set({{"src"_key, std::string{"logo.png"}}, {"width"_key, 64}, {"height"_key, 64}}).get();
```

For large files, `upload_file`/`download_file` also accept a `std::istream&`/`std::ostream&` to stream the content in chunks instead of buffering it whole, and `upload_package(dest_path, zip_stream)` uploads a zip archive and has the server unpack it into `dest_path` in the sandbox. See [DESIGN.md](DESIGN.md#bdgwishfile_service) for the chunked-transfer protocol.

New to wish? [docs/tutorial.md](docs/tutorial.md) walks through building, running, and extending a UI end to end, starting from the `calculator` example.

## Core Concepts

| Concept | Description |
|---------|-------------|
| **Object hierarchy** | UI elements are bison `dynamic` objects forming a tree. Children are addressed by numeric index (lists) or by name (named slots like `"ok"`, `"cancel"`). Layouts are first-class nodes in the tree. |
| **Layouts** | `VerticalLayout` stacks children top-to-bottom; `HorizontalLayout` places them side by side. Layouts can be nested to create row/column grids and complex arrangements. A `Spring` child claims a weighted share of leftover space for flexbox-style alignment (centering, space-between). |
| **UI templates** | Named JSON/YAML blueprints stored on the server via `register_template`. `instantiate_template(name)` parses and creates a fresh object tree, returning a map of named handles. |
| **Remote properties** | `proxy.set(fields)` / `proxy.get()` synchronize typed fields. Property sets are one-way (no round-trip) for low-latency visual updates. |
| **Events** | Server-side interactions emit named events (`clicked`, `changed`, ...) to the client via `proxy.onEvent`. |
| **File service** | Clients upload/download files via `client::upload_file` / `download_file` (whole-file or streamed/chunked overloads), plus `upload_package` to upload and unpack a zip archive. Files are stored in a sandboxed per-session folder, deleted on disconnect. |
| **Multi-client** | Each connected client has an isolated session: independent object tree, template registry, and resource folder. |
| **Renderer backends** | `wish::renderer` is an abstract interface. The SDL3 (windowed) and web (browser, via `--renderer web`) backends both build on it — see [docs/building.md](docs/building.md) for the `WISH_ENABLE_WEB` option and [src/web/DESIGN.md](src/web/DESIGN.md) for the browser renderer's architecture. New backends (Qt, terminal/TUI, ...) implement the same interface. |
| **Automation** | `WISH_ENABLE_AUTOMATION` adds a widget-tree/hit-test query API, letting a Playwright-driven headless browser (web renderer) or a `wish.Client` connection (SDL3 renderer, no browser) introspect and drive a running wish UI — see [src/automation/DESIGN.md](src/automation/DESIGN.md), [docs/bindings.md](docs/bindings.md#automation-bindingspythonwishautomationpy), and `CLAUDE.md`'s "Automation" section. |
| **Transports** | TCP socket, named pipe/Unix socket, an interactive terminal hop (`--transport=term`), or an in-memory queue (single-process demos). Chosen at runtime via `--transport`; the renderer and class registry are independent of the transport. See [docs/cli.md](docs/cli.md). |
| **Extending wish** | New server-side UI element classes are bison RMI prototypes registered in the `"wish"` namespace, same pattern as any bison RMI class. See [DESIGN.md](DESIGN.md#adding-a-new-ui-element-class) for the step-by-step pattern. |
| **Object Inspector** | `ObjectInspector` reflects over a `bison::dynamic` object's registered class (fields, `DisplayName`/`Description`/`Range`/`Enum`/`ColorField`/... attributes) to build a Unity/Visual-Studio-style property table + description panel — see [docs/object-inspector.md](docs/object-inspector.md). |

## Security

Every client session gets an isolated, sandboxed temporary directory
(`session::resource_dir`) for uploaded/served files; absolute paths are
rejected by default and file paths are never trusted without going through
`resolve_widget_path()` / `file_service::resolve_path()`. A server can opt
into a persistent, identity-keyed directory per client via an
`auth_module_iface` instead of the default throwaway temp dir. See
`CLAUDE.md`'s "Security Considerations for AI Code Assist" section and
[src/auth/DESIGN.md](src/auth/DESIGN.md) for the full design.

## Further Documentation

| File | Contents |
|------|----------|
| [docs/tutorial.md](docs/tutorial.md) | Beginner-friendly walkthrough of wish with runnable examples |
| [docs/building.md](docs/building.md) | Prerequisites, CMake options, build commands |
| [docs/cli.md](docs/cli.md) | Full `wish` CLI reference: `server`/`client`/`standalone`/`desktop` |
| [docs/examples.md](docs/examples.md) | Annotated example walkthroughs |
| [docs/ui-elements.md](docs/ui-elements.md) | Full UI element/widget catalog (fields, events), window/layout model, JSON template schema, and the `editor` preview tool |
| [docs/visual-editor-options.md](docs/visual-editor-options.md) | Proposal: WYSIWYG editor options — forking ImRAD Studio vs. extending the native `editor` module |
| [docs/bindings.md](docs/bindings.md) | Python, C#, and Android (Java/Kotlin) language binding setup and usage |
| [docs/publishing-python.md](docs/publishing-python.md) | Release runbook for the `wish-abi` PyPI package: Trusted Publisher setup, cibuildwheel matrix, per-release tag steps |
| [docs/ai-assisted-development.md](docs/ai-assisted-development.md) | Building wish apps/UI via natural language with the `wish-module`/`wish-ui` AI-agent skills |
| [docs/automation.md](docs/automation.md) | Driving a running wish UI programmatically (widget tree queries, screenshots, input injection) for debugging and e2e tests |
| [docs/object-inspector.md](docs/object-inspector.md) | `ObjectInspector`: reflection-driven property table + description panel, its field → widget dispatch table, and the new `Hidden`/`Order`/`ColorField`/`Multiline`/`DropTarget` bison attributes |
| [docs/profiling.md](docs/profiling.md) | Capturing a Perfetto trace of the server render loop/layout engine via bison's profiler, and viewing it at ui.perfetto.dev |
| [DESIGN.md](DESIGN.md) | Architecture, key abstractions, object model, and design decisions |

## License

MIT License © 2023 Binary Dice Games. See [LICENSE](LICENSE) for the full text.
