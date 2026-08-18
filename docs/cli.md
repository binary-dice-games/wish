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
                [--title T] [--width W] [--height H] [--verbose]
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
`wish client --run=notepad -- path/to/file`.

## Shared transport flags

`server` and `client` (and their standalone binaries) accept the same
transport flag set — `desktop` has its own `downstream_`/`upstream_`
prefixed pair of these instead (see below), and `standalone` accepts none
of them (it fuses server and client into one process with no transport at
all; passing any of `--transport`/`--host`/`--port`/`--name` is an error).

| Flag | Default | Description |
|------|---------|-------------|
| `--transport T` | `term` | `tcp`, `pipe`, `tls`, or `term`. `term` is an interactive pty/stdio hop the server spawns and the client is expected to run inside — see the [C# bindings example](bindings.md#c-bindingscsharp). `tls` is a TLS-secured TCP socket. |
| `--host H` | `0.0.0.0` for server, `127.0.0.1` for client | Bind/connect host address (`--transport tcp`/`tls` only) |
| `--port P` | `7070` | Bind/connect port (`--transport tcp`/`tls` only) |
| `--name PATH` | *(empty)* | Named-pipe / Unix-socket path (`--transport pipe` only) |
| `--cmd C` | *(empty)* | Command to spawn (`server`, `--transport term` only) |
| `--verbose` | `false` | Print session lifecycle / RMI trace messages to stdout |
| `--debugger` | `false` | Wait for debugger attachment before starting |

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
wish client --transport tls --port 8443 --ca_file ca-cert.pem --run notepad
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
| `--theme NAME` | `wish` | UI theme preset. Built in: `dark`, `light`, `classic`, `wish` (a more modern theme built on `dark`, the default). Any name is accepted; one the renderer doesn't recognize falls back to `wish` with a logged warning. |

```sh
wish client --list
wish client --describe=notepad
wish client --transport tcp --port 7070 --run=notepad -- path/to/file.txt
```

### The `wish <app>` alias

An argument that isn't a known subcommand (`server`/`client`/`standalone`/
`desktop`) and names a registered app is aliased to
`wish client --run=<app> ...`, forwarding every other argument unchanged:

```sh
wish notepad -- path/to/file.txt   # == wish client --run=notepad -- path/to/file.txt
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
`server`.

| Flag | Default | Description |
|------|---------|-------------|
| `--list` | `false` | List available embedded applications and exit |
| `--run=<name>` | *(empty)* | Launch the named app (required unless `--list`/`--describe`) |
| `--describe=<name>` | *(empty)* | Print the named app's description and parameters, and exit |
| `--title TITLE` | `wish` | Window title (`--renderer sdl3` only) |
| `--width N` | `1280` | Initial window width in pixels (`--renderer sdl3` only) |
| `--height N` | `720` | Initial window height in pixels (`--renderer sdl3` only) |
| `--renderer NAME` | `web` | Rendering backend: `sdl3` or `web` |
| `--web_port PORT` / `--web_bind ADDR` | `8080` / `127.0.0.1` | Same as `server` (`--renderer web` only) |

`standalone` has no `--transport`/`--host`/`--port`/`--name`/`--theme`
flags — passing a transport flag is rejected with an explicit error, since
there is no transport to configure.

```sh
wish standalone --list
wish standalone --run=calculator --renderer sdl3
```

## `wish desktop`

A multiplexing shell: it listens **downstream** for client connections
(like a server) while itself connecting **upstream** to a real `wish
server` (like a client), rendering a menu bar (File → Quit) and clock, with
downstream clients' own windows docking into the same upstream session
automatically. Useful for running several `wish client`/`wish <app>`
processes as windows inside one shared desktop, without each one owning its
own top-level window.

Downstream and upstream each get their own flag set, since both transports
are active at once:

| Flag | Default | Description |
|------|---------|-------------|
| `--downstream_transport T` | `tcp` | `tcp`, `pipe`, `tls`, or `term` |
| `--downstream_host H` | `0.0.0.0` | (`downstream_transport=tcp`/`tls` only) |
| `--downstream_port P` | `7071` | (`downstream_transport=tcp`/`tls` only) |
| `--downstream_name PATH` | *(empty)* | (`downstream_transport=pipe` only) |
| `--downstream_cert_file`/`--downstream_cert_pem`, `--downstream_key_file`/`--downstream_key_pem`, `--downstream_key_password`, `--downstream_client_auth`, `--downstream_ca_file`/`--downstream_ca_pem` | — | Downstream TLS server flags, same meaning as `--cert_file`/etc. above (`downstream_transport=tls` only) |
| `--upstream_transport T` | `term` | `tcp`, `pipe`, `tls`, or `term` |
| `--upstream_host H` | `127.0.0.1` | (`upstream_transport=tcp`/`tls` only) |
| `--upstream_port P` | `7070` | (`upstream_transport=tcp`/`tls` only) |
| `--upstream_name PATH` | *(empty)* | (`upstream_transport=pipe` only) |
| `--cmd C` | *(empty)* | Command to spawn (`upstream_transport=term` only) |
| `--upstream_ca_file`/`--upstream_ca_pem`, `--upstream_server_name`, `--upstream_insecure_skip_verify`, `--upstream_cert_file`/`--upstream_cert_pem`, `--upstream_key_file`/`--upstream_key_pem`, `--upstream_key_password` | — | Upstream TLS client flags, same meaning as `--ca_file`/etc. above (`upstream_transport=tls` only) |
| `--timeout MS` | `30000` | Upstream connection timeout in milliseconds |

```sh
wish desktop   # upstream: spawns its own terminal (term, default)
               # downstream: listens on tcp:7071
```

`wish desktop` sets `WISH_TRANSPORT`/`WISH_HOST`/`WISH_PORT`/`WISH_NAME` in
the terminal it spawns to match its own downstream flags, so a `wish
client`/`wish server` launched from inside that terminal connects to the
desktop with no flags at all:

```sh
wish desktop                      # downstream defaults to tcp:7071
# inside the spawned terminal:
wish client --run notepad         # connects to the desktop's tcp:7071, no flags needed
wish notepad                      # same thing, via the app-name alias
```

## Environment-variable flag defaults

Every flag above falls back to a `WISH_<FLAG_NAME_UPPERCASED>` environment
variable when not given on the command line (e.g. `WISH_TRANSPORT`,
`WISH_HOST`, `WISH_PORT`, `WISH_NAME`, `WISH_DOWNSTREAM_PORT`,
`WISH_WEB_BIND`, ...) — an explicit command-line flag always wins. This is
the mechanism `wish desktop` uses to make its spawned terminal's `wish
client`/`wish server` connect with no flags, shown above.

## See also

- [building.md](building.md) — build prerequisites and CMake options.
- [examples.md](examples.md) — the `calculator`/`demo` examples, which run
  entirely in-process and don't go through this CLI at all.
- [bindings.md](bindings.md) — connecting from Python or C# instead of the
  CLI.
- [ai-assisted-development.md](ai-assisted-development.md) — building a new
  `wish client --run=<name>` app or embedding wish UI into another
  application with an AI agent's help.
