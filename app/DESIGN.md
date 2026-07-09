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

`bridge` mode reuses the shared transport flags for its downstream side and
adds `--upstream_transport`, `--upstream_host`, `--upstream_port`,
`--upstream_name` for its upstream side (see "bridge mode" below).

### Factory-method pattern (client/server/standalone/bridge)

Each of the four `bison::app::*_app` scaffolds has a virtual factory method
(`make_client()`, `make_server()`, `make_standalone()`, `make_bridge()`) that
constructs the RMI object the app's lifecycle drives. Wish overrides the
relevant factory in each of its four app classes to construct its own
session/server/bridge type (`wish::client`, `wish::server`,
`wish_standalone_session`, `wish_bridge`) instead of the generic bison type,
so hooks like `on_session()` can `static_cast` to the wish-specific type and
call wish-level methods (`upload_file()`, `set_style()`, etc.) without
duplicating the base class's connect/run/disconnect lifecycle. `server_app`
and `standalone_app` additionally needed small hook seams
(`server_app::run_with_transport()` already existed; `standalone_app` gained
`open_session()`/`close_session()`) because `wish::server`/`wish::standalone`
replace `listen()`/`connect()` with their own `start()` that also spins up a
render thread.

---

## Subcommand dispatch

`app/wish_cli/main.cpp` reads `argv[1]` and dispatches:

```
"server"     → wish_server_app{}.run(argc-1, argv+1)
"client"     → wish_client_app{}.run(argc-1, argv+1)
"standalone" → run_standalone_mode(argc-1, argv+1)   (wraps wish_standalone_app{}.run(...))
"bridge"     → wish_bridge_app{}.run(argc-1, argv+1)
else         → print_usage(), exit(1)
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
`src/server/registry.cpp`.

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

**Classes**: `wish_bridge` (extends `bison::rmi::bridge`, desktop chrome) and
`wish_bridge_app` (extends `bison::app::bridge_app`, CLI scaffold)
**Sources**: `app/wish_cli/bridge/wish_bridge_app.hpp/.cpp`

`bison::rmi::bridge` already exposes a protected `upstream()` accessor
(`rmi::client& upstream() { return upstream_client_; }`) that subclasses use
to reach the upstream RMI client -- no bison change needed for this.
`wish_bridge_app::make_bridge()` overrides `bison::app::bridge_app`'s factory
to construct a `wish_bridge` instead of the generic internal bridge, so
`wish_bridge`'s own `on_client_connected`/`on_client_disconnected` overrides
(which call `upstream()`) run.

### Flags

| Group | Flags |
|-------|-------|
| Downstream | shared with server mode: `--transport`, `--host` (0.0.0.0), `--port` (7070), `--name`, `--cmd` |
| Upstream | `--upstream_transport` (term), `--upstream_host` (127.0.0.1), `--upstream_port` (7070), `--upstream_name` |
| Common | `--timeout` (shared with client mode), `--verbose`, `--debugger` |

### Desktop chrome

`wish_bridge` overrides `on_client_connected` and `on_client_disconnected`
to maintain a Window on the upstream session that shows the connected client
count. On first connect the Window is created via `upstream().instantiate(...)`;
subsequent connects/disconnects update its `title` field.

### Lifecycle

```
wish_bridge_app::run()
  parse flags
  build upstream transport (bison::app::bridge_app::run())
  build downstream server transport
  make_bridge()      → constructs a wish_bridge
  bridge::start()    → connects upstream; starts downstream listen loop
  block until Enter is pressed (or the spawned terminal exits, --transport=term)
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
2. `rmi::bridge` (bison) is extended (`wish_bridge`), not modified, aside from
   the pre-existing `upstream()` accessor.
3. Shared transport flags (`--transport`/`--host`/`--port`/`--name`/`--cmd`/
   `--verbose`/`--debugger`/`--timeout`) are each defined exactly once: in
   `main.cpp` for the combined `wish-cli` binary, or in the relevant
   `standalone_main.cpp` for each mode's standalone binary.
4. Client mode has no SDL renderer dependency; it links `wish_client` only
   (transitively via `wish`).
5. Bridge desktop chrome uses `upstream().instantiate(...)` directly — no
   `wish::client` wrapper needed for basic wish classes.
