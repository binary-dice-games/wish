# wish — Building

wish targets **Linux and MSYS2 only**; native Windows/MSVC builds are not
supported. This page covers prerequisites, CMake options, and
platform-specific notes for building wish and running the wish server on
Linux and MSYS2 (Windows host).

---

## Prerequisites

### All platforms

| Tool | Minimum version | Notes |
|------|----------------|-------|
| CMake | 3.10 | 3.21+ recommended for `--preset` support |
| C++ compiler | C++20 | GCC 10+, Clang 12+ |
| Git | any | Required to check out submodules |
| Internet access at configure time | — | SDL3, Dear ImGui, ImPlot, and ImPlot3D are fetched automatically via CMake FetchContent (plus civetweb and stb when `WISH_ENABLE_WEB=ON`) |

### Linux

Install the following packages before configuring. Package names are for Debian/Ubuntu; adjust for your distribution.

```sh
sudo apt-get install -y \
    cmake ninja-build \
    build-essential \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev \
    libgl1-mesa-dev libgles2-mesa-dev
```

SDL3 is fetched and built from source by CMake, so no `libsdl3-dev` package is required.

### MSYS2 (Windows host)

Install [MSYS2](https://www.msys2.org/), then from an **MSYS2 MSYS** shell
(not MinGW64, not `cmd.exe`/PowerShell) install the toolchain:

```bash
pacman -S --needed \
    mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-libuv pkg-config
```

wish's core dependency (`extern/bison`) picks up MSYS2's system `libuv` via
`pkg-config` automatically; configuring and building from here on is
identical to Linux.

---

## Getting the source

Clone the repository and initialise the `extern/bison` submodule:

```sh
git clone https://github.com/binary-dice-games/wish.git
cd wish
git submodule update --init --recursive
```

---

## Configuring

Run CMake from the repository root. The defaults build everything including the SDL3 renderer, the wish server, and the test suite.

```sh
cmake -S . -B build
```

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `WISH_ENABLE_IMGUI` | `ON` | Build the Dear ImGui renderer. Required for SDL3 renderer and wish server. |
| `WISH_ENABLE_SDL3` | `ON` | Build the SDL3 windowed renderer, the wish server, and the calculator/demo examples. |
| `WISH_ENABLE_WEB` | `OFF` | Build the web renderer (`--renderer web`): a browser-based backend over HTTP + WebSocket, using civetweb and a first-party binary draw-data protocol (no OpenGL/window system required). Requires no additional system packages beyond what's already needed (SSL is compiled out, so no OpenSSL dependency). Can be combined with `WISH_ENABLE_SDL3` in the same binary; `WISH_ENABLE_SDL3=OFF -DWISH_ENABLE_WEB=ON` builds `wish server`/`wish-server` with no windowing/GPU dependency at all. |
| `WISH_ENABLE_AUTOMATION` | `OFF` | Build the automation query API on top of the web renderer: a widget-tree/hit-test query protocol that lets a Playwright-driven headless browser (or an AI agent) introspect and drive a running wish UI, in addition to the screenshot/input control it already gets for free from the web renderer. Requires `WISH_ENABLE_WEB=ON` (configure-time error otherwise). See [src/automation/DESIGN.md](../src/automation/DESIGN.md) and `CLAUDE.md`'s "Automation" section. |
| `WISH_BUILD_SHARED` | `ON` | Build `wish_client` as a shared library with a C ABI (`wish_client.dll` on MSYS2 / `libwish_client.so` on Linux). |
| `WISH_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `WISH_COLLECTION_BDG_DESKTOP` | `OFF` | Include every module in `modules/bdg/desktop/` (calculator, notepad, process_explorer) — see below. |
| `WISH_MODULE_BDG_DESKTOP_CALCULATOR` | `OFF` | Include the Calculator form (server) and its self-registering reference client runner. |
| `WISH_MODULE_BDG_DESKTOP_NOTEPAD` | `OFF` | Include the Notepad form (server) and its self-registering reference client runner. |
| `WISH_MODULE_BDG_DESKTOP_PROCESS_EXPLORER` | `OFF` | Include the Process Explorer form (server) and its self-registering reference client runner. |

Modules live in a `modules/<organization>/<collection>/<module>` tree (see
[modules/README.md](../modules/README.md)); each individual
`WISH_MODULE_<ORG>_<COLLECTION>_<NAME>` option can be set directly, or a
whole collection enabled at once with `-DWISH_COLLECTION_<ORG>_<COLLECTION>=ON`
(individual module options still override it, e.g.
`-DWISH_COLLECTION_BDG_DESKTOP=ON -DWISH_MODULE_BDG_DESKTOP_NOTEPAD=OFF`). A
3rd-party project consuming wish via `add_subdirectory()`/`FetchContent` can
register its own module or collection the same way, with its source living
outside the wish repo — see [Out-of-tree modules](../src/ui/forms/DESIGN.md#out-of-tree-modules-3rd-party-projects)
in `src/ui/forms/DESIGN.md`.

Enabled modules' client-side code is also compiled into `wish_client_dll`
(when `WISH_BUILD_SHARED=ON`), reachable from Python via
`wish.client.list_apps()`/`Client.run_app()` — see
[Client modules and wish_client_dll](../src/ui/forms/DESIGN.md#client-modules-and-wish_client_dll).

Example — headless/CI build with no window system:

```sh
cmake -S . -B build -DWISH_ENABLE_SDL3=OFF -DWISH_BUILD_TESTS=OFF
```

Example — Release build (Ninja):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

---

## Building

```sh
cmake --build build
```

To build a specific target:

```sh
cmake --build build --target wish-cli     # unified `wish` binary (server/client/standalone/desktop subcommands)
cmake --build build --target calculator
cmake --build build --target demo
```

### Output locations

`wish-cli` is the single binary most users want — it dispatches to
`server`/`client`/`standalone`/`desktop` subcommands (see below) and its
output filename is `wish`, not `wish-cli`. Single-purpose binaries
(`wish-server`, `wish-client`, `wish-standalone`, `wish-desktop`) build
alongside it with no subcommand needed, for callers that only want one mode
and a smaller dependency footprint (e.g. `wish-client` links neither
SDL3 nor ImGui).

CMake mirrors the source tree under `build/` (no flattened output directory
is configured), so every `app/wish_cli` binary lands under `build/app/`:

| Generator | Unified CLI | Single-purpose binaries |
|-----------|-------------|--------------------------|
| Ninja / Makefiles (Linux) | `build/app/wish` | `build/app/wish-server`, `build/app/wish-client`, `build/app/wish-standalone`, `build/app/wish-desktop` |
| Ninja / Makefiles (MSYS2) | `build/app/wish.exe` | `build/app/wish-server.exe`, `build/app/wish-client.exe`, `build/app/wish-standalone.exe`, `build/app/wish-desktop.exe` |

---

## Running the wish server

`wish server` (or the standalone `wish-server` binary, equivalent flags, no
subcommand) opens an SDL3 window, or — with `--renderer web` (the default) —
a browser endpoint, and renders UI pushed by connected clients over the
transport selected at launch.

```sh
# Linux
./build/app/wish server

# MSYS2
./build/app/wish.exe server
```

See [docs/cli.md](cli.md) for the full `wish server` flag reference (transport
selection, window/renderer options), the other three subcommands (`client`,
`standalone`, `desktop`), the `wish <app>` alias, and the `WISH_<FLAG>`
environment-variable fallback available for every flag.

Close the window, or choose **Server → Quit** from the menu bar, to stop the server.

### Running the web renderer

Requires a build with `-DWISH_ENABLE_WEB=ON`:

```sh
./build/app/wish server --renderer web --web_port 8080
```

Then open `http://localhost:8080` in a browser. Ctrl+C stops the process —
there's no window to close, and the server does not auto-quit when no
browser is connected. See [src/web/DESIGN.md](../src/web/DESIGN.md) for the
protocol and architecture.

### Running automation

Requires a build with `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON`.
The server itself is launched exactly like the plain web renderer — no new
flags — automation just adds two new WebSocket message types the browser
client already knows how to speak:

```sh
./build/app/wish server --renderer web --web_port 8080
```

Drive it with `wish.automation.AutomationClient` (`bindings/python/wish/automation.py`,
needs the `playwright` package — `pip install playwright && playwright install chromium`):

```python
from wish.automation import AutomationClient

with AutomationClient.launch(url="http://127.0.0.1:8080") as ui:
    tree = ui.get_tree()
    ui.click("dialog.ok")
    png_bytes = ui.screenshot()
```

`AutomationClient.launch(server_cmd=[...])` can also start the server
subprocess itself (picking a free port automatically) instead of attaching
to one already running via `url=`. See
[src/automation/DESIGN.md](../src/automation/DESIGN.md) for the protocol,
and `CLAUDE.md`'s "Automation: debugging and testing a wish UI" section for
the agent-facing workflow (investigating a bug, driving e2e tests).

---

## Running the examples

See [docs/examples.md](examples.md) for annotated walkthroughs of each example. Quick reference:

```sh
# Linux
./build/examples/calculator
./build/examples/demo

# MSYS2
./build/examples/calculator.exe
./build/examples/demo.exe
```
