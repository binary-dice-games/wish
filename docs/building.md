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
| Internet access at configure time | — | SDL3, Dear ImGui, ImPlot, and ImPlot3D are fetched automatically via CMake FetchContent |

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
| `WISH_BUILD_SHARED` | `ON` | Build `wish_client` as a shared library with a C ABI (`wish_client.dll` on MSYS2 / `libwish_client.so` on Linux). |
| `WISH_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |
| `WISH_MODULE_PROCESS_EXPLORER` | `OFF` | Include the `ProcessExplorer` form (top/htop-style system monitor). OS-specific: reads `/proc` on Linux/MSYS2. |

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
| `--title TITLE` | `wish` | Window title |
| `--width N` | `1280` | Initial window width in pixels |
| `--height N` | `720` | Initial window height in pixels |

**Example — listen on a non-default port with a custom window title:**

```sh
./build/wish --port 9090 --title "My App Server"
```

Close the window, or choose **Server → Quit** from the menu bar, to stop the server.

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
