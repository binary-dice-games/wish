# wish — Examples

This page describes every runnable example in the `examples/` directory: what it demonstrates and how to build and launch it.

---

## Prerequisites

| Tool | Minimum version |
|------|----------------|
| CMake | 3.10 |
| C++ compiler | C++20 (MSVC 19.29+, GCC 10+, Clang 12+) |
| Internet access at configure time | SDL3 and Dear ImGui are fetched automatically via CMake FetchContent — no manual download needed |

---

## Building

All examples are built as part of the normal wish build. Run once from the repository root:

```sh
cmake -S . -B build
cmake --build build
```

To build only the calculator (faster if you don't need the test suite):

```sh
cmake -S . -B build -DWISH_BUILD_TESTS=OFF
cmake --build build --target calculator
```

To disable the SDL3 renderer and calculator (headless / CI builds):

```sh
cmake -S . -B build -DWISH_ENABLE_SDL3=OFF
cmake --build build
```

### Finding the binary

| Generator | Binary location |
|-----------|----------------|
| Ninja / Unix Makefiles (Linux, macOS) | `build/calculator` |
| Visual Studio (Windows, default) | `build\Debug\calculator.exe` or `build\Release\calculator.exe` |
| Ninja on Windows | `build\calculator.exe` |

To build in Release mode with Visual Studio:

```sh
cmake -S . -B build
cmake --build build --config Release --target calculator
```

---

## Calculator (`examples/calculator/`)

### What it demonstrates

A self-contained 4-function calculator that exercises the full wish stack end to end inside a single process:

- **Template registration** — the calculator UI is described as a JSON string and registered with `wish::client::register_template`.
- **Template instantiation** — `instantiate_template` parses the descriptor on the server and returns a flat map of named proxies (`display`, `row0.c`, `row1.n7`, …).
- **Property updates** — every button click calls `proxy.set({{"text"_key, result}})` on the display label, which propagates to the server-side object tree on the next render tick.
- **Event handling** — each button proxy gets an `onEvent("clicked"_key, handler)` registration; the server-side imgui renderer emits the event when the button is pressed.
- **sdl3_renderer** — the server renders the UI into a real SDL3 window using the Dear ImGui SDL3 backend. Closing the window shuts the server down cleanly.
- **In-memory transport** — server and client run in the same process, connected over `memory_server_transport` / `memory_client_transport` (no network required).

### Running

```sh
# Linux / macOS
./build/calculator

# Windows (Visual Studio generator, Debug build)
.\build\Debug\calculator.exe

# Windows (Ninja generator)
.\build\calculator.exe
```

A 300 × 420 window opens with a standard calculator layout:

```
┌─────────────────────────┐
│  0                      │  ← display label
├─────────────────────────┤
│  C    /    *    <-      │
│  7    8    9    -       │
│  4    5    6    +       │
│  1    2    3    =       │
│  0    .   +/-   %      │
└─────────────────────────┘
```

| Button | Action |
|--------|--------|
| `0`–`9` | Append digit to current number |
| `.` | Append decimal point (ignored if already present) |
| `+` `-` `*` `/` | Store current number and operator; start new entry |
| `=` | Compute and display result |
| `C` | Reset to `0` |
| `<-` | Delete last character (backspace) |
| `+/-` | Negate current number |
| `%` | Divide current number by 100 |

Close the window to exit. The process returns exit code 0.

### Architecture of the example

```
main()
  │
  ├─ memory_server_transport transport
  ├─ wish::server{transport, sdl3_renderer("Calculator", 300, 420)}
  │    └─ server.start()
  │         ├─ render thread: setup() → SDL window + ImGui context
  │         │                 loop: begin_frame / render / end_frame
  │         │                 teardown() on window close
  │         └─ bison listen thread: accepts in-memory connections
  │
  └─ calc_client{transport.connect(), renderer_ptr}.run()
       └─ on_session():
            ├─ register_template("calc", JSON_descriptor)
            ├─ pm = instantiate_template("calc")
            ├─ pm["row0.c"].onEvent("clicked", clear_handler)
            ├─ pm["row1.n7"].onEvent("clicked", digit_handler("7"))
            ├─ … (one handler per button)
            └─ while (!renderer->should_quit()) sleep 16ms
```

When the user closes the SDL window:
1. `SDL_EVENT_QUIT` is caught in `sdl3_renderer::begin_frame()`.
2. `should_quit()` returns `true`.
3. The client's `on_session()` loop exits → client disconnects.
4. Back in `main()`, `server.stop()` is called.
5. The render loop exits and `teardown()` destroys SDL objects.

---

## Planned examples (not yet implemented)

| Example | Step | Description |
|---------|------|-------------|
| `pty_server` / `pty_client` | Step 17 | Minimal server + client communicating over a Linux PTY. Linux only. |
| `socket_server` / `socket_client` | Step 18 | Server listening on TCP port 7070; client connects over the network. Windows + Linux. |
