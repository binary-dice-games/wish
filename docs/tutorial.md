# Tutorial: Getting Started with wish

This is a beginner-friendly, example-driven walkthrough of wish. It assumes
you've already built the project — see [building.md](building.md) if not —
and that you're at least loosely familiar with
[bison](https://github.com/binary-dice-games/bison) (wish's transport,
serialization, and RMI layer). Every snippet below is drawn from, or
directly adapted from, `examples/calculator/main.cpp` and
`examples/demo/main.cpp`, which you can build and run to see this same
material executed:

```sh
cmake -S . -B build
cmake --build build --target calculator
./build/examples/calculator          # MSYS2: ./build/examples/calculator.exe
```

All public symbols live in namespace `bdg::wish`; the snippets below assume
`using namespace bdg::bison;` and `namespace wish = bdg::wish;` unless noted
otherwise.

## Table of contents

1. [Your first wish app](#1-your-first-wish-app)
2. [Anatomy of a wish app](#2-anatomy-of-a-wish-app)
3. [Building UI with templates](#3-building-ui-with-templates)
4. [Building UI by hand](#4-building-ui-by-hand)
5. [Remote properties](#5-remote-properties)
6. [Events](#6-events)
7. [Layouts](#7-layouts)
8. [The file service](#8-the-file-service)
9. [Style and themes](#9-style-and-themes)
10. [Running client and server as separate processes](#10-running-client-and-server-as-separate-processes)
11. [Where to go next](#11-where-to-go-next)

## 1. Your first wish app

Run the calculator example — a self-contained, four-function calculator
that renders in a real SDL3 window:

```sh
./build/examples/calculator
```

Click around, then close the window; the process exits cleanly. Everything
you clicked was a wish `Button`, `Label`, and `HorizontalLayout` element,
driven by a client that never touched a pixel itself. The rest of this
tutorial builds up the pieces that make that possible.

## 2. Anatomy of a wish app

A wish app has two halves that talk bison RMI to each other:

- A **server** (`wish::server`) that owns a `wish::renderer` (a real window,
  or a browser endpoint) and a registry of UI element classes. It renders
  whatever object tree its connected clients build.
- A **client** (`wish::client`) that builds the object tree, reacts to
  events, and contains all the application logic. It never touches
  rendering directly.

The calculator example wires both up in one process over an in-memory
transport (no sockets, no serialization overhead) — the simplest possible
setup, and a good one to learn the API against before splitting client and
server into separate processes (§10):

```cpp
#include "server/server.hpp"
#include "sdl/sdl3_renderer.hpp"
#include "client/client.hpp"
#include "src/rmi/rmi.hpp"   // memory_server_transport / memory_client_transport

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

class my_client : public wish::client {
 public:
  using wish::client::client; // inherit the transport-taking constructors

 protected:
  void on_session() override {
    // ... build UI, wire events (see §3 onward) ...
  }
};

int main() {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::sdl3_renderer>("Hello", 400, 300, 16)};
  srv.start();

  my_client client{transport.connect()};
  client.run();   // connect() -> on_session() -> disconnect()

  srv.stop();
}
```

`wish::server::start()` spawns its render loop and RMI accept loop on
background threads and returns immediately; `client::run()` blocks for the
duration of `on_session()`. Every UI-building call from here on happens
inside `on_session()`, on `this` (a `wish::client`).

## 3. Building UI with templates

The fastest way to describe a UI is as data: a JSON (or YAML) tree of typed
nodes, registered once by name and instantiated as many times as you like.
This is what the calculator example does:

```cpp
static constexpr const char* kCalcDesc = R"({
  "type": "Window", "title": "Calculator", "width": 328, "height": 420,
  "children": {
    "display": { "type": "Label", "text": "0" },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",  "width": 72, "height": 52 },
        "div": { "type": "Button", "label": "/",  "width": 72, "height": 52 }
      }
    }
  }
})";

void on_session() override {
  register_template_from_json("calc"_key, kCalcDesc).get();

  // proxy_map: dot-path name -> proxy, one entry per named node in the tree.
  auto pm = instantiate_template("calc"_key).get();
  auto& display = pm.at("display");
  auto& clear_btn = pm.at("row0.c");
}
```

`register_template_from_json` parses the JSON client-side into a
`bison::dynamic` descriptor and sends it to the server in one call;
`register_template(name, dynamic)` is the lower-level form for a descriptor
you built or transformed yourself, and `register_template_from_yaml` is the
YAML equivalent. `instantiate_template(name)` asks the server to build a
fresh copy of the registered tree and returns a
`proxy_map` — `std::unordered_map<std::string, proxy::dynamic>` keyed by the
dot-joined path of every *named* node (`"row0.c"`, not `"row0"` — unnamed
structural nodes don't get their own map entry, but their named descendants
still do). Instantiating the same template again returns an independent
tree with a fresh set of proxies; the server keeps them isolated per call.

## 4. Building UI by hand

For a UI you're constructing programmatically (from external data, or that
changes shape at runtime) skip templates and instantiate element classes
directly. This is the same `dynamic::instantiate` pattern any bison RMI
class uses, in the `"wish"` namespace:

```cpp
auto win = instantiate("wish"_key, "Window"_key).get();
win.set({{"title"_key, std::string{"Hello"}}, {"width"_key, 400}}).get();

auto btn = instantiate("wish"_key, "Button"_key).get();
btn.set({{"label"_key, std::string{"OK"}}}).get();

// Attach btn as a named child of win: children is a dynamic mapping names
// to child object-id tokens.
bison::dynamic children;
children["ok"_key] = btn.object_id();
win.set({{"children"_key, children}}).get();
```

`instantiate` is inherited from `bison::rmi::client`, not wish-specific — it
takes a namespace key (`"wish"_key` for every built-in element class), a
class key, and an optional params `dynamic` (defaulted, so 2-argument calls
like the ones above are fine). `object_id()` returns the `bison::key_t`
token a `children` field expects; `id()` returns the same identifier as a
plain `uint64_t`, useful for logging but not assignable into a `dynamic`
field directly.

## 5. Remote properties

Every UI element is a bison RMI object: `proxy.set(fields)` pushes a partial
field update (unspecified fields are left alone), and `proxy.get()` reads
a full snapshot back:

```cpp
display.set({{"text"_key, std::string{"42"}}});   // no .get() needed for a
                                                    // one-way visual update
bison::dynamic snapshot = display.get().get();
std::string current_text = snapshot["text"_key].as<std::string>();
```

`set()` returns `std::future<bool>` — calling code typically doesn't wait on
it (property sets are meant to be fire-and-forget for low-latency visual
updates), but `.get()` is there if you need to confirm the round trip.
`get(projection)` fetches only the fields named in `projection`, GraphQL-style,
when you don't want the whole object.

## 6. Events

Interactive elements (`Button`, `Checkbox`, `SliderFloat`/`SliderInt`,
`InputText`, and more) emit named events when the user interacts with them
server-side; `onEvent` subscribes a client-side handler:

```cpp
pm.at("row0.c").onEvent("clicked"_key, [this](bison::dynamic /*payload*/) {
  display_ = "0";
  pm.at("display").set({{"text"_key, display_}});
});
```

Handlers run serially on the client's worker thread (or in-process for the
in-memory transport), so ordering across events is preserved; an exception
thrown by a handler is caught and discarded rather than propagating. Capture
by reference is safe here only as long as the captured objects (`pm`,
`this`) outlive the session — see how `examples/calculator/main.cpp`
structures its member state for a pattern that avoids dangling captures.

## 7. Layouts

`VerticalLayout` and `HorizontalLayout` are ordinary `Element` nodes whose
only job is arranging their children; nest them to build grids:

```json
{
  "type": "VerticalLayout", "spacing": 4,
  "children": {
    "row1": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "r1c1": { "type": "Button", "label": "R1 C1" },
        "r1c2": { "type": "Button", "label": "R1 C2" }
      }
    },
    "row2": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "r2c1": { "type": "Button", "label": "R2 C1" },
        "r2c2": { "type": "Button", "label": "R2 C2" }
      }
    }
  }
}
```

Layouts compose with every other element type, including other layouts, so
row-of-columns and column-of-rows grids are both just nesting. See the
"Layouts" section of `examples/demo/main.cpp`'s `kTabBasicsDesc` for the
full runnable version of the snippet above, and
[docs/examples.md](examples.md#demo-examplesdemo) for the complete widget
catalog (tables, tabs, trees, 2D/3D plots, and more) the `demo` example
exercises tab by tab.

## 8. The file service

Clients can upload resources (images, fonts, arbitrary files) into a
sandboxed, per-session server-side directory and reference them by name:

```cpp
upload_file("logo.png", file_bytes).get();

auto img = instantiate("wish"_key, "Image"_key).get();
img.set({{"src"_key, std::string{"logo.png"}}, {"width"_key, 64}, {"height"_key, 64}}).get();
```

`upload_file`/`download_file` also have streamed overloads
(`std::istream&`/`std::ostream&`, chunked internally) for large files
without buffering the whole thing in memory, and `upload_package(dest_path,
zip_stream)` uploads a zip archive and has the server unpack it into
`dest_path` inside the sandbox. The directory is deleted when the client
disconnects unless the server was configured with a persistent, identity-keyed
sandbox — see [DESIGN.md](../DESIGN.md#bdgwishfile_service) and
`src/auth/DESIGN.md` for that setup and the sandboxing guarantees.

## 9. Style and themes

`set_style_preset` applies one of the three built-in themes and resets any
per-field overrides to that theme's baseline; `set_style` layers scalar or
color overrides on top:

```cpp
set_style_preset("dark").get();

bison::dynamic overrides;
overrides["window_rounding"_key] = 6.0f;
overrides["frame_rounding"_key] = 4.0f;
set_style(std::move(overrides)).get();
```

Color fields (e.g. `"color_button"_key`) take a `"#RRGGBBAA"` hex string
instead of a float. Call `set_style_preset` again before re-applying
overrides if you want to reset to a clean baseline rather than layering on
top of the previous overrides.

## 10. Running client and server as separate processes

Everything above ran over an in-memory transport in one process. The same
client code works unchanged against a real `wish server` in a second
process, or on another machine — only the transport changes. First, start a
server listening on TCP:

```sh
./build/app/wish server --transport tcp --port 7070 --renderer sdl3
```

Then point a client at it with `socket_client_transport` instead of
`memory_client_transport` — everything from `my_client` in §2 onward is
unchanged:

```cpp
#include "src/rmi/transport/socket_transport.hpp"

int main() {
  my_client client{bison::rmi::transport::socket_client_transport{"127.0.0.1", 7070}};
  client.run();
}
```

`--transport` also accepts `pipe` (named pipe / Unix socket, via `--name`)
and `term` (an interactive terminal hop the server spawns and the client
runs inside — `wish server`'s own default). See
[docs/cli.md](cli.md) for the full flag reference (including `wish
client`/`wish standalone`/`wish desktop`), [docs/bindings.md](bindings.md)
for connecting from Python or C#, and `wish client --list` /
`modules/README.md` for the separate module
system that lets a pre-built client binary run a named app by string
(`wish client --run=<name>`) instead of linking your own `main()`.

## 11. Where to go next

- [README.md](../README.md) — feature overview and a compact API reference.
- [docs/examples.md](examples.md) — building and running the calculator and
  demo examples, including the full demo widget catalog.
- [docs/building.md](building.md) — platform setup, CMake options, and the
  full `wish` CLI flag reference.
- [docs/bindings.md](bindings.md) — using wish from Python or C#, and
  driving a running UI with the automation module.
- [DESIGN.md](../DESIGN.md) — architecture, the object model, and the
  step-by-step pattern for registering a new server-side UI element class.
- [bison's own tutorial](https://github.com/binary-dice-games/bison/blob/main/docs/tutorial.md) —
  the `dynamic`/RMI concepts (keys, fields, proxies, futures) that
  everything above is built on.
