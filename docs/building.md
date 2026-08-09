# wish — Building

wish targets **Linux, MSYS2, and native Windows (MSVC)**. This page covers
prerequisites, CMake options, and platform-specific notes for building wish
and running the wish server on all three.

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

### Native Windows (MSVC)

Install [Visual Studio](https://visualstudio.microsoft.com/) (2022 or later)
with the "Desktop development with C++" workload, and
[CMake](https://cmake.org/download/) 3.21+. From a **Developer Command
Prompt** or **Developer PowerShell** (so `cl.exe`/MSVC is on `PATH`):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

No `libuv` package install is needed — on native Windows (detected via
`WIN32 AND NOT MSYS AND NOT CYGWIN` in `extern/bison/CMakeLists.txt`), bison
builds its bundled `extern/libuv` from source instead of looking for a
system/pkg-config copy, unlike the Linux/MSYS2 path above.

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
| `WISH_ENABLE_AUTOMATION` | `OFF` | Build the automation query API: a widget-tree/hit-test query protocol that lets an AI agent (or a pytest-style e2e suite) introspect and drive a running wish UI. On the web renderer this adds a Playwright-driven headless-browser path (screenshot/input control it already gets for free, plus two new WebSocket message types). On the SDL3 renderer this adds a native path built directly into the wish C ABI (no browser). Requires `WISH_ENABLE_WEB=ON` and/or `WISH_ENABLE_SDL3=ON` (configure-time error otherwise). See [src/automation/DESIGN.md](../src/automation/DESIGN.md) and `CLAUDE.md`'s "Automation" section. |
| `WISH_BUILD_SHARED` | `ON` | Build `wish_client` as a shared library with a C ABI (`wish_client.dll` on MSYS2/native Windows / `libwish_client.so` on Linux). |
| `WISH_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `WISH_COLLECTION_BDG_DESKTOP` | `OFF` | Include every module in `modules/bdg/desktop/` (calculator, log_tail, notepad, pix, process_explorer, zip_tool) — see below. |
| `WISH_MODULE_BDG_DESKTOP_CALCULATOR` | `OFF` | Include the Calculator form (server) and its self-registering reference client runner. |
| `WISH_MODULE_BDG_DESKTOP_LOG_TAIL` | `OFF` | Include the Log Tail form (server) and its self-registering reference client runner — a `tail`-like log viewer (`wish client --run=log_tail -- [-f] [-n N] FILE...`). |
| `WISH_MODULE_BDG_DESKTOP_NOTEPAD` | `OFF` | Include the Notepad form (server) and its self-registering reference client runner. |
| `WISH_MODULE_BDG_DESKTOP_PIX` | `OFF` | Include the Pix form (server) and its self-registering reference client runner — a local image folder viewer (`wish client --run=pix`); the client runner needs `stb_image`/`stb_image_resize2`/`stb_image_write` (`extern/stb`, fetched on demand). |
| `WISH_MODULE_BDG_DESKTOP_PROCESS_EXPLORER` | `OFF` | Include the Process Explorer form (server) and its self-registering reference client runner. |
| `WISH_MODULE_BDG_DESKTOP_ZIP_TOOL` | `OFF` | Include the Zip Tool form (server) and its self-registering reference client runner (client-side compress/extract/list-contents via miniz). |
| `WISH_COLLECTION_BDG_DEV` | `OFF` | Include every module in `modules/bdg/dev/` (currently just editor) — see below. |
| `WISH_MODULE_BDG_DEV_EDITOR` | `OFF` | Include the Editor form (server) and its self-registering reference client runner — a live JSON UI mock editor (`wish client --run=editor -- path/to/ui.json`). |

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
| Visual Studio (native Windows) | `build/app/Release/wish.exe` | `build/app/Release/wish-server.exe`, `build/app/Release/wish-client.exe`, `build/app/Release/wish-standalone.exe`, `build/app/Release/wish-desktop.exe` |

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
needs the `playwright` package — `pip install playwright && playwright install chromium`).
On Linux, downloading the browser binary isn't enough on its own: the first
time, also run `sudo playwright install-deps chromium` to install the OS
shared libraries (`libnspr4`, `libnss3`, ...) Chromium needs to launch —
without it, `AutomationClient.launch()` fails with an
`error while loading shared libraries: libnspr4.so: ...` error. Skip either
step if a working Chromium is already configured (e.g. via
`PLAYWRIGHT_BROWSERS_PATH`).

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

### Running native automation (SDL3)

Requires a build with `-DWISH_ENABLE_SDL3=ON -DWISH_ENABLE_AUTOMATION=ON`
(no `WISH_ENABLE_WEB` needed). Launch the server exactly like the plain SDL3
renderer, over any transport (TCP shown here):

```sh
./build/app/wish server --renderer sdl3 --transport tcp --port 7070
```

Then drive it with an ordinary `wish.Client` connection — no Playwright, no
browser, no second client:

```python
from wish import Client

def session(c: Client) -> None:
    tree = c.get_tree()
    c.click("dialog.ok")
    png_bytes = c.screenshot()

client = Client.tcp("127.0.0.1", 7070)
client.run(session)
client.destroy()
```

Raises `WishError(code=WISH_ERR_NOT_FOUND)` if the connected server's active
renderer doesn't support automation. For a headless CI run with no real
display, set `SDL_VIDEODRIVER=dummy` (and `SDL_RENDER_DRIVER=software`) in
the server subprocess's environment. See
[src/automation/DESIGN.md](../src/automation/DESIGN.md)'s "Native
(ABI-based) automation" section for the architecture.

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

---

## Packaging a release

`cmake/Packaging.cmake` adds `install()` rules (tagged `COMPONENT wish`, to
keep FetchContent-vendored dependencies like SDL3/civetweb out of the
package) plus a CPack `ZIP` generator config, so any configured build can
produce a release zip directly:

```sh
cmake --build build --target package   # or: cd build && cpack -G ZIP
```

That zip has the CLI binaries, `wish_client_dll` + its public C headers,
`docs/` + `README.md`, and the binding sources and examples
(`bindings/{cpp,python,csharp}/examples/`). `scripts/package_release.py`
wraps this end to end — configuring a Release build with the recommended
options, building, running `cpack`, then bolting on the pieces CPack can't
produce on its own: compiled C# binding DLLs (via `dotnet publish`, if
`dotnet` is on `PATH`), the MSYS2/native-Windows runtime DLLs the binaries
dynamically link against (via `ldd`), and `extern/bison`'s Python binding
(a sibling import `bindings/python/wish/_native.py` needs at runtime):

```sh
python3 scripts/package_release.py --version 1.2.3
```

Produces `dist/wish-<version>-<System>-<arch>.zip`. Run it once per platform
(Linux, MSYS2, native Windows) to produce that platform's release asset.

### Putting the release on PATH

The zip also ships a pair of small scripts (`packaging/unix/` on Linux,
`packaging/windows/` on Windows — installed to the zip's root) so `wish` and
`wish_client.dll`/`libwish_client.so` are usable from any working directory
after extracting, not just from inside the zip's own `bin/`:

- **`wish-env.sh` / `wish-env.ps1` / `wish-env.cmd`** — session-only, no
  files modified. `source ./wish-env.sh` (Linux), `. .\wish-env.ps1`
  (PowerShell), or `wish-env.cmd` (cmd.exe).
- **`install.sh` / `install.ps1`** — persists the change so new
  terminals pick it up automatically, without asking the user each time:
  appends a marked, idempotent block to `~/.bashrc`/`~/.zshrc` on Linux, or
  (on Windows, via `install.ps1`) adds the zip's `bin\` to the per-user
  `HKCU` `PATH` — no administrator rights needed, and never touches the
  machine-wide PATH. Both accept `--uninstall` / `-Uninstall` to remove what
  they added.

The two platforms need different mechanisms because their dynamic linkers
work differently:

- **Windows** searches every directory on `PATH` when resolving a DLL, so
  putting `bin\` on `PATH` is sufficient for *both* running `wish.exe` from
  anywhere *and* letting a separate program (a C# app P/Invoking
  `wish_client.dll`, Python `ctypes.CDLL("wish_client.dll")` without
  `WISH_LIB` set) find it.
- **Linux's dynamic linker does not consult `PATH`** for shared libraries —
  only `LD_LIBRARY_PATH`, `/etc/ld.so.conf(.d/)` (which needs root and
  `ldconfig`, inappropriate for a per-user zip extraction), or an
  executable's own rpath. `PATH` alone makes `wish`/`wish-server`/etc.
  runnable from anywhere, but a *separate* program linking
  `libwish_client.so` still needs `LD_LIBRARY_PATH` (or `WISH_LIB`, which
  `bindings/python/wish/_native.py` reads directly) — both scripts set all
  three (`PATH`, `LD_LIBRARY_PATH`, `WISH_LIB`) to cover every consumer.

### Release vs. Debug

By default the script always configures and builds `Release`
(`-DCMAKE_BUILD_TYPE=Release` for single-config generators like Ninja/Make,
`--config Release`/`cpack -C Release` for multi-config generators like
Visual Studio) — you don't need to pass anything extra for a normal run to
produce Release binaries.

This only needs attention if you point `--build-dir` at a build directory
that already exists from earlier day-to-day development (e.g. a `Debug`
build you've been iterating in) **and** pass `--skip-configure`: skipping
configure also skips the `-DCMAKE_BUILD_TYPE=Release` reconfigure, so
`cpack` packages whatever that directory was last built as. Either use a
dedicated build directory for packaging (the default, `build-release`,
already avoids this — it's never shared with a Debug dev build), or drop
`--skip-configure` so the script reconfigures it to `Release` itself:

```sh
# Safe: dedicated build dir, always configured/built fresh as Release.
python3 scripts/package_release.py

# Also fine: reconfigures an existing dir to Release before packaging.
python3 scripts/package_release.py --build-dir build

# Only do this once you've confirmed --build-dir is already a Release
# build -- --skip-configure --skip-build trusts it as-is and just repacks.
python3 scripts/package_release.py --build-dir build --skip-configure --skip-build
```
