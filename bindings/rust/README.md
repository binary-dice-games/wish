# wish

Rust bindings for [wish](https://github.com/binary-dice-games/wish):

- **`wish`** (this crate) wraps the **client** C ABI (`wish_client_c.h`) --
  connect to a running `wish server` process and drive UI templates from
  Rust.
- **`wish-server`** ([`wish-server/`](wish-server/)) wraps the **server** C
  ABI (`wish_server_c.h`) -- host and render a real wish session from Rust,
  the same `bdg::wish::server` implementation the `wish server` CLI uses.

They are **separate crates on purpose**: `libwish_client` and
`libwish_server` both export the `bison_*`/`rmi_*` C ABI symbols, so a single
binary must link exactly one of them (the same constraint `bindings/cpp/`'s
two targets and the Python binding's two `ctypes` modules carry). Depend on
`wish` to drive a UI, on `wish-server` to host one.

Both crates link directly against their precompiled shared library at build
time (via `build.rs`) -- the same model `bindings/cpp/` uses. Neither is
published to crates.io; use them from a checkout of this repository.

## Build

Build `wish_client_dll` first (from the repo root):

```bash
cmake -B build -DWISH_BUILD_SHARED=ON
cmake --build build --target wish_client_dll
```

Then build the crate:

```bash
cd bindings/rust
cargo build
```

`build.rs` looks for `wish_client` in this order: the `WISH_LIB` environment
variable (a full path to `libwish_client.so`/`.dylib`/`wish_client.dll`),
then a sibling `build/` directory next to `bindings/rust/`, then the system
library search path. Set `WISH_LIB` explicitly if the library isn't found:

```bash
export WISH_LIB=$(pwd)/../../build/libwish_client.so   # Linux, from bindings/rust
```

## Quick start

```rust
use wish::{Client, Value};

let client = Client::tcp("127.0.0.1", 7070);
client.run(|c| {
    c.set_style_preset("dark").unwrap();
    c.register_template("ui", r#"{
        "type": "Window", "title": "Hi",
        "children": { "label": { "type": "Label", "text": "" } }
    }"#).unwrap();
    let _root = c.instantiate_template("ui", "ui").unwrap();
    let label = c.proxy_get("ui.label").unwrap();
    let mut fields = Value::new();
    fields.set_string("text", "Hello from Rust").unwrap();
    label.set(&fields, -1).unwrap();
}).unwrap();
```

Event handlers subscribe via `Proxy::on_event`, whose closure must be
`'static` (Rust's FFI-callback requirement) -- use `Client::quit_handle()`
for a cheap, `Send`/`Sync` token to call `quit()` from inside one, and
`Arc`/`Arc<Mutex<_>>` to share proxies/state across handlers. See
`examples/calculator.rs` for the full pattern.

## Examples and tests

```bash
# Start a server first (a separate terminal; --renderer=console needs no display):
build/app/wish server --transport=tcp --port=7070 --renderer=console

cargo run --example calculator -- --transport=tcp --host=127.0.0.1 --port=7070

cargo test
```

## Hosting a server (`wish-server/`)

```bash
# Build the server shared library (a non-default CMake option):
cmake -B build -DWISH_BUILD_SERVER_SHARED=ON
cmake --build build --target wish_server_dll

cd bindings/rust
cargo run -p wish-server --example basic_server -- --transport=tcp --port=7070 --renderer=console
cargo test -p wish-server
```

```rust
use wish_server::{Params, Server};

let mut server = Server::tcp("127.0.0.1", 7070);
server.start("sdl3", Some(&Params::new().string("title", "My App").int("width", 1280))).unwrap();
while !server.should_quit() {
    std::thread::sleep(std::time::Duration::from_millis(50));
}
server.stop().unwrap();
```

`build.rs` locates `libwish_server` via `WISH_SERVER_LIB` (or the sibling
`build/` directory), the server-side counterpart of the client crate's
`WISH_LIB`. `renderer` is `"sdl3"`, `"web"`, or `"console"`; see
[docs/bindings.md](../../docs/bindings.md#running-a-server-from-rust).

Full binding documentation (API surface, error handling, transports) lives
in [docs/bindings.md](../../docs/bindings.md#rust-bindingsrust). Proxies and
futures wrap the same `rmi_proxy_handle`/`rmi_future_handle` primitives
bison's own Rust binding does -- see
[extern/bison/docs/bindings.md](../../extern/bison/docs/bindings.md) for
more on that layer.
