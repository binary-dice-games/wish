# wish

A remote UI framework built on [bison](https://github.com/binary-dice-games/bison). A wish server hosts an imgui rendering loop and exposes a set of UI element classes over bison RMI. Client applications connect, instantiate UI objects, set properties, and receive user-interaction events — all without owning a window or a graphics context.

## Architecture

```
Client App
  └─ wish client lib (bdg::wish::client)
       └─ bison RMI transport (TCP socket or PTY)
            └─ wish server (bdg::wish::server)
                 ├─ bison RMI server  <- object create / set / get / call / event
                 └─ imgui renderer    <- traverses object tree each frame
```

- The **server** registers all built-in UI element classes in the `"wish"` bison namespace and drives an imgui frame loop.
- The **client** connects via a bison transport, instantiates UI objects, and links them into a parent-child hierarchy by setting the `children` field.
- **Properties** are synchronized by calling `proxy.set(fields)` from the client; the server's `__setter` hook marks the object dirty so the next frame picks up the change.
- **Events** (button clicks, slider drags, etc.) flow from the server to the client via `proxy.onEvent(name, handler)`.

## Quick Build

```sh
cmake -S . -B build
cmake --build build
```

See [docs/building.md](docs/building.md) for prerequisites, options, and platform notes.

## Quick Start

### Server side — register a custom panel

```cpp
#include <wish/wish.hpp>

// bdg::wish::server already registers all built-in UI classes.
// Extend with application-specific classes before calling run().
class my_server : public bdg::wish::server {
protected:
  void register_classes() override {
    bdg::wish::server::register_classes();  // Window, Button, Label, ...

    auto proto = bison::dynamic_ptr{"MyPanel"_key, {}};
    proto->addField("title"_key, bison::field{std::string{"Panel"}});
    proto->addMethod("reset"_key,
      [](bison::dynamic& self, const bison::dynamic&) -> bison::dynamic {
        self["title"_key] = std::string{"Panel"};
        return {};
      });
    bison::dynamic::addClass("wish"_key, proto, 0U);
  }
};

int main(int argc, char** argv) {
  return my_server{}.run(argc, argv);
}
```

### Client side — create a window with a button

```cpp
#include <wish/wish.hpp>

class my_client : public bdg::wish::client {
protected:
  int on_session(bison::rmi::client& c) override {
    auto win = c.instantiate("wish"_key, "Window"_key).get();
    win.set({{"width"_key, 400}, {"height"_key, 300},
             {"title"_key, std::string{"Hello"}}}).get();

    auto btn = c.instantiate("wish"_key, "Button"_key).get();
    btn.set({{"label"_key, std::string{"OK"}}}).get();

    // Link button into the window's children map.
    bison::dynamic children;
    children[0U] = btn.id();
    win.set({{"children"_key, children}}).get();

    btn.onEvent("clicked"_key, [](bison::dynamic) {
      std::cout << "Button clicked\n";
    });

    std::string line;
    std::getline(std::cin, line);  // keep session alive
    return 0;
  }
};

int main(int argc, char** argv) {
  return my_client{}.run(argc, argv);
}
```

## Core Concepts

| Concept | Description |
|---------|-------------|
| **Object hierarchy** | UI elements are bison `dynamic` objects linked via indexed `children` fields, forming a tree rooted at a `Window`. |
| **Remote properties** | `proxy.set(fields)` / `proxy.get()` synchronize typed fields between client and server. Partial updates and projected `get()` calls are supported. |
| **Events** | Server-side interactions emit named events (`clicked`, `changed`, ...) delivered to the client via `proxy.onEvent`. |
| **Renderer backends** | The initial backend is imgui. The renderer dispatches each node by its `__class` field. A new backend is a new `wish::renderer` subclass. |
| **Transports** | PTY (local, default) or TCP socket (network). Chosen at runtime via server/client launch parameters. |

## Further Documentation

| File | Contents |
|------|----------|
| [docs/building.md](docs/building.md) | Prerequisites, CMake options, platform notes |
| [docs/examples.md](docs/examples.md) | Annotated example walkthroughs |
