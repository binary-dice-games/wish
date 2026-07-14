# wish — Examples

This page describes every runnable example in the `examples/` directory: what it demonstrates and how to build and launch it.

---

## Prerequisites

| Tool | Minimum version |
|------|----------------|
| CMake | 3.10 |
| C++ compiler | C++20 (GCC 10+, Clang 12+) |
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

CMake mirrors the source tree under `build/` (no flattened output directory
is configured), so example binaries land under `build/examples/`:

| Generator | Binary location |
|-----------|----------------|
| Ninja / Unix Makefiles (Linux) | `build/examples/calculator` |
| Ninja / Unix Makefiles (MSYS2) | `build/examples/calculator.exe` |

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
# Linux
./build/examples/calculator

# MSYS2
./build/examples/calculator.exe
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

## Demo (`examples/demo/`)

### What it demonstrates

A comprehensive showcase of every wish widget and layout type in a single dockable window — the wish equivalent of `ImGui::ShowDemoWindow`. It is the canonical reference for what the framework can render.

- **DockSpaceViewport root** — the top-level node is a `DockSpaceViewport` so the inner window can be undocked, resized, and rearranged by the user.
- **Menu bar** — a `MenuBar` with File, View, and Options menus; menu-item `clicked` and `checked` events are forwarded to the status bar.
- **TabBar with nine tabs** — each tab is a self-contained widget group:

| Tab | Widgets shown |
|-----|---------------|
| Basics | `Label`, `Button` (including fixed-width), `Checkbox`, `RadioButton`, visibility toggle |
| Sliders & Drags | `SliderFloat`, `SliderInt`, `DragFloat`, `DragInt` |
| Text & Numbers | `InputText` (with hint), `InputInt`, `InputFloat` |
| Selection | `Combo`, `Selectable` |
| Tree & Collapse | `TreeNode` (nested), `CollapsingHeader` |
| Misc | `ProgressBar`, `HorizontalLayout`, `VerticalLayout` (nested rows), runtime theme switching |
| Tables | Static `Table` with borders and row backgrounds; interactive `Table` with buttons in cells |
| Plots | `Plot` containing `PlotLine`, `PlotScatter`, `PlotStairs`, `PlotStems`, `PlotShaded`, `PlotDigital`, `PlotBars`, `PlotBarsH`, `PlotHistogram`, `PlotHistogram2D`, `PlotHeatmap`, `PlotPieChart`, `PlotText`, `PlotInfLines` |
| 3-D Plots | `Plot3D` containing `Plot3DLine`, `Plot3DScatter`, `Plot3DSurface`, `Plot3DTriangle`, `Plot3DQuad`, `Plot3DMesh`, `Plot3DText` |

- **Status bar** — a `Label` at the bottom of the window that displays the last event received from any widget.
- **Runtime theme switching** — `set_style_preset` is called at startup and also wired to the theme buttons and View menu, showing how the server-side style can be changed while the session is live.
- **In-memory transport** — server and client run in the same process (same as the calculator).

### Running

```sh
# Linux
./build/examples/demo

# MSYS2
./build/examples/demo.exe
```

Command-line flags:

| Flag | Effect |
|------|--------|
| `--verbose` / `-v` | Print session lifecycle messages to `stderr` |
| `--theme dark\|light\|classic` / `-t <theme>` | Set the initial ImGui theme (default: `dark`) |

A 900 × 950 window opens titled "wish Widget Demo". Use the tab bar to browse widget groups. Every interaction updates the status label at the bottom of the window.

### Architecture of the example

```
main()
  │
  ├─ memory_server_transport transport
  ├─ wish::server{transport, sdl3_renderer("wish Widget Demo", 900, 950)}
  │    └─ server.start()
  │
  └─ demo_client{transport.connect(), renderer_ptr, verbose, theme}.run()
       └─ on_session():
            ├─ set_style_preset(theme)
            ├─ register_template("demo", kDemoDescStr)
            ├─ pm = instantiate_template("demo")
            ├─ push plot data (xs/ys/zs) to Plot / Plot3D series
            ├─ register onEvent handlers for every interactive widget
            └─ while (!renderer->should_quit()) sleep 16ms
```

The full UI descriptor is assembled at compile time from nine per-tab `constexpr` string fragments (`kTabBasicsDesc`, `kTabSlidersDesc`, …) concatenated into `kDemoDescStr`. Each fragment is a JSON object subtree; the root node is a `DockSpaceViewport` containing a `MenuBar` and the main `Window`.

---

## Planned examples (not yet implemented)

| Example | Step | Description |
|---------|------|-------------|
| `socket_server` / `socket_client` | Step 18 | Server listening on TCP port 7070; client connects over the network. Linux + MSYS2. |
