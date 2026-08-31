# wish

Go bindings for [wish](https://github.com/binary-dice-games/wish):

- **`wish/`** wraps the **client** C ABI (`wish_client_c.h`) -- connect to a
  running `wish server` process and drive UI templates from Go.
- **`wishserver/`** wraps the **server** C ABI (`wish_server_c.h`) -- host
  and render a real wish session from Go, the same `bdg::wish::server`
  implementation the `wish server` CLI uses.

They are **separate packages on purpose**: `libwish_client` and
`libwish_server` both export the `bison_*`/`rmi_*` C ABI symbols, so a
single binary must link exactly one of them (the same constraint
`bindings/cpp/`'s two targets, the Python binding's two `ctypes` modules,
and `bindings/rust/`'s two crates all carry). Import `wish` to drive a UI,
`wishserver` to host one.

Both packages link directly against their precompiled shared library at
build time via `cgo` -- the same model `bindings/cpp/` and `bindings/rust/`
use. This module is not published as a Go module elsewhere; use it from a
checkout of this repository (import paths
`github.com/binary-dice-games/wish/bindings/go/wish` and `.../wishserver`).

## Build

Build `wish_client_dll` first (from the repo root):

```bash
cmake -B build -DWISH_BUILD_SHARED=ON
cmake --build build --target wish_client_dll
```

Then build the module:

```bash
cd bindings/go
go build ./...
```

`wish/native.go`'s `#cgo` directives resolve `wish_client_c.h` (this
checkout's own `include/`), `bison_c.h`/`rmi_c.h` (`extern/bison/include/`
-- a separate directory from wish's own `include/`, since wish's C++ build
picks those up transitively from the `bison` CMake target's public include
dirs, a shortcut not available to a cgo preamble), and `libwish_client`
(`build/`) against this checkout's directory layout by default. To build
against a `wish_client` installed elsewhere, set `CGO_CFLAGS`/`CGO_LDFLAGS`
before building -- the Go toolchain merges env-supplied flags with the
`#cgo` directives automatically (the Go-native equivalent of every other
wish binding's `WISH_LIB` override):

```bash
export CGO_CFLAGS="-I/path/to/wish/include -I/path/to/wish/extern/bison/include"
export CGO_LDFLAGS="-L/path/to/wish/build -lwish_client -Wl,-rpath,/path/to/wish/build"
go build ./...
```

Requires `CGO_ENABLED=1` (the default) and a C compiler on `PATH`.

## Quick start

```go
client, _ := wish.NewTCPClient("127.0.0.1", 7070)
defer client.Destroy()

err := client.Run(func(c *wish.Client) {
    c.SetStylePreset("dark")
    c.RegisterTemplate("ui", `{"type":"Window","title":"Hi",
        "children":{"label":{"type":"Label","text":""}}}`)
    root, _ := c.InstantiateTemplate("ui", "ui")
    defer root.Close()
    label, _ := c.ProxyGet("ui.label")
    defer label.Close()
    fields, _ := wish.NewValue()
    defer fields.Close()
    fields.SetString("text", "Hello from Go")
    label.Set(fields, -1)
})
```

`Proxy.OnEvent`'s handler may run on a different goroutine than the one
that registered it (the RMI worker thread delivering the push event), so
share state across handlers with a mutex or channel -- see
`examples/calculator_example/main.go` for the full pattern.

## Examples and tests

```bash
# Start a server first (a separate terminal; --renderer=console needs no display):
build/app/wish server --transport=tcp --port=7070 --renderer=console

go run ./examples/calculator_example -transport=tcp -host=127.0.0.1 -port=7070

go test ./...
```

## Hosting a server (`wishserver/`)

```bash
# Build the server shared library (a non-default CMake option):
cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
cmake --build build --target wish_server_dll

cd bindings/go
go run ./examples/basic_server_example -transport=tcp -port=7070 -renderer=console
go test ./wishserver/
```

```go
server, _ := wishserver.NewTCPServer("127.0.0.1", 7070)
defer server.Destroy()

params := wishserver.NewParams().SetString("title", "My App").SetInt("width", 1280)
defer params.Close()
server.Start("sdl3", params)   // "sdl3", "web", or "console"
for !server.ShouldQuit() {
    time.Sleep(50 * time.Millisecond)
}
server.Stop()
```

`wishserver/native.go`'s `#cgo` directives resolve `libwish_server` from
`build/` by default; override with `CGO_CFLAGS`/`CGO_LDFLAGS` (using
`-lwish_server`) the same way the client package documents. See
[docs/bindings.md](../../docs/bindings.md#running-a-server-from-go).

Full binding documentation (API surface, error handling, transports) lives
in [docs/bindings.md](../../docs/bindings.md#go-bindingsgo). Proxies and
futures wrap the same `rmi_proxy_handle`/`rmi_future_handle` primitives
bison's own Go binding does -- see
[extern/bison/docs/bindings.md](../../extern/bison/docs/bindings.md) for
more on that layer.
