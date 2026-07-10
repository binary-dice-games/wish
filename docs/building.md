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
| `WISH_BUILD_SHARED` | `ON` | Build `wish_client` as a shared library with a C ABI (`wish_client.dll` on MSYS2 / `libwish_client.so` on Linux). |
| `WISH_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `WISH_MODULE_CALCULATOR` | `OFF` | Include the Calculator form (server) and its self-registering reference client runner. |
| `WISH_MODULE_NOTEPAD` | `OFF` | Include the Notepad form (server) and its self-registering reference client runner. |
| `WISH_MODULE_PROCESS_EXPLORER` | `OFF` | Include the Process Explorer form (server) and its self-registering reference client runner. |

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
cmake --build build --target wish-server
cmake --build build --target calculator
cmake --build build --target demo
```

### Output locations

| Generator | Executable |
|-----------|-----------|
| Ninja / Makefiles (Linux) | `build/wish` |
| Ninja / Makefiles (MSYS2) | `build/wish.exe` |

---

## Running the wish server

The wish server opens an SDL3 window that acts as the rendering host. Clients connect over TCP or a Unix domain socket and push UI to the window.

```sh
# Linux
./build/wish

# MSYS2
./build/wish.exe
```

### Command-line flags

| Flag | Default | Description |
|------|---------|-------------|
| `--host HOST` | `0.0.0.0` | Bind address for the TCP transport |
| `--port PORT` | `7070` | TCP listen port |
| `--pipe PATH` | *(empty)* | Unix-socket path; when set, TCP is not used |
| `--verbose` | `false` | Print session lifecycle messages to stdout |
| `--title TITLE` | `wish` | Window title (`--renderer sdl3` only) |
| `--width N` | `1280` | Initial window width in pixels (`--renderer sdl3` only) |
| `--height N` | `720` | Initial window height in pixels (`--renderer sdl3` only) |
| `--renderer NAME` | `sdl3` | Rendering backend: `sdl3` or `web` |
| `--web_port PORT` | `8080` | HTTP/WebSocket port (`--renderer web` only) |
| `--web_bind ADDR` | `127.0.0.1` | Bind address (`--renderer web` only; localhost-only by default) |

**Example — listen on a non-default port with a custom window title:**

```sh
./build/wish --port 9090 --title "My App Server"
```

### Environment-variable flag defaults

Every `wish`/`wish-*` flag falls back to a `WISH_<FLAG_NAME_UPPERCASED>`
environment variable when not given on the command line (e.g.
`WISH_TRANSPORT`, `WISH_HOST`, `WISH_PORT`, `WISH_NAME`,
`WISH_DOWNSTREAM_PORT`, `WISH_WEB_BIND`, ...) -- an explicit command-line
flag always wins. `wish desktop` sets `WISH_TRANSPORT`/`WISH_HOST`/
`WISH_PORT`/`WISH_NAME` in the terminal it spawns to match its own
`--downstream_transport`/`--downstream_host`/`--downstream_port`/
`--downstream_name`, so a `wish client`/`wish server` launched from that
terminal connects to the desktop with no flags:

```sh
wish desktop                      # downstream defaults to tcp:7071
# inside the spawned terminal:
wish client --run notepad         # connects to the desktop's tcp:7071, no flags needed
```

Close the window, or choose **Server → Quit** from the menu bar, to stop the server.

### Running the web renderer

Requires a build with `-DWISH_ENABLE_WEB=ON`:

```sh
./build/wish server --renderer web --web_port 8080
```

Then open `http://localhost:8080` in a browser. Ctrl+C stops the process —
there's no window to close, and the server does not auto-quit when no
browser is connected. See [src/web/DESIGN.md](../src/web/DESIGN.md) for the
protocol and architecture.

---

## Running the examples

See [docs/examples.md](examples.md) for annotated walkthroughs of each example. Quick reference:

```sh
# Linux
./build/calculator
./build/demo

# MSYS2
./build/calculator.exe
./build/demo.exe
```
