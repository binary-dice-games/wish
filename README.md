# wish

A remote UI framework built on [bison](https://github.com/binary-dice-games/bison). A wish server hosts a UI rendering loop and exposes a set of UI element classes over bison RMI. Client applications connect, define a UI hierarchy (in code or via JSON/YAML), and receive user-interaction events — all without owning a window or a graphics context.

## Architecture

```
Client App
  └─ wish client lib (bdg::wish::client)
       └─ bison RMI transport (TCP socket or PTY)
            └─ wish server (bdg::wish::server)
                 ├─ bison RMI server    <- object create / set / get / call / event
                 ├─ wish::renderer      <- abstract; imgui backend by default
                 └─ wish::file_service  <- per-session sandboxed resource store
```

- The **server** is transport-agnostic. The same UI class registry and renderer work with PTY (Linux), TCP socket, or any other bison transport.
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

PTY transport is only available on Linux builds. See [docs/building.md](docs/building.md) for prerequisites, CMake options, and platform notes.

## Quick Start

### Option A — register and instantiate a UI template

```cpp
// Register a named template (descriptor is parsed server-side on instantiate):
c.register_template("ConfirmDialog"_key, R"({
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
children["ok"_key] = btn.id();
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

## Core Concepts

| Concept | Description |
|---------|-------------|
| **Object hierarchy** | UI elements are bison `dynamic` objects forming a tree. Children are addressed by numeric index (lists) or by name (named slots like `"ok"`, `"cancel"`). Layouts are first-class nodes in the tree. |
| **Layouts** | `VerticalLayout` stacks children top-to-bottom; `HorizontalLayout` places them side by side. Layouts can be nested to create row/column grids and complex arrangements. |
| **UI templates** | Named JSON/YAML blueprints stored on the server via `register_template`. `instantiate_template(name)` parses and creates a fresh object tree, returning a map of named handles. |
| **Remote properties** | `proxy.set(fields)` / `proxy.get()` synchronize typed fields. Property sets are one-way (no round-trip) for low-latency visual updates. |
| **Events** | Server-side interactions emit named events (`clicked`, `changed`, ...) to the client via `proxy.onEvent`. |
| **File service** | Clients upload/download files via `client::upload_file` / `download_file`. Files are stored in a sandboxed per-session folder, deleted on disconnect. |
| **Multi-client** | Each connected client has an isolated session: independent object tree, template registry, and resource folder. |
| **Renderer backends** | `wish::renderer` is an abstract interface. The imgui backend is the default. New backends (Qt, Win32, ...) implement the same interface. |
| **Transports** | PTY (Linux only) or TCP socket. Chosen at runtime; the renderer and class registry are independent of the transport. |

## Further Documentation

| File | Contents |
|------|----------|
| [docs/building.md](docs/building.md) | Prerequisites, CMake options, platform notes |
| [docs/examples.md](docs/examples.md) | Annotated example walkthroughs |
