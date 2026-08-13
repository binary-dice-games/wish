# wish-abi

Python bindings for [wish](https://github.com/binary-dice-games/wish), a
remote-UI framework: a server hosts (and renders) UI that connected clients
build and drive via RMI, over TCP, TLS, named-pipe, or terminal transports.

This package is a thin `ctypes` wrapper around two C ABI shared libraries —
`pip install` compiles both from source (via CMake) and ships them inside
the installed package, so no separate build step is needed afterwards:

- **`wish_client_dll`** (`wish.Client`) — connects to a running wish server
  and drives UI templates.
- **`wish_server_dll`** (`wish.Server`) — hosts a session and actually
  renders the UI clients build, via a real SDL3 window or the web/browser
  renderer (`--renderer=sdl3|web`, matching the `wish server` CLI), plus a
  lightweight `console` renderer for headless/CI use. Only import
  `wish.server` if you need to host a server from Python; `wish.client`
  alone has no dependency on it.

## Install

```bash
git clone --recurse-submodules https://github.com/binary-dice-games/wish.git
pip install ./wish/bindings/python
```

`--recurse-submodules` is required: wish depends on
[bison](https://github.com/binary-dice-games/bison) as a git submodule
(`extern/bison`), and the pip build configures wish's full CMake project
(`cmake.source-dir` points at the repo root) to build just the two ABI
targets above. `bison-abi` itself is installed automatically as a normal
Python dependency — no manual step needed for it.

A C++20 compiler and CMake 3.11+ must be available on the machine running
`pip install`. Unlike `bison-abi`, this build is not lightweight: because
`wish_server_dll` exposes real SDL3/web rendering, the install compiles Dear
ImGui, SDL3, and the web renderer's embedded HTTP/WebSocket server
(civetweb) too — the same dependencies a normal `wish` C++ build needs. See
[docs/building.md](https://github.com/binary-dice-games/wish/blob/main/docs/building.md)
for platform-specific prerequisites.

## Quick start

Client, connecting to an existing server:

```python
from wish import Client

def session(client):
    client.set_style_preset("dark")
    client.register_template("ui", '{"type": "Window", "title": "Hi"}')
    root = client.instantiate_template("ui", "ui")
    print(root["title"])   # "Hi"
    client.wait()          # blocks until an event handler calls client.quit()

Client.tcp("127.0.0.1", 7070).run(session)
```

Server, hosting and rendering a session (SDL3 window by default):

```python
from wish import Server

server = Server.tcp("127.0.0.1", 7070)
server.start(renderer="sdl3")
input("Press Enter to stop...\n")
server.stop()
```

Full binding documentation lives in
[docs/bindings.md](https://github.com/binary-dice-games/wish/blob/main/docs/bindings.md#python-bindingspython).
