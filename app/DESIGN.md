# Wish CLI Design

## Overview

`wish` is a single binary with three subcommands:

```
wish server  [transport flags] [window flags]    — render server (current behaviour)
wish client  [transport flags] --run <app>        — connect and run an embedded app
wish bridge  [upstream flags] [downstream flags]  — multiplex + desktop chrome
```

Shared transport flags (all modes):

| Flag | Description |
|------|-------------|
| `--host`, `--port` | TCP socket (default: `0.0.0.0:7070` for server, `127.0.0.1:7070` for client) |
| `--pipe=<path>` | Named pipe / Unix socket |
| `--verbose` | Trace RMI messages |

`bridge` mode adds prefixed variants: `--up-host`, `--up-port`, `--up-pipe`
(upstream) and `--down-host`, `--down-port`, `--down-pipe` (downstream).

---

## Subcommand dispatch

`app/wish_cli/main.cpp` reads `argv[1]` and dispatches:

```
"server" → wish_server_app::run(argc-1, argv+1)
"client" → wish_client_app::run(argc-1, argv+1)
"bridge" → wish_bridge_app::run(argc-1, argv+1)
else     → print_usage(), exit(1)
```

Shared transport flags (`--host`, `--port`, `--pipe`, `--verbose`) are
defined in `main.cpp` so that `bison::app::server_app` can DECLARE them as usual.
Mode-specific flags are defined in each mode's `.cpp` file.

---

## server mode

**Class**: `wish_server_app` (renamed from `wish_server`)
**Base**: `bison::app::server_app`
**Sources**: `app/wish_cli/server/wish_server_app.hpp/.cpp`

Behaviour is identical to the original implementation. The SDL3 renderer,
dockspace, menu bar, and session restart logic are unchanged.

---

## client mode

**Class**: `wish_client_session` (local to `wish_client_app.cpp`)
**Base**: `wish::client`
**Sources**: `app/wish_cli/client/wish_client_app.hpp/.cpp`

### Flags

| Flag | Description |
|------|-------------|
| `--list` | Print available apps and exit |
| `--describe=<name>` | Print an app's name, description, and parameters and exit |
| `--run=<name>` | Launch named app (e.g. `calculator`) |
| `--timeout=<ms>` | Connection timeout in milliseconds (default: 30000) |

Transport flags `--host`, `--port`, `--pipe` are shared with server mode.

### App registry

A static map `{ name → AppFn }` lists all embedded apps:

```cpp
using AppFn = std::function<void(wish_client_session&)>;
static const std::map<std::string, AppFn> kApps = {
    {"calculator", run_calculator},
};
```

`--list` prints the keys. `--run=<name>` looks up and calls the function inside
`on_session()`.

### Lifecycle

```
wish_client_app::run()
  parse flags
  if --list: print app names, return 0
  build transport (pipe > tcp)
  create wish_client_session, connect, call run()
    on_session()
      call kApps[--run](session)
      block until app signals done (via session.signal_done())
  disconnect
```

### Done signalling

`wish_client_session` holds a `std::promise<void> done_`. App functions call
`session.signal_done()` to unblock `on_session()`. For the calculator this is
triggered by registering a `"closed"` event handler on the Calculator form proxy.

---

## Calculator app (proof of concept)

### Server-side form: `src/forms/calculator.hpp/.cpp`

Extends `wish::form`. Registered as class `"Calculator"` in namespace `"wish"`.
Manages its own UI tree:

```
Window (title: "Calculator", 265×370)
  VerticalLayout
    Label     (id: display, text: "0")
    HLayout   [C] [+/-] [%] [<]       ← clear / negate / percent / backspace
    HLayout   [7] [8] [9] [/]
    HLayout   [4] [5] [6] [*]
    HLayout   [1] [2] [3] [-]
    HLayout   [0] [.] [=] [+]
    HLayout   [     Close     ]
```

All button-click events are handled server-side in `on_event()`. State:
`display_` (string), `operand_` (double), `pending_op_` (char), `has_result_`
(bool). The "Close" button calls `emit("closed"_key)` then
`remove_internal_objects()`.

Registration: `register_calculator()` called from `register_all()` in
`src/registry.cpp`.

### Client-side runner: `app/wish_cli/client/apps/calculator.hpp/.cpp`

```cpp
void run_calculator(wish_client_session& s) {
    auto proxy = s.instantiate("wish"_key, "Calculator"_key).get();
    proxy.onEvent("closed"_key, [&s](bison::dynamic) { s.signal_done(); });
    s.keep_alive(std::move(proxy));
}
```

The client just instantiates the form, registers the "closed" event handler,
and returns. The session blocks in `on_session()` until `signal_done()` fires.

---

## bridge mode

**Class**: `wish_bridge_app` (extends `rmi::bridge`)
**Sources**: `app/wish_cli/bridge/wish_bridge_app.hpp/.cpp`

### Required bison change

`rmi::bridge::upstream_client_` is private. A one-line protected accessor is
added to `bridge.hpp` so subclasses can reach the upstream RMI client:

```cpp
protected:
  rmi::client& upstream() { return upstream_client_; }
```

### Flags

| Group | Flags |
|-------|-------|
| Upstream | `--up-host` (127.0.0.1), `--up-port` (7070), `--up-pipe` ("") |
| Downstream | `--down-host` (0.0.0.0), `--down-port` (7071), `--down-pipe` ("") |
| Common | `--verbose` (shared, DECLARE only) |

### Desktop chrome

`wish_bridge_app` overrides `on_client_connected` and `on_client_disconnected`
to maintain a Window on the upstream session that shows the connected client
count. On first connect the Window is created via `upstream().instantiate(...)`;
subsequent connects/disconnects update its `title` field.

### Lifecycle

```
wish_bridge_app::run()
  parse flags
  build upstream transport (up-pipe > up-tcp)
  build downstream server transport (down-pipe > down-tcp)
  bridge::start()   → connects upstream; starts downstream listen loop
  block until SIGINT
  bridge::stop()
```

---

## File layout

```
app/
  DESIGN.md                              ← this document
  CMakeLists.txt                         ← updated
  wish_cli/
    main.cpp                             ← subcommand dispatcher + shared flag DEFINE
    server/
      wish_server_app.hpp                ← renamed from wish_server/wish_server.hpp
      wish_server_app.cpp
    client/
      wish_client_app.hpp
      wish_client_app.cpp
      apps/
        calculator.hpp
        calculator.cpp
    bridge/
      wish_bridge_app.hpp
      wish_bridge_app.cpp

src/
  forms/
    calculator.hpp                       ← NEW server-side form
    calculator.cpp
```

Old `app/wish_server/` directory is removed.

---

## Build

`app/CMakeLists.txt` renames the target and lists new sources:

```cmake
add_executable(wish-cli
  wish_cli/main.cpp
  wish_cli/server/wish_server_app.cpp
  wish_cli/client/wish_client_app.cpp
  wish_cli/client/apps/calculator.cpp
  wish_cli/bridge/wish_bridge_app.cpp
)
set_target_properties(wish-cli PROPERTIES OUTPUT_NAME "wish")
target_link_libraries(wish-cli PRIVATE wish gflags)
```

`CMakeLists.txt` (root) adds `src/forms/calculator.cpp` to `wish_server` sources.

---

## Key invariants

1. Calculator form is in `wish_server` static lib — linked by both server and
   client modes.
2. `rmi::bridge` (bison) is only extended, not modified beyond the one-line
   `upstream()` accessor.
3. Shared transport flags are defined exactly once, in `main.cpp`.
4. Client mode has no SDL renderer dependency; it links `wish_client` only
   (transitively via `wish`).
5. Bridge desktop chrome uses `upstream().instantiate(...)` directly — no
   `wish::client` wrapper needed for basic wish classes.
