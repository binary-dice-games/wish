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
`process_launcher` (starts the apps launched from the desktop); the
`desktop::` config parser (`load_config()`, `parse_start_menu()`)
**Sources**: `app/wish_cli/desktop/wish_desktop_app.hpp/.cpp`,
`app/wish_cli/desktop/process_launcher.hpp/.cpp`,
`app/wish_cli/desktop/desktop_config.hpp/.cpp`,
`app/wish_cli/desktop/multi_server_transport.hpp/.cpp`; the chrome itself is drawn
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
  desktop.yaml ──► DesktopMenu/DesktopApp tree  │ render_server_frame():
  user store "wish.desktop" (pins, theme)       │  host_renderer → render_desktop_shell()
  shell events ◄──────────────────────────────── │  activate / pin / desktop_icon / theme
  launch_entry() ─► process_launcher (libuv)    │
  run_action() ◄── __WishDesktop (downstream)   │
        │                                       │
        └─► wish client --run=<app> ──► downstream ──► relayed upstream (group = session id)
```

The split follows where the information lives:

- **The server draws everything.** Only the renderer knows the open windows,
  their focus and their z-order, so the taskbar, Start menu, switcher and
  minimize/raise are server-side (`render_desktop_shell()`, called from
  `host_renderer::render_server_frame()` before the sessions render). The
  `DesktopShell` element carries only data (`wallpaper`, `wallpaper_fit`,
  `background`, `show_icons`, `user`, `clients`, clock formats,
  `config_error`, `shutdown`) and the Start menu tree as
  children: `DesktopMenu` folders (one with `role: "power"` fills the power
  button), `Separator`s and `DesktopApp` entries (`id`, `app`, `label`,
  `icon`, `tooltip`, `pinned`, `on_desktop`, `sessions`). User actions come
  back as events naming the entry `id`; the server never knows what an
  entry does.
- **The bridge owns policy and processes.** It reads the config file, knows
  the app registry (it is linked with the modules, for `apps: all`), the
  user's settings, and can spawn processes, so it builds the `DesktopShell`
  from the config, answers its events, runs entries and starts apps.
- **The server never changes `pinned`/`on_desktop` itself**: it emits the
  event and the bridge sets the field, so the bridge's saved settings and
  what is shown can't diverge.

### The config file

The desktop's settings -- `appearance` (wallpaper, background, theme,
desktop icons), `taskbar` (clock formats) and `start_menu` -- are sections of
a per-user file,
`~/.wish/desktop/desktop.yaml` (`--config`; format in
[docs/desktop-config.md](../docs/desktop-config.md)). It is a plain file next
to the persistent stores (`default_store_dir()`, in `wish_client` so the
client-only `wish-desktop` can use it), not a store entry, so users can edit
it with any editor, and the server store stays unreachable over RMI. The
top level is a map of sections so later settings can be added without a
format change; unknown sections and settings are ignored, and missing ones
keep their defaults.

- **Parsing** (`desktop_config.cpp`) is pure: YAML/JSON → `ordered_json`
  (`yaml_to_json()`, shared with the UI descriptor importer) →
  `desktop_config{look, taskbar, menu}`: validated settings, the wallpaper
  path made absolute (relative to the file), and `start_menu{items, power}`
  of `menu_entry`s with placeholders expanded, `apps:` expanded from the
  registry, and unique ids. Errors are
  `config_error`s naming the offending path. A missing file gets the default
  written; an unusable one yields the default menu plus an error, and is
  never overwritten.
- **Rebuilds.** `build_shell()` turns the settings and the menu into a
  descriptor, registers
  it as a fresh `__wish_desktop_chrome_<n>` template, instantiates it, maps
  the instantiated node ids to entry ids, then destroys the previous shell
  root (its `HOOK_DESTRUCT` removes the top-level object). Rebuilding the
  whole shell is simpler than diffing trees and costs one round trip per
  config save.
- **Live reload.** A watcher thread polls the file's mtime and size
  (`--config` may point anywhere, and polling needs no platform-specific
  notification API) and posts `reload_config()` to the worker. The initial
  stamp is taken before the first load, so an edit racing the start is
  still seen. A reload that fails keeps the current configuration and sets
  `DesktopShell.config_error`. The watcher also polls the wallpaper image,
  so a new image saved under the same name is picked up.
- **The wallpaper** is a file on the desktop's machine, while the server
  draws it, so `refresh_wallpaper()` uploads it into the upstream sandbox
  (`private/apps/wish.desktop/wallpaper_<n>.<ext>`) whenever its path, mtime
  or size changes. Each upload gets a new name, because the renderer caches
  textures by path, and the previous one is erased. A missing image is
  reported through `config_error`; `--wallpaper` overrides the file.
- **The theme** in the file applies at start (over the saved one) and on a
  reload that changes it; the tray toggle decides in between. So a user who
  never sets it keeps the toggle's choice across runs, and one who sets it
  sees each edit take effect.
- **Pins and icons** are entry ids. The file supplies the initial set; once
  the user pins, unpins or adds/removes a desktop icon, the saved lists in
  `wish.desktop` (`saved_pins`) win. Unknown ids are ignored.

### Actions and `__WishDesktop`

`run_action()` is the desktop's action table (`quit`, `shutdown_server`,
`set_theme`, `toggle_theme`, `reload_config`, `launch`, plus `items` for
RMI). `action:` menu entries call it, and so can connected apps: the
desktop serves a `__WishDesktop` object on its **downstream** side, through
`on_check_class()`/`on_create_object()`, and `try_handle_request()` keeps
the `OP_INSTANTIATE` of that class and every op on a downstream-local object
id out of the upstream relay. `shutdown_server` can't be done by the bridge
directly (the server is another process), so it sets
`DesktopShell.shutdown` and the host renderer calls its quit callback once.
Clients start entries **by id only**, never an arbitrary command, since the
downstream port may listen on all interfaces.

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
- It publishes each app's live session ids on the `DesktopApp.sessions` of
  every entry with that `app` (an exact match, or a short `--run=nano`
  matching `bdg/desktop/nano`). The
  renderer maps a window's `__wish_id` to its group, and the group to an
  app, for the taskbar icon and the running indicator under pinned apps.

Apps no menu entry names -- a script on the client C ABI that sends
`"app"` (plus optional `"app_label"`/`"app_icon"`) -- get an entry too:
`add_unknown_apps()` instantiates a one-off template holding a single
unlisted `DesktopApp` (`listed = false`: taskbar only, no Start menu entry,
pin or desktop icon, since the desktop can't start it). It is a top-level
object next to the shell rather than a child of it, because the server
doesn't relink `children` by object id on a `set`; `render_desktop_shell()`
lists those top-level `DesktopApp`s after the shell's children, and the
per-session render pass skips them (`is_host_chrome_class()`). The entry is
kept for the run; with no sessions it just isn't shown.

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

An app's **main window** -- its frame, or a top-level non-modal `Window` of
an app without one (the windows the taskbar lists) -- gets a Windows-style
caption (`src/imgui/imgui_window_caption.hpp`): minimize, maximize/restore
and close buttons at the top right, the title ellipsized before them, and a
1px border plus a shadow drawn just outside the window, so overlapping
windows stay apart (inside, the frame's docked panels would cover them).
ImGui's own title is hidden (the window is begun as `"###id"`, the same ID)
except while docked, where the tab needs it. Panels get no caption.
Minimize hides the whole app, as the taskbar does; maximize fills the
cascade area (`desktop_area_min()/max()`) with `NoMove|NoResize` and
restores the saved rectangle (`desktop_maximize_state`, render
bookkeeping like `minimized()`); a `NoResize` window cannot maximize.
Close emits `closed` only when the window (or frame) is `closable`. The
bundled tools' panels are not closable: a panel's X used to close the whole
app, which nobody expects from a panel.

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
`wish_logs/desktop/<entry id>_<n>.log`, never to the desktop's terminal.
The entry's `env` goes on top and its `cwd` is applied. The entry point sets
what `{wish_client}` expands to: `<wish> client` in the combined `wish`
binary, the sibling `wish-client` for the standalone `wish-desktop`. `wish_desktop_app::wait_for_shutdown()`
terminates the children when the desktop exits. With
`--downstream_transport=term` nothing can dial in, so launching is
disabled.

### Settings

Saved in the upstream user store entry `wish.desktop`
(`{pinned: [...], desktop: [...], theme}`); the bridge connects upstream as
`--upstream_username` (default: the OS user name) so the session has a
store. Without a store (anonymous upstream), settings last for the run.
`pinned`/`desktop` are written only once the user has changed them; until
then the config file's lists apply (the default pins Settings, nano, mc and top). The
wallpaper is drawn per `wallpaper_fit` (`detail::desktop_wallpaper_layout()`,
using `imgui_renderer::texture_size()`).

### Lifecycle

```
wish_desktop_app::run()
  parse flags; claim the fixed endpoint (claim_desktop_endpoint()), or
    print "already running" and exit 1
  export WISH_TRANSPORT/HOST/PORT/NAME for the anchor terminal
  bridge_app::run()
    make_bridge()     → multi_server_transport(downstream + endpoint);
                        wish_desktop on it, configured with launcher, client
                        command, launch environment, log dir, wallpaper,
                        user, config file
    bridge::start()   → connects upstream (with username); listens downstream
                        and on the endpoint
    on_listening()    → desktop_->build_chrome() (loads the config, starts
                        the file watcher)
    wait_for_shutdown() → until "Quit desktop", the anchor terminal exits,
                          or the upstream connection is lost;
                          then terminates launched apps and stops the
                          worker and watcher (stop_tasks()) while the
                          upstream connection is still open
    bridge::stop()
```

### Fixed endpoint and single instance

There is at most one desktop per machine, reachable without configuration:
besides its `--downstream_*` transport, the desktop always listens on
`desktop_endpoint()` (`src/client/desktop_endpoint.hpp`): the named pipe
`\\.\pipe\wish-desktop` on Windows, the abstract Unix socket
`@wish-desktop` on Linux, or `$WISH_DESKTOP_ENDPOINT`. Both kinds of name
have the two properties the design rests on:

- **Only one listener at a time.** A second bind fails with
  `address_in_use_error` (libuv creates a Windows pipe's first instance
  with `FILE_FLAG_FIRST_PIPE_INSTANCE`; Linux refuses a second bind on an
  abstract name). So the endpoint *is* the single-instance lock, with no
  lock or pid file. `run()` claims it before connecting upstream or
  spawning anything, so a second desktop fails fast.
- **Released by the OS when the process exits**, even on a crash. Abstract
  sockets leave no file behind, unlike a socket file in `/tmp`, so there is
  nothing stale to clean up.

Alternatives rejected: a file registry of running desktops (needed atomic
writes, pid liveness checks, stale-file reaping and per-user directories),
and UDP/multicast announcements (unreliable on loopback, no access
control). Nothing can notify a process before the desktop exists, so a
waiting client retries the connect instead (`wish_client_run_on_desktop()`,
about once a second; a failed local connect costs microseconds).

The bridge accepts from one transport, so `multi_server_transport` combines
the configured downstream transport (borrowed) with the endpoint (owned):
one accept thread per transport feeds a shared queue. Its `stop()` joins the
accept threads before stopping the transports, so it never depends on a
transport's `stop()` being safe against a concurrent `accept()` (bison's
pipe transport was not, before the fix that came with this feature). When
`--downstream_transport=pipe --downstream_name` already names the endpoint,
no extra listener is added.

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
      desktop_config.hpp               ← desktop.yaml: sections, start_menu parser
      desktop_config.cpp

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
