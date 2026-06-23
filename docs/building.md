# wish — Building

This page covers prerequisites, CMake options, and platform-specific notes for building wish and running the wish server on Windows and Linux.

---

## Prerequisites

### All platforms

| Tool | Minimum version | Notes |
|------|----------------|-------|
| CMake | 3.10 | 3.21+ recommended for `--preset` support |
| C++ compiler | C++20 | MSVC 19.29+, GCC 10+, Clang 12+ |
| Git | any | Required to check out submodules |
| Internet access at configure time | — | SDL3, Dear ImGui, ImPlot, and ImPlot3D are fetched automatically via CMake FetchContent |

### Windows

- **Visual Studio 2022** (recommended) with the "Desktop development with C++" workload, or
- **MSVC + CMake + Ninja** installed via the Visual Studio installer.

No additional system libraries are needed; all graphics dependencies are fetched automatically.

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
| `WISH_BUILD_SHARED` | `ON` | Build `wish_client` as a shared library with a C ABI (`wish_client.dll` / `libwish_client.so`). |
| `WISH_BUILD_TESTS` | `ON` | Build and register the GoogleTest suite. |

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

To build in Release mode with the Visual Studio generator (Windows):

```sh
cmake --build build --config Release
```

### Output locations

| Generator | Executable |
|-----------|-----------|
| Visual Studio (Windows) | `build\Debug\wish.exe`, `build\Release\wish.exe` |
| Ninja on Windows | `build\wish.exe` |
| Ninja / Makefiles (Linux) | `build/wish` |

---

## Running the wish server

The wish server opens an SDL3 window that acts as the rendering host. Clients connect over TCP (or a named pipe on Windows / Unix socket on Linux) and push UI to the window.

### Windows

```bat
.\build\Debug\wish.exe
```

Or with Ninja:

```bat
.\build\wish.exe
```

### Linux

```sh
./build/wish
```

### Command-line flags

| Flag | Default | Description |
|------|---------|-------------|
| `--host HOST` | `0.0.0.0` | Bind address for the TCP transport |
| `--port PORT` | `7070` | TCP listen port |
| `--pipe PATH` | *(empty)* | Named-pipe / Unix-socket path; when set, TCP is not used |
| `--pty` | `false` | Use PTY transport — **Linux only** (see below) |
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

## PTY transport (Linux only)

On Linux, the server can spawn a shell process and communicate with it over a pseudo-terminal instead of a socket. This is useful for wrapping interactive TUI programs.

```sh
./build/wish --pty --cmd bash
```

| Flag | Default | Description |
|------|---------|-------------|
| `--pty` | `false` | Enable PTY transport |
| `--cmd CMD` | `bash` | Shell command to spawn |

PTY transport is not available on Windows; the `--pty` and `--cmd` flags are not defined in Windows builds.

---

## Running the examples

See [docs/examples.md](examples.md) for annotated walkthroughs of each example. Quick reference:

```sh
# Linux
./build/calculator
./build/demo

# Windows (Visual Studio generator, Debug)
.\build\Debug\calculator.exe
.\build\Debug\demo.exe
```
