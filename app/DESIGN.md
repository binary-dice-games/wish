# Wish CLI Design

## Overview

`wish` is a single binary with three subcommands:

```
wish server  [transport flags] [window flags]    — render server (current behaviour)
wish client  [transport flags] --run <app>        — connect and run an embedded app
wish desktop [upstream flags] [downstream flags]  — multiplex + desktop shell
```

Shared transport flags (all modes):

| Flag | Description |
|------|-------------|
| `--host`, `--port` | TCP socket (default: `0.0.0.0:7070` for server, `127.0.0.1:7070` for client) |
| `--pipe=<path>` | Named pipe / Unix socket |
| `--verbose=<level>` | Log verbosity: `none` (default) \| `fatal` \| `error` \| `warning` \| `info` \| `trace` (RMI trace lines at `info`+, decoded payloads at `trace`) |

`desktop` mode reuses the shared transport flags for its downstream side and
adds `--upstream_transport`, `--upstream_host`, `--upstream_port`,
`--upstream_name` for its upstream side (see "desktop mode" below).

### Factory-method pattern (client/server/standalone/desktop)

Each of the four `bison::app::*_app` scaffolds has a virtual factory method
(`make_client()`, `make_server()`, `make_standalone()`, `make_bridge()`) that
constructs the RMI object the app's lifecycle drives. Wish overrides the
relevant factory in each of its four app classes to construct its own
session/server/bridge type (`wish::client`, `wish::server`,
`wish_standalone_session`, `wish_desktop`) instead of the generic bison type,
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
"desktop"    → wish_desktop_app{}.run(argc-1, argv+1)
else         → print_usage(), exit(1)
```

Shared transport flags (`--host`, `--port`, `--pipe`, `--verbose`) are
defined in `main.cpp` so that `bison::app::server_app` can DECLARE them as usual.
Mode-specific flags are defined in each mode's `.cpp` file.

### Flag resolution order

Every flag resolves as command line > `WISH_<FLAG>` env var > saved default
> `DEFINE_*` default. `apply_env_flag_defaults()` (`env_flags.hpp`) and
`load_cli_config()` (`config_flags.hpp`, `server` and `standalone` only)
both write with `gflags::SET_FLAGS_DEFAULT`, so `is_default` keeps meaning
"not given on the command line". That is what lets the saved defaults skip
flags given on the command line or in the environment, lets
`--save_defaults` save only command-line flags, and keeps standalone's
transport-flag rejection working.

The saved defaults live in the server store (`<store_dir>/server_store.bison`,
entries `wish.cli.server` / `wish.cli.standalone`) rather than a config file,
so server-side code can read them like any other server-store entry. The
store is read after the command line is parsed, since `--store_dir` locates
it, and before `server_app::run()` builds the transport. `load_cli_config()`
closes its store instance before the server opens its own. `server_app::run()`
parses argv again, so `wish_server_app::run()` parses a copy first (gflags
strips argv in place). The saveable flags come from `gflags::GetAllFlags()`
minus an exclusion list, so a new flag is saveable with no extra
registration: `config_flag_names()` always leaves out the store's own flags,
gflags' built-ins and secrets (`*password`, `*_pem`), and each mode adds its
own exclusions (debug/security switches; for standalone also the app
selection and transport flags). An exclusion list was preferred to an
allow list because forgetting to add a new flag to an allow list fails
silently.

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
| `--run=<name>` | Launch named app (e.g. `bc`) |
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

`on_session()` also calls `signal_done()` if the server closes the connection
first (via `wish::client::set_on_disconnected()`, registered at the top of
`wish_client_app::on_session()`) -- otherwise a `"closed"` event that would
have triggered it can never arrive, and the process hangs forever after the
server exits instead of unblocking `done_future_.wait()` and exiting cleanly.
The C ABI's `wish_client_wait()`/`wish_run_app()` mirror this via
`c_abi_client::on_disconnect()` in `src/wish_client_c.cpp`.

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

## desktop mode

**Classes**: `wish_desktop` (extends `bison::rmi::bridge`, desktop shell),
`wish_desktop_app` (extends `bison::app::bridge_app`, CLI scaffold) and
`process_launcher` (starts the apps launched from the desktop)
**Sources**: `app/wish_cli/desktop/wish_desktop_app.hpp/.cpp`,
`app/wish_cli/desktop/process_launcher.hpp/.cpp`; the chrome itself is drawn
by the server: `src/ui/ui_elements/desktop_shell.cpp` (classes) and
`src/imgui/imgui_desktop_shell.hpp/.cpp` (rendering)

`wish desktop` multiplexes downstream clients into one upstream wish server
session and turns that server's host window into a Windows/macOS-like
desktop: wallpaper, desktop icons, and a bottom taskbar with a Start menu,
pinned apps, open windows and a tray (connected apps, theme toggle, clock
with a calendar, show-desktop strip). See [docs/cli.md](../docs/cli.md#wish-desktop)
for the user-facing behavior.

### Who does what

```
wish desktop (bridge process)                 wish server (upstream)
──────────────────────────────                ─────────────────────────────────
wish_desktop::build_chrome()                  DesktopShell top-level object
  registered_apps() ──► DesktopApp children     │ render_server_frame():
  user store "wish.desktop" (pins, theme)       │  host_renderer → render_desktop_shell()
  shell events ◄──────────────────────────────── │  launch / pin / desktop_icon / theme / quit
  launch_app() ─► process_launcher (libuv)      │
        │                                       │
        └─► wish client --run=<app> ──► downstream ──► relayed upstream (group = session id)
```

The split follows where the information lives:

- **The server draws everything.** Only the renderer knows the open windows,
  their focus and their z-order, so the taskbar, Start menu, switcher and
  minimize/raise are server-side (`render_desktop_shell()`, called from
  `host_renderer::render_server_frame()` before the sessions render). The
  `DesktopShell` element carries only data (`wallpaper`, `user`, `clients`,
  clock formats) and `DesktopApp` children (`app`, `label`, `icon`,
  `pinned`, `on_desktop`, `sessions`); user actions come back as events.
- **The bridge owns policy and processes.** It knows the app registry (it
  is linked with the modules), the user's settings, and can spawn
  processes, so it builds the `DesktopShell` from `registered_apps()`,
  answers its events, and starts apps.
- **The server never changes `pinned`/`on_desktop` itself**: it emits the
  event and the bridge sets the field, so the bridge's saved settings and
  what is shown can't diverge.

### Which window belongs to which app

All downstream clients share the bridge's one upstream session, but
`rmi::bridge` tags every relayed request with the downstream session id as
its object group, and the upstream server files every object it creates
under that group (`bison::rmi::context::groups`, including objects created
indirectly by templates). Two pieces close the loop:

- `wish client --run=X` sends `"app": "<qualified name>"` in its connect
  params. `wish_desktop` overrides `try_handle_request()` to read it on
  `OP_CONNECT` (then lets the bridge handle the connect as usual) and
  `on_client_disconnected()` to forget it.
- It publishes each app's live session ids on `DesktopApp.sessions`. The
  renderer maps a window's `__wish_id` to its group, and the group to an
  app, for the taskbar icon and the running indicator under pinned apps.

An app instance whose group has a `DockSpaceViewport` (an app with its own
dock layout, e.g. nano, top, du) gets one taskbar entry for that frame,
standing for all of its top-level windows; otherwise every visible,
non-modal top-level `Window` is its own entry.

### Window behavior in a desktop

`render_desktop_shell()` calls `imgui_renderer::set_desktop_cascade_area()`
each frame, which switches two renderers into desktop behavior for that frame:

- `render_window()`: an un-positioned, dockable window without a
  `dock_target` that ImGui has never seen opens **floating at the next
  cascade slot** (`claim_cascade_slot()`) instead of docking into the host
  dockspace. The host dockspace is still there (passthrough central node),
  so dragging a window into it docks it.
- `render_dockspace_viewport()`: a `DockSpaceViewport` is an app's main
  frame. It is a titled, floating, cascaded window, never a fullscreen host
  covering the desktop.

Minimize is server-side render bookkeeping (`window::minimized()`,
`ui_dockspace_viewport::minimized()`; mutable members, not fields): a
minimized window is not submitted to ImGui at all, so ImGui keeps its
position, size and dock state for the restore. A minimized frame keeps its
dockspace alive (`ImGuiDockNodeFlags_KeepAliveOnly`) so its docked windows
stay docked. Raising uses `request_raise()` (`SetNextWindowFocus()` before
`Begin`), the server-side counterpart of a client's `focus_request`.
"Active" on the taskbar means most recently focused (`desktop_shell_state`
keeps an MRU list), not focused this frame, because clicking the taskbar
takes focus itself.

The chrome is drawn with the shell session's style (`imgui_renderer::
with_session_style()`/`session_style()`), so the tray's theme toggle (which
the bridge applies through `__WishStyle.preset` on that session) restyles
the chrome and every client window together.

### Locking

`render_desktop_shell()` locks each session once to copy out what it shows
(`shell_frame`), draws with no lock held, then locks each affected session
once more to apply what the user did (minimize/raise, a "closed" event, a
shell event). It never holds two session locks at once.

`wish_desktop`'s event handlers run on the upstream client's event thread,
which must not block on an upstream round trip (the response would be
dispatched by that same thread), so they post their work (proxy sets, user
store writes, `__WishStyle.preset`, launching) to a private worker thread.
`wish_desktop::flush()` waits for that worker, for tests.

### Launching apps

`process_launcher` runs a private libuv loop on its own thread (`uv_spawn`
for each child, `exit_cb` to reap it, `uv_process_kill` to terminate it,
`uv_exepath` for the program's own path), so it has no platform-specific
code. Each child gets this process's environment plus
`WISH_TRANSPORT/HOST/PORT/NAME` for the downstream side (a wildcard bind
host becomes loopback) and writes stdout/stderr to
`wish_logs/desktop/<app>_<n>.log`, never to the desktop's terminal. The
command is set by the entry point: `<wish> client --run=<app>` in the
combined `wish` binary, the sibling `wish-client --run=<app>` for the
standalone `wish-desktop`. `wish_desktop_app::wait_for_shutdown()`
terminates the children when the desktop exits. With
`--downstream_transport=term` nothing can dial in, so launching is
disabled.

### Settings

Saved in the upstream user store entry `wish.desktop`
(`{pinned: [...], desktop: [...], theme}`); the bridge connects upstream as
`--upstream_username` (default: the OS user name) so the session has a
store. Without a store (anonymous upstream), settings last for the run. The
first run pins nano, mc and top (those that are installed) and puts them on
the desktop. `--wallpaper FILE` is uploaded into the upstream sandbox at
`private/apps/wish.desktop/wallpaper.<ext>` and drawn "cover"-fit
(`imgui_renderer::texture_size()`).

### Lifecycle

```
wish_desktop_app::run()
  parse flags; export WISH_TRANSPORT/HOST/PORT/NAME for the anchor terminal
  bridge_app::run()
    make_bridge()     → wish_desktop, configured with launcher, client command,
                        launch environment, log dir, wallpaper, user
    bridge::start()   → connects upstream (with username); listens downstream
    on_listening()    → desktop_->build_chrome()
    wait_for_shutdown() → until "Quit desktop" or the anchor terminal exits;
                          then terminates launched apps
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
    env_flags.cpp                        ← WISH_<FLAG> env var defaults
    config_flags.cpp                     ← --save_defaults / saved defaults in the server store
    server/
      wish_server_app.hpp                ← renamed from wish_server/wish_server.hpp
      wish_server_app.cpp
    client/
      wish_client_app.hpp
      wish_client_app.cpp
      apps/
        calculator.hpp
        calculator.cpp
    desktop/
      wish_desktop_app.hpp
      wish_desktop_app.cpp
      process_launcher.hpp             ← starts apps launched from the desktop (libuv)
      process_launcher.cpp

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
  wish_cli/desktop/wish_desktop_app.cpp
)
set_target_properties(wish-cli PROPERTIES OUTPUT_NAME "wish")
target_link_libraries(wish-cli PRIVATE wish gflags)
```

`CMakeLists.txt` (root) adds `src/forms/calculator.cpp` to `wish_server` sources.

---

## Key invariants

1. Calculator form is in `wish_server` static lib — linked by both server and
   client modes.
2. `rmi::bridge` (bison) is extended (`wish_desktop`), not modified, aside from
   the pre-existing `upstream()` accessor.
3. Shared transport flags (`--transport`/`--host`/`--port`/`--name`/`--cmd`/
   `--verbose`/`--debugger`/`--timeout`) are each defined exactly once: in
   `main.cpp` for the combined `wish-cli` binary, or in the relevant
   `standalone_main.cpp` for each mode's standalone binary.
4. Client mode has no SDL renderer dependency; it links `wish_client` only
   (transitively via `wish`).
5. The desktop shell uses `upstream().instantiate(...)`/`upstream().make_proxy(...)`
   directly, including replicating the `__WishTemplate` register/instantiate
   protocol inline — no `wish::client` wrapper needed.
