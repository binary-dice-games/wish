# wish Home Directory & User Profiles — Architecture & Design

**Status: implemented.** Refactors where wish puts its files on disk and
how a session's identity (profile) is chosen. User-facing summary:
[docs/wish-home.md](../../docs/wish-home.md). Builds on the auth module and
persistent sandbox design in [src/auth/DESIGN.md](../auth/DESIGN.md), which
stays authoritative for `auth_module_iface`, `local_auth_module` and the
per-tool `private/` layout inside a sandbox.

## 1. Purpose / Scope

Gives every file wish writes for a user one fixed place under one root
(`~/.wish` by default), and makes "identified user" the default instead of
"anonymous", so a user can find their profile, sandbox, settings, logs,
screenshots and traces without knowing which flag put them where.

Owns:

- `src/context/wish_home.hpp`/`.cpp` (new; replaces `store_dir.hpp`/`.cpp`):
  the layout (`wish::wish_home`), root resolution and identity-to-directory
  mapping.
- `src/context/os_user.hpp`/`.cpp` (new): `wish::os_user_name()`, the
  logged-in OS user, used as the default identity.
- The path-related parts of `persistent_store.hpp` (`store_registry`),
  `context.hpp`/`.cpp` (`is_safe_sandbox_identity()`, `resource_dir`) and
  their callers in `src/server`, `src/standalone`, `src/wish_server_c.cpp`,
  `src/wish_client_c.cpp`, `src/client` and `app/wish_cli`.

Does not cover: authentication (still trust-the-client
`local_auth_module`; see 7.4), the format or API of the persistent stores
(`docs/persistent-store.md`), the sandbox's internal layout
(`src/auth/DESIGN.md`, "Sandbox layout"), or anonymous sessions, which keep
their throwaway OS-temp sandbox.

## 2. Current State (why it does not work as expected)

Each location is chosen by a different flag with its own default, so an
identified session's files end up in up to four unrelated places:

| What | Today | Chosen by |
|------|-------|-----------|
| Server store | `~/.wish/server_store.bison` | `--store_dir` |
| User store | `~/.wish/users/<id>.bison` | `--store_dir` |
| Session sandbox | OS temp `wish_<session id>`, deleted on disconnect | `--sandbox_root` (persistent only if set; unrelated to `--store_dir`) |
| Desktop config | `~/.wish/desktop/desktop.yaml` (not per identity) | `wish desktop --config` |
| Logs | `./wish_logs/` (CWD-relative; OS temp for `--renderer none`) | hard-coded |
| Desktop app logs | `./wish_logs/desktop/` | hard-coded |
| Screenshots | `./wish_screenshots/` | `--screenshot_dir` |
| Profiler traces | nowhere unless set | `--profiling_dir` (also the on/off switch) |

Root causes of "user sessions don't go into store_dir":

1. The sandbox only persists when `--sandbox_root` is passed
   (`server::on_authenticated()` returns early when
   `persistent_sandbox_root_` is empty). `--store_dir` never affects the
   sandbox, so an identified session gets its user store under
   `~/.wish` but its files in a temp directory deleted on disconnect.
2. `wish_server_start()` (C ABI) has no way to set a sandbox root at all,
   so servers started from a binding never persist sandboxes.
3. `wish client --username` defaults to empty, so a client is anonymous
   unless the user passes it; only `wish desktop` falls back to the OS
   user, and `wish standalone` falls back to the literal `default`.
4. Identities are used as directory names without case folding or a
   character whitelist (`is_safe_sandbox_identity()` only rejects `/`, `\`
   and `..`), so `Carlos` and `carlos`, or a name with `:`, behave
   differently per platform.

## 3. Design Goals

1. **One root.** Everything wish keeps for a user lives under one
   directory, `~/.wish` by default, overridable by one flag / env var.
2. **One layout.** Every path is derived from the root by one class
   (`wish_home`); no caller concatenates paths under the root itself.
3. **Identified by default.** A client that does not name a user connects
   as the logged-in OS user; anonymous is an explicit opt-out.
4. **A profile is one directory.** An identity's user store, persistent
   sandbox and per-user config sit together in `users/<identity>/`.
5. **Anonymous stays throwaway.** An anonymous session's sandbox is always
   `<os temp>/wish_<session id>`, deleted on disconnect, never under the
   wish home, whatever the root or persistence settings are.
6. **Same behavior on Linux, MSYS2 and native Windows**, including
   case-insensitive file systems.

## 4. Directory Layout

```
<wish_home>/                         ~/.wish (see 5.1)
  server/
    store.bison                      server store
  users/
    <identity>/                      one profile per identity (see 5.3)
      store.bison                    user store
      sandbox/                       persistent session sandbox (resource_dir)
        ...                          user files; res/, private/apps/..., private/packages/...
      desktop/
        desktop.yaml                 wish desktop config of this identity
  logs/
    server.log
    standalone.log
    desktop/<app>_<n>.log            output of apps launched by wish desktop
  screenshots/                       server overlay screenshots
  traces/                            profiler .perfetto-trace files
```

- Anonymous sessions are not in this tree: their sandbox stays
  `<os temp>/wish_<session id>`, deleted on disconnect, with no user store.
- Directories are created on first use, never up front, so `~/.wish`
  shows only what has actually been used.
- The web renderer's extracted assets stay in OS temp: they are a cache of
  embedded data, not user files.

## 5. Key Abstractions

### 5.1 `wish::wish_home`

A value type holding the root and deriving every path from it. Cheap to
copy; holds no open files and no locks.

- Root resolution (`wish_home::default_root()`), first match wins:
  1. an explicit root passed by the caller (`--wish_home`, the
     `wish_home` C ABI param, `server::set_home()`);
  2. the `WISH_HOME` environment variable;
  3. `$HOME/.wish` (`%USERPROFILE%\.wish` on native Windows);
  4. `.wish` in the working directory (no home directory at all).
- Accessors: `root()`, `server_store_file()`, `logs_dir()`,
  `screenshots_dir()`, `traces_dir()`, and per identity `user_dir(id)`,
  `user_store_file(id)`, `user_sandbox_dir(id)`, `user_desktop_config(id)`.
- Per-identity accessors return an empty path for an identity that is not
  valid (5.3). Callers treat empty as "anonymous": no store, temp sandbox.
- `list_identities()` returns the identities with a `users/<id>/`
  directory (the server overlay's settings editor uses it instead of
  scanning `users/*.bison`).
- Invariant: nothing outside `wish_home` builds a path under the root.

### 5.2 `wish::os_user_name()`

Returns the logged-in OS user's login name, or empty if it cannot be
determined.

- POSIX: `getpwuid_r(geteuid())`, falling back to `$USER`/`$LOGNAME`.
- Windows: `GetUserNameW` (UTF-8 converted), falling back to
  `%USERNAME%`. A `DOMAIN\user` or `user@domain` form keeps only `user`.
- Lives in the client library (so `wish desktop`, the C client ABI and
  every binding reach it) and replaces the private copy in
  `wish_desktop_app.cpp`.

### 5.3 Identity normalization

An identity is used as a directory name, so it is normalized once, in
`normalize_identity()`, before any use (store, sandbox, admin check,
notification source):

- ASCII letters are lowercased, so one user gets one profile on
  case-insensitive (NTFS, APFS) and case-sensitive file systems alike.
  Spaces become `_`, since Windows account names often contain them.
- The result must match `[a-z0-9._-]{1,64}` and must not start with `.`;
  anything else is rejected and the session stays anonymous (and the
  server logs it, as today).
- `is_safe_sandbox_identity()` becomes "normalizes to a valid identity";
  `context::identity` holds the normalized form.

### 5.4 `store_registry`

Constructed from a `wish_home` instead of a directory. Opens
`server_store_file()` and `user_store_file(id)`; caching, locking and the
store format are unchanged.

## 6. Data Flow

Connecting client (any API or binding):

1. The client library builds the connect params. If they carry no
   `username` and no `anonymous: true`, it adds
   `username = os_user_name()` (skipped when that is empty). This is
   `client::apply_default_identity()`, called by `wish::client::run()`
   (so the C ABI and every binding) and by `wish client`. A bare
   `bison::rmi::client::connect()` adds nothing: low-level callers and
   tests stay anonymous unless they ask.
2. The server's `local_auth_module` returns that username as the identity
   (no default identity: a client that sends none, such as an old client
   or one connecting anonymously, stays anonymous).
3. `server::on_authenticated()` normalizes it (5.3). Invalid: anonymous,
   stop.
4. It attaches the user store at `home.user_store_file(id)`.
5. If persistent sandboxes are enabled (7.4), it adopts
   `home.user_sandbox_dir(id)` as `resource_dir` (marked persistent, not
   deleted on disconnect) and re-instantiates the file and media services,
   as today.

Server process startup (`wish server`, `wish standalone`,
`wish desktop --with_server`, `wish_server_start()`):

1. Resolve the `wish_home` (5.1) and pass it to the server
   (`set_home()`), the logger (`logs_dir()`), the overlay
   (`screenshots_dir()`) and the profiler (`traces_dir()`).
2. Log the resolved root at info level, so the user can see where
   everything is.

`wish standalone` has no connect step: it uses `--username`, else
`os_user_name()`, else anonymous, and goes through steps 3-5 directly.

`wish desktop` reads `user_desktop_config(identity)` from its own local
`wish_home`, where identity is `--upstream_username` or `os_user_name()`
(`<root>/desktop.yaml` for a name that can't be a profile). Apps it
launches need no identity of their own: the desktop multiplexes them into
its one upstream session.

## 7. Public API Contract

### 7.1 C++

| API | Change |
|-----|--------|
| `wish::wish_home` (new) | See 5.1. |
| `wish::os_user_name()` (new) | See 5.2. |
| `server::set_home(path)` / `home()` (new) | Must be called before `start()`. |
| `server::set_persistent_sandboxes(bool)` (new) | Default per 7.4. Must be called before `start()`. |
| `standalone::set_home()` / `set_persistent_sandboxes()` (new) | Same contract as the server's; persistent sandboxes default on (one local session). |
| `standalone::set_user_identity()` | Now also selects the persistent sandbox. |
| `client::apply_default_identity(params)` (new) | See 6, step 1. |
| `is_loopback_host(host)` (new) | `localhost`, `127.x.x.x`, `::1`, `[::1]`. |
| `server::set_store_dir()`, `server::set_persistent_sandbox_root()`, `standalone::set_store_dir()`, `standalone::set_persistent_sandbox()`, `default_store_dir()` | Removed. |

### 7.2 CLI

| Flag | Change |
|------|--------|
| `--wish_home PATH` (all modes) | New. Root of the layout; default per 5.1. |
| `--store_dir PATH`, `--sandbox_root PATH` | Removed. |
| `--username NAME` | Default becomes the OS user, not empty / `default`. |
| `--anonymous` (client, standalone) | New. Connect without an identity. |
| `--persistent_sandboxes=auto\|true\|false` (server) | New. See 7.4; `auto` is the default. |
| `--profiling` (server, standalone) | New on/off switch; traces go to `traces_dir()`. |
| `--profiling_dir PATH` | Overrides the trace location; still implies `--profiling`. |
| `--screenshot_dir PATH` | Default becomes `screenshots_dir()`. |

As `--store_dir` was, `--wish_home` is never saved by
`--save_defaults`: the saved defaults live under it.

### 7.3 C ABI and bindings

- `wish_server_start()` params: `wish_home` (new; replaces `store_dir`,
  which is removed) and `persistent_sandboxes` (bool).
- `wish_client_run_with_params()` and the other connect entry points add
  `username = os_user_name()` unless the params carry `username` or
  `anonymous: true` (6, step 1). Every binding layered on the C ABI
  inherits this with no binding change; each binding documents the
  `anonymous` param. A binding that builds its own connect params without
  the C ABI must add the same default.
- Android (JNI over the C ABI) inherits it too, but the OS user there is
  the per-app uid name (e.g. `u0_a123`), which identifies the app, not the
  person. The Android binding therefore passes an explicit `username` (or
  `anonymous`) chosen by the app rather than relying on the default.

### 7.4 Persistent sandboxes default

Making identified the default (goal 3) would give every TCP client, not
only local ones, a persistent directory chosen by a name it claims. So:

- `persistent_sandboxes` defaults to **on** when the server only accepts
  local connections (memory, pipe/Unix socket, terminal, TCP/TLS bound to
  a loopback address), and **off** otherwise. The user store follows the
  same identity either way, as today.
- `wish::server` cannot see a TCP/TLS listener's bind address (bison does
  not expose it), so its own default is off for any `socket_server_transport`
  or `tls_socket_server_transport` and on for every other transport. The
  layers that know the address -- `wish server` (`--host`) and
  `wish_server_start()` (the `*_create()` host) -- turn it on for a
  loopback bind.
- An operator exposing the server on the network can turn it on
  explicitly; `local_auth_module` still trusts the client's claim, so that
  remains a trusted-network-only setting (unchanged from
  `src/auth/DESIGN.md`).

## 8. Design Decisions

- **Profile directory per identity (`users/<id>/store.bison`), not a flat
  `users/<id>.bison` beside a separate sandbox root.** Rejected keeping the
  flat file and adding `sandboxes/<id>/`: one user's data would still be
  split across two trees, which is the problem being fixed.
- **The client supplies the OS user, the server does not default to its
  own.** A server-side default (`local_auth_module(os_user_name())`) would
  file every client of a shared or remote server under the account running
  the server. The client knows who is logged in on its side.
- **Anonymous stays possible and stays in OS temp.** Tests, CI, automation
  recipes and throwaway runs need a session that leaves nothing behind;
  `--anonymous` is cheaper than a scratch `--wish_home` for that.
- **Lowercase normalization over case-preserving names.** Case-insensitive
  file systems would alias `Carlos` and `carlos` anyway; normalizing makes
  every platform agree, and the admin list (`--settings_admins`) is
  normalized the same way so checks still match.
- **Persistence keyed to local-only transports by default (7.4)**, not
  always on: keeps the opt-in rule `src/auth/DESIGN.md` set for exposing
  persistent, client-named directories over a network.
- **Logs, screenshots and traces in the home, not the working
  directory.** CWD-relative directories scatter across wherever the server
  was started and pollute project directories (the reason `--renderer none`
  already moved its log to temp).
- **No backward compatibility.** Old flags and setters are removed, not
  deprecated, and files in the old layout (`server_store.bison`,
  `users/<id>.bison`, `desktop/desktop.yaml`, `--sandbox_root` trees) are
  neither read nor moved: wish starts with an empty profile in the new
  layout. Keeping aliases and a migration step would preserve the very
  inconsistency this refactor removes, for little benefit.

## 9. Constraints and Invariants

- Only `wish_home` derives paths under the root; per-identity paths exist
  only for a normalized, valid identity.
- `context::identity` is always normalized or empty; empty means
  anonymous everywhere (no user store, temp sandbox, not an admin).
- An anonymous session (no identity, `--anonymous`, or an identity that
  fails normalization) always gets the OS-temp sandbox
  `wish_<session id>`, deleted on disconnect. Nothing for it is written
  under the wish home.
- A persistent sandbox is never deleted by wish; a temp sandbox always is.
  Session temp dirs inside a persistent sandbox
  (`private/apps/.../tmp/<random>/`) are still deleted at session end.
- `set_home()` and `set_persistent_sandboxes()` are fixed once `start()`
  runs (the stores and the auth step depend on them).
- Concurrent sessions of one identity share its sandbox and user store,
  as today; tools must follow the `private/` rules in
  `src/auth/DESIGN.md`.
- Tests never use the real `~/.wish`: every wish test binary links
  `tests/wish_test_main.cpp`, which points `WISH_HOME` at a fresh
  per-process directory, and the Python binding tests import
  `bindings/python/tests/_wish_home.py`. Tests that inspect files pass
  their own `wish_home` (named after the gtest test), and tests that
  expect an anonymous session through `run()` pass `anonymous: true`.
- Native Windows: paths are built as `std::filesystem::path` (wide), the
  OS user name is converted from UTF-16, and no POSIX-only call is used
  outside a narrow `#if defined(_WIN32)` guard in `os_user.cpp`.

## 10. Integration Boundaries

- **Depends on:** `bison::rmi::auth_module_iface` and connect params
  (unchanged), `std::filesystem`, the OS user APIs.
- **Used by:** `wish::server`/`on_authenticated()`, `wish::standalone`,
  `store_registry`, the logger setup in `app/wish_cli`, the server
  overlay (paths shown in the info tab, sandbox/log/screenshot shortcuts,
  identity list), the profiler, `wish desktop` (config path, app logs), `wish_server_c.cpp`, `wish_client_c.cpp` and
  through it every binding.
- **User docs:** `docs/wish-home.md` (layout and flags),
  `docs/persistent-store.md`, `docs/cli.md`.
