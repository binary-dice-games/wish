# The wish home (`~/.wish`)

Everything wish keeps for its users lives under one directory, the **wish
home**: `~/.wish` by default (`%USERPROFILE%\.wish` on native Windows).
Nothing is written to the working directory.

```
~/.wish/
  server/
    store.bison                    server store (saved defaults, server-wide settings)
  users/
    <user>/                        one profile per user
      store.bison                  user store (tool settings, desktop pins, theme, ...)
      sandbox/                     the user's files on the server (uploads, tool data)
      desktop/desktop.yaml         wish desktop configuration
  logs/
    server.log                     wish server
    standalone.log                 wish standalone
    desktop/<app>_<n>.log          apps started from wish desktop
  screenshots/                     server overlay screenshots
  traces/                          profiler traces (--profiling)
```

Directories are created when first used.

## Choosing the home

In order of precedence:

1. `--wish_home PATH` (`wish server`, `wish standalone`, `wish desktop`), or
   `wish::server::set_home()`, `wish::standalone::set_home()`, or the
   `wish_home` param of `wish_server_start()`;
2. the `WISH_HOME` environment variable;
3. `~/.wish`.

Point it at a scratch directory for tests and throwaway runs, so nothing
reaches your real profile.

## Users

A client connects as the **logged-in OS user** unless it says otherwise:

- `wish client --username NAME` (or `{"username": NAME}` in the connect
  params of any binding) picks another profile.
- `--anonymous` (`{"anonymous": true}`) connects without one: no user store,
  and a temporary sandbox in the system temp directory, deleted on
  disconnect. Anonymous sessions never write under the wish home.

User names are normalized into the profile's directory name: lowercased,
spaces become `_`, and only `a-z`, `0-9`, `.`, `_` and `-` are allowed (at
most 64, not starting with `.`). A name that can't be normalized connects
as anonymous.

The server trusts the name the client sends (`local_auth_module`). Only use
it on a single-user machine or a trusted network.

## Sandboxes

An identified client keeps its sandbox in `users/<user>/sandbox` across
connections when the server has **persistent sandboxes** on
(`wish server --persistent_sandboxes`):

| Server | Default |
|--------|---------|
| pipe, terminal, in-process (`wish desktop --with_server`), `wish standalone` | on |
| TCP/TLS bound to loopback (`127.0.0.1`, `localhost`, `::1`) | on |
| TCP/TLS bound to any other address | off: each connection gets a temporary sandbox |

The user store is kept in either case.

See also: [persistent-store.md](persistent-store.md),
[cli.md](cli.md), [src/context/DESIGN.md](../src/context/DESIGN.md).
