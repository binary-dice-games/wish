# wish — CLI Reference

`wish` is a single binary with four subcommands — `server`, `client`,
`standalone`, and `desktop` — implemented in `app/wish_cli/`. Each
subcommand is also available as its own single-purpose binary
(`wish-server`, `wish-client`, `wish-standalone`, `wish-desktop`, no
subcommand needed) for callers who only want one mode and a smaller
dependency footprint — see [building.md](building.md#output-locations) for
where each binary lands after a build. Everything below applies equally to
`wish <subcommand>` and its matching standalone binary.

```
wish server     [--transport T] [--host H] [--port P] [--name PATH] [--cmd C]
                [--title T] [--width W] [--height H] [--verbose LEVEL]
wish client     [--transport T] [--host H] [--port P] [--name PATH]
                (--list | --run=<app>) [--timeout MS] [-- app-args...]
wish standalone [--title T] [--width W] [--height H]
                (--list | --run=<app>) [-- app-args...]
wish desktop    [--downstream_transport T] [--downstream_host H]
                [--downstream_port P] [--downstream_name PATH]
                [--upstream_transport T] [--upstream_host H]
                [--upstream_port P] [--upstream_name PATH] [--timeout MS]
wish <app>      [--transport T] [--host H] [--port P] [--name PATH]
                [--timeout MS] [-- app-args...]
```

Anything after a literal `--` is forwarded to the app, e.g.
`wish client --run=nano -- path/to/file`.

## Shared transport flags

`server` and `client` (and their standalone binaries) accept the same
transport flag set — `desktop` has its own `downstream_`/`upstream_`
prefixed pair of these instead (see below), and `standalone` accepts none
of them (it fuses server and client into one process with no transport at
all; passing any of `--transport`/`--host`/`--port`/`--name` is an error).

| Flag | Default | Description |
|------|---------|-------------|
| `--transport T` | `term` | `tcp`, `pipe`, `tls`, or `term`. `term` is an interactive pty/stdio hop the server spawns and the client is expected to run inside — see the [C# bindings example](bindings.md#c-bindingscsharp). `tls` is a TLS-secured TCP socket. Clients also accept `desktop`: the local `wish desktop`'s fixed endpoint, no other flag needed (see [Services: a UI whenever the desktop runs](#services-a-ui-whenever-the-desktop-runs)). |
| `--host H` | `0.0.0.0` for server, `127.0.0.1` for client | Bind/connect host address (`--transport tcp`/`tls` only) |
| `--port P` | `7070` | Bind/connect port (`--transport tcp`/`tls` only) |
| `--name PATH` | *(empty)* | Named-pipe / Unix-socket path (`--transport pipe` only) |
| `--cmd C` | *(empty)* | Command to spawn (`server`, `--transport term` only) |
| `--verbose LEVEL` | `none` | Log verbosity: `none` \| `fatal` \| `error` \| `warning` \| `info` \| `trace`. See [Log verbosity](#log-verbosity) below. |
| `--debugger` | `false` | Wait for debugger attachment before starting |

### Log verbosity

`--verbose` selects a level (ascending verbosity). The server always opens
`wish_logs/server.log`; what gets written depends on the level:

| Level | Effect |
|-------|--------|
| `none` (default) | Nothing written. RMI trace strings are never even formatted. |
| `fatal` / `error` / `warning` | Only client `logger.log()` messages at that severity or higher are written to the log file. Still no RMI trace lines. |
| `info` | Adds RMI request/response trace lines (envelope metadata) and session lifecycle lines; all output is also mirrored to stdout. |
| `trace` | Adds decoded call payloads (`args=`, `set` values, response bodies) to those trace lines. |

`WISH_VERBOSE=<level>` sets the default (an explicit `--verbose` still wins).

### TLS flags (`--transport tls`)

| Flag | Default | Description |
|------|---------|-------------|
| `--cert_file` / `--cert_pem` | *(empty)* | Certificate chain: server cert (`server`) or client cert for mutual TLS (`client`) |
| `--key_file` / `--key_pem` | *(empty)* | Private key matching `--cert_file`/`--cert_pem` |
| `--key_password` | *(empty)* | Passphrase for an encrypted private key |
| `--client_auth` | `none` | Server mutual TLS mode: `none`, `optional`, or `required` (`server` only) |
| `--ca_file` / `--ca_pem` | *(empty)* | Trust anchor: verifies client certs (`server`, when `--client_auth != none`) or the server's cert (`client`) |
| `--server_name` | *(empty)* | SNI / hostname-verification target (`client` only, defaults to `--host`) |
| `--insecure_skip_verify` | `false` | Skip server certificate verification — unsafe, dev/test only (`client` only) |

```sh
wish server --transport tls --port 8443 --cert_file server-cert.pem --key_file server-key.pem
wish client --transport tls --port 8443 --ca_file ca-cert.pem --run nano
```

These map 1:1 onto `tls_socket_client_transport`/`tls_socket_server_transport`'s
own parameters — see [TLS-Secured Transport](https://github.com/binary-dice-games/bison/blob/main/docs/tls.md)
in bison for the full reference (generating dev certificates, mutual TLS, and
the underlying transport/C-ABI details).

## `wish server`

Opens an SDL3 window, or — with `--renderer web` — a browser endpoint, and
renders UI pushed by connected clients over the transport selected at
launch.

| Flag | Default | Description |
|------|---------|-------------|
| `--title TITLE` | `wish` | Window title (`--renderer sdl3` only) |
| `--width N` | `1280` | Initial window width in pixels (`--renderer sdl3` only) |
| `--height N` | `720` | Initial window height in pixels (`--renderer sdl3` only) |
| `--font_size N` | `16` | UI font size in pixels |
| `--renderer NAME` | `web` | Rendering backend: `sdl3` or `web` |
| `--web_port PORT` | `8080` | HTTP/WebSocket port (`--renderer web` only) |
| `--web_bind ADDR` | `127.0.0.1` | Bind address (`--renderer web` only; localhost-only by default) |
| `--settings_admins A,B` | *(empty)* | Identities (`--username`) allowed to change server-wide settings, such as the saved default theme, from the Settings tool (`bdg/desktop/conf`; see [settings.md](settings.md)). Trusts the client-supplied name — local/single-user use only. Never saved as a default |
| `--sandbox_root PATH` | *(empty)* | Keep session sandboxes across connections: each client gets `PATH/<username>` (its `--username`, else `default`) instead of a temp directory deleted on disconnect. Trusts the client-supplied name (`local_auth_module`) — local/single-user use only; see [src/auth/DESIGN.md](../src/auth/DESIGN.md) |
| `--store_dir PATH` | `~/.wish` | Directory holding the persistent server store and per-user stores. The server always trusts a client's `--username` as its identity: identified clients get a user store, clients without one are anonymous. See [docs/persistent-store.md](persistent-store.md) |
| `--lang CODE` | *(empty: `en`)* | Default UI language of every session (e.g. `es`); a client's own `--lang` overrides it. See [docs/i18n.md](i18n.md) |
| `--save_defaults` | `false` | Save the flags given on this command line as the defaults of later runs, then start as usual. See [Saved flag defaults](#saved-flag-defaults) |
| `--clear_defaults` | `false` | Erase the saved defaults |

```sh
# TCP, windowed:
wish server --transport tcp --port 9090 --renderer sdl3 --title "My App Server"

# Browser-based, no window/GPU required:
wish server --renderer web --web_port 8080
```

Close the window, choose **Server → Quit** from the menu bar, or Ctrl+C
(`--renderer web` has no window) to stop the server. See
[building.md#running-the-web-renderer](building.md#running-the-web-renderer)
and [building.md#running-automation](building.md#running-automation) for the
build flags and Python driver needed for the browser backend and the
automation query API.

## `wish client`

Connects to a running server and runs one embedded application module by
name (built in via the `WISH_MODULE_*`/`WISH_COLLECTION_*` CMake options —
see [building.md#cmake-options](building.md#cmake-options); a fresh default
build has none registered).

| Flag | Default | Description |
|------|---------|-------------|
| `--list` | `false` | List available embedded applications and exit |
| `--run=<name>` | *(empty)* | Launch the named app (required unless `--list`/`--describe`) |
| `--describe=<name>` | *(empty)* | Print the named app's description and parameters, and exit |
| `--timeout MS` | `30000` | Connection timeout in milliseconds |
| `--username NAME` | *(empty)* | Identity sent on connect: selects this client's persistent user store (empty: anonymous, no user store; see [docs/persistent-store.md](persistent-store.md)) and, on a server started with `--sandbox_root`, its persistent sandbox directory (empty: the server's `default`) |
| `--theme NAME` | `wish` | UI theme preset. Built in: `dark`, `light`, `classic`, `wish` (a more modern theme built on `dark`, the default). Any name is accepted; one the renderer doesn't recognize falls back to `wish` with a logged warning. |
| `--lang CODE` | *(empty)* | UI language for this session (e.g. `es`): tools load their `i18n/<CODE>.lang` translations, falling back to English. Empty keeps the server's `--lang`. See [docs/i18n.md](i18n.md) |

```sh
wish client --list
wish client --describe=nano
wish client --transport tcp --port 7070 --run=nano -- path/to/file.txt
wish client --run=tail --lang=es -- /var/log/syslog
```

### The `wish <app>` alias

An argument that isn't a known subcommand (`server`/`client`/`standalone`/
`desktop`) and names a registered app is aliased to
`wish client --run=<app> ...`, forwarding every other argument unchanged:

```sh
wish nano -- path/to/file.txt   # == wish client --run=nano -- path/to/file.txt
```

## `wish standalone`

Fuses server and client into a single process with no transport and no
serialization — the fastest way to run one app interactively, and the
recommended way to render a mockup for the `wish-ui`/`wish-module` AI-agent
skills (see [ai-assisted-development.md](ai-assisted-development.md)).
The binary must have been built with `WISH_ENABLE_SDL3=ON` (the default) —
unlike `server`, which can run in a web-only build
(`-DWISH_ENABLE_SDL3=OFF -DWISH_ENABLE_WEB=ON`), `standalone` isn't
available at runtime in that configuration even though, once built with
SDL3, it still supports `--renderer sdl3|web` at runtime exactly like
`server`, plus `--renderer none`.

| Flag | Default | Description |
|------|---------|-------------|
| `--list` | `false` | List available embedded applications and exit |
| `--run=<name>` | *(empty)* | Launch the named app (required unless `--list`/`--describe`) |
| `--describe=<name>` | *(empty)* | Print the named app's description and parameters, and exit |
| `--title TITLE` | `wish` | Window title (`--renderer sdl3` only) |
| `--width N` | `1280` | Initial window width in pixels (`--renderer sdl3` only) |
| `--height N` | `720` | Initial window height in pixels (`--renderer sdl3` only) |
| `--renderer NAME` | `web` | Rendering backend: `sdl3`, `web`, or `none` (no window and no web server — for apps run as command line tools, see below) |
| `--web_port PORT` / `--web_bind ADDR` | `8080` / `127.0.0.1` | Same as `server` (`--renderer web` only) |
| `--sandbox_root PATH` / `--username NAME` | *(empty)* / `default` | Persist the session sandbox in `PATH/<username>` across runs instead of a temp directory deleted on exit. `--username` (or `default`) also selects the session's user store |
| `--store_dir PATH` | `~/.wish` | Directory holding the persistent server store and user stores (see [docs/persistent-store.md](persistent-store.md)) |
| `--lang CODE` | *(empty: `en`)* | UI language (e.g. `es`); see [docs/i18n.md](i18n.md) |
| `--save_defaults` / `--clear_defaults` | `false` | Save the flags given on this command line as the defaults of later runs, or erase them. Without `--run`, only updates the saved defaults and exits. See [Saved flag defaults](#saved-flag-defaults) |

`standalone` has no `--transport`/`--host`/`--port`/`--name`/`--theme`
flags — passing a transport flag is rejected with an explicit error, since
there is no transport to configure.

```sh
wish standalone --list
wish standalone --run=bc --renderer sdl3
```

`--renderer none` is for an app that does a job and exits rather than
showing a UI, e.g. [nymph](../modules/bdg/dev/nymph/README.md)'s `render`:

```sh
wish standalone --renderer none --run=nymph -- render chart.nymph -o chart.png
```

Nothing is drawn, the log goes to the system temp directory instead of
`./wish_logs`, and the process exit code is whatever the app set with
`wish_app_host::set_exit_code()` (0 unless the app reports a failure; the
same holds for `wish client --run`).

## `wish desktop`

A desktop for your wish apps, in the style of Windows or macOS. It listens
**downstream** for client connections (like a server) while itself
connecting **upstream** to a real `wish server` (like a client), and turns
that server's window into a desktop:

- **Wallpaper**: a built-in gradient that follows the theme, a plain color,
  or your own image, set in the [configuration file](desktop-config.md#appearance)
  (or with `--wallpaper`).
- **Taskbar** along the bottom, always visible; windows open above it:
  - **Start** button: a menu of apps and actions in folders, read from the
    [desktop configuration file](desktop-config.md) (by default every app
    built into wish, a folder per collection). Type to search, and press
    Enter to open the best match. Right-click an entry to pin it to the
    taskbar or add it to the desktop. The **power** button at the bottom
    quits the desktop or shuts down the server.
  - **Pinned apps**: click to start one. A line under the icon means it is
    running; clicking it then brings it to the front (or minimizes it when
    it already is), and its right-click menu has "New window" and "Unpin".
  - **Open windows**: one button per app or window. Click to bring it to the
    front, click the active one to minimize it; right-click for Minimize /
    Restore, New window, Pin and Close.
  - **Tray**: number of connected apps, a light/dark theme toggle, and the
    time and date (click for a calendar). The strip at the far right edge
    minimizes every window ("show desktop"); click it again to restore them.
- **Desktop icons**: double-click to start the app; right-click to remove
  it or pin it.
- **Windows** open floating and cascaded, like on a desktop. Drag one onto
  the desktop's dock area to dock it.
- **Settings**: the pinned Settings button (also in the Start menu) opens
  the [Settings tool](settings.md), macOS System Settings-style, to change
  your apps' settings, and, with `wish server --settings_admins=<you>`, the
  server's defaults such as its theme.
- **Window switcher**: Ctrl+Tab (Ctrl+Shift+Tab backwards) cycles through
  the open windows while Ctrl is held. In a browser, which keeps Ctrl+Tab for
  itself (`--renderer web`), use Ctrl+\` instead.

The wallpaper, background color, theme, desktop icons, clock formats and
the Start menu (with its initial pins, desktop icons and power button) come
from `~/.wish/desktop/desktop.yaml` (`--config`), written with the default
configuration on first run; edit it and the desktop reloads it live.
Each entry either runs a command or calls a desktop action, so external apps
can be installed there too. See [desktop-config.md](desktop-config.md).

Starting a built-in app runs `wish client --run=<app>` as a separate process
that connects back to the desktop (the `wish-client` beside a standalone
`wish-desktop` binary). Its output goes to
`wish_logs/desktop/<entry id>_<n>.log`. Apps started from the desktop are
closed when the desktop quits. Pins and desktop icons you change, and the
theme, are saved in the upstream server's [user store](persistent-store.md)
(entry `wish.desktop`). The desktop connects as `--upstream_username`, which
defaults to your OS user name.

Downstream and upstream each get their own flag set, since both transports
are active at once:

| Flag | Default | Description |
|------|---------|-------------|
| `--downstream_transport T` | `tcp` | `tcp`, `pipe`, `tls`, or `term` (apps can't be started from the desktop with `term`) |
| `--downstream_host H` | `0.0.0.0` | (`downstream_transport=tcp`/`tls` only) |
| `--downstream_port P` | `7071` | (`downstream_transport=tcp`/`tls` only) |
| `--downstream_name PATH` | *(empty)* | (`downstream_transport=pipe` only) |
| `--downstream_cert_file`/`--downstream_cert_pem`, `--downstream_key_file`/`--downstream_key_pem`, `--downstream_key_password`, `--downstream_client_auth`, `--downstream_ca_file`/`--downstream_ca_pem` | — | Downstream TLS server flags, same meaning as `--cert_file`/etc. above (`downstream_transport=tls` only) |
| `--upstream_transport T` | `term` | `tcp`, `pipe`, `tls`, or `term` |
| `--upstream_host H` | `127.0.0.1` | (`upstream_transport=tcp`/`tls` only) |
| `--upstream_port P` | `7070` | (`upstream_transport=tcp`/`tls` only) |
| `--upstream_name PATH` | *(empty)* | (`upstream_transport=pipe` only) |
| `--upstream_username NAME` | OS user | Identity the desktop connects upstream as; its user store keeps the desktop settings |
| `--cmd C` | *(empty)* | Command to spawn (`upstream_transport=term` only) |
| `--upstream_ca_file`/`--upstream_ca_pem`, `--upstream_server_name`, `--upstream_insecure_skip_verify`, `--upstream_cert_file`/`--upstream_cert_pem`, `--upstream_key_file`/`--upstream_key_pem`, `--upstream_key_password` | — | Upstream TLS client flags, same meaning as `--ca_file`/etc. above (`upstream_transport=tls` only) |
| `--config PATH` | `~/.wish/desktop/desktop.yaml` | [Desktop configuration file](desktop-config.md); written with the default if missing, reloaded when it changes |
| `--wallpaper FILE` | *(from the config file)* | Image (png, jpg, ...) shown as the desktop background; overrides the config file's `appearance.wallpaper` |
| `--timeout MS` | `30000` | Upstream connection timeout in milliseconds |

```sh
wish server --transport tcp &          # the window the desktop draws in
wish desktop --upstream_transport tcp --wallpaper ~/Pictures/beach.jpg
```

**Apps using the client ABI** (`wish_client_c.h`, and every binding on top
of it) work on the desktop too: they connect to its downstream port like
`wish client` does, and their windows float, cascade, minimize and follow the
theme the same way. To show on the taskbar with their own name and icon --
and be counted, and grouped, like a built-in tool -- they add three optional
fields to their connect params: `app` (an identifier), `app_label` (the
name shown) and `app_icon` (e.g. `res/icons/check.png`). Without them, a
window shows under its title with a generic icon. Add a `run` entry for it
to the [configuration file](desktop-config.md#installing-a-client-abi-app)
to start it from the Start menu and pin it; without one it only shows on the
taskbar while running. Apps can also call the desktop's actions (start a
menu entry, switch the theme, quit) through its `__WishDesktop` object. Python's
`Client.from_env()` connects wherever the `WISH_*` variables point; see
`bindings/python/examples/desktop/` for three examples.

`wish desktop` sets `WISH_TRANSPORT`/`WISH_HOST`/`WISH_PORT`/`WISH_NAME` in
the terminal it spawns to match its own downstream flags, so a `wish
client`/`wish server` launched from inside that terminal connects to the
desktop with no flags at all (and shows up on its taskbar like an app
started from the Start menu):

```sh
wish desktop                      # downstream defaults to tcp:7071
# inside the spawned terminal:
wish client --run nano     # connects to the desktop's tcp:7071, no flags needed
wish nano                  # same thing, via the app-name alias
```

### Services: a UI whenever the desktop runs

Only one `wish desktop` runs per machine. Besides its downstream transport,
it always listens on a fixed, system-wide local endpoint, and owning that
endpoint is what stops a second desktop from starting (it prints `wish
desktop is already running` and exits with 1). The endpoint is released by
the OS when the desktop exits, even after a crash. A desktop also exits
when the wish server it draws on stops, so it never holds the endpoint
without a screen to show.

| Platform | Endpoint |
|----------|----------|
| Windows (and MSYS2) | named pipe `\\.\pipe\wish-desktop` |
| Linux | abstract Unix socket `@wish-desktop` (no file on disk) |

`WISH_DESKTOP_ENDPOINT` overrides the name, for the desktop and its clients
alike. Any local process can connect there without knowing a port:

```sh
wish client --transport=desktop --run nano
WISH_TRANSPORT=desktop wish nano
```

A long-running process (a service, a daemon) that wants a UI on the desktop
uses the client library's desktop loop: it retries the connection while no
desktop is running (about once a second), runs its UI session when one is,
and goes back to waiting when the desktop closes, so the UI comes back when
the desktop is started again. In the C ABI that is
`wish_client_desktop_create()` + `wish_client_run_on_desktop()` (stopped by
`wish_client_desktop_stop()`); every binding wraps it, see
[bindings.md](bindings.md#desktop-services-a-ui-whenever-the-desktop-runs)
and `bindings/python/examples/desktop/service.py`.

## Environment-variable flag defaults

Every flag above falls back to a `WISH_<FLAG_NAME_UPPERCASED>` environment
variable when not given on the command line (e.g. `WISH_TRANSPORT`,
`WISH_HOST`, `WISH_PORT`, `WISH_NAME`, `WISH_DOWNSTREAM_PORT`,
`WISH_WEB_BIND`, ...) — an explicit command-line flag always wins. This is
the mechanism `wish desktop` uses to make its spawned terminal's `wish
client`/`wish server` connect with no flags, shown above.

## Saved flag defaults

`wish server` and `wish standalone` can keep their own defaults in the
[server store](persistent-store.md) (`<store_dir>/server_store.bison`). Each
flag resolves in this order:

1. the command line;
2. its `WISH_<FLAG>` environment variable;
3. the saved default;
4. the built-in default listed in the tables above.

`--save_defaults` saves the flags given on the command line (not those from
environment variables), merged over the ones saved before. `--clear_defaults`
erases them all:

```sh
wish server --renderer sdl3 --font_size 20 --lang es --save_defaults
wish server                      # sdl3, font size 20, Spanish
WISH_LANG=fr wish server         # the env var still wins: French
wish server --lang en            # and so does the command line
wish server --clear_defaults     # back to the built-in defaults

wish standalone --theme light --save_defaults   # saves and exits (no --run)
```

Every flag of the mode can be saved, except:

- in every mode: `--save_defaults`, `--clear_defaults`, `--store_dir` (it
  locates the store), secrets (flags named `*password` or `*_pem`, e.g.
  `--key_password`, `--cert_pem`) and gflags' own flags (`--help`,
  `--flagfile`, ...);
- the debug and security switches: `--debugger`, `--profiling_dir`,
  `--profiling_autostart`, `--allow_absolute_paths`;
- in `standalone` only: `--run`, `--list`, `--describe`, and the rejected
  transport flags.

| Mode | Store entry |
|------|-------------|
| `server` | `wish.cli.server` |
| `standalone` | `wish.cli.standalone` |

The defaults live under `--store_dir`, so pass the same `--store_dir` when
saving and when running. A saved value that is no longer valid for its flag
is skipped with a warning.

## See also

- [building.md](building.md) — build prerequisites and CMake options.
- [examples.md](examples.md) — the `calculator`/`demo` examples, which run
  entirely in-process and don't go through this CLI at all.
- [bindings.md](bindings.md) — connecting from Python or C# instead of the
  CLI.
- [ai-assisted-development.md](ai-assisted-development.md) — building a new
  `wish client --run=<name>` app or embedding wish UI into another
  application with an AI agent's help.
