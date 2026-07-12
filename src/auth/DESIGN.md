# wish Optional Auth Module & Persistent Sandbox Directories — Architecture & Design

**Status: implemented.** See `src/auth/local_auth_module.hpp`/`.cpp`,
`bison::rmi::auth_module_iface` (`extern/bison/src/rmi/server/auth.hpp`),
`wish::context::resource_dir_persistent`/`populate_resource_dir()`
(`src/context/context.hpp`/`.cpp`), and `wish::server::start()`/
`set_persistent_sandbox_root()`/`on_authenticated()`
(`src/server/server.hpp`/`.cpp`). One deviation from the original brief
below: `auth_module` ended up as a parameter of `server::listen()` /
`wish::server::start()` rather than a `set_auth_module()` setter (see
"Key Abstractions" below) -- the module cannot sensibly change once the
accept loop is running, so a setter with a "must call before start()"
convention-only contract was replaced with a compile-time-enforced
parameter instead. Everything else matches this document as designed.

## Overview

Every wish session currently gets a throwaway temp directory
(`context::context`, `src/context/context.cpp:55-73`), deleted on
disconnect (`context::~context`, `context.cpp:75-81`). Files a client
uploads via `client::upload_file` (`src/client/client.hpp:121-122` →
`file_service::upload`) therefore vanish at the end of every run. For
local/single-machine deployments (server and client on the same box, or a
deployment with a stable per-user identity) it is useful for the sandbox
directory to persist across runs, so previously uploaded files are still
there next time — clients don't have to re-upload on every reconnect.

This must be **opt-in and privacy-conscious**: an operator must explicitly
enable persistence (mirroring the existing `set_allow_absolute_paths`
opt-in for absolute paths, `src/server/server.hpp:76-88`), and a session
must only get a persistent, identity-keyed directory when the server has
some trustworthy way to know *whose* directory it is. That requires a
notion of connection identity that does not exist anywhere in bison or wish
today — hence the auth-module piece below. bison's own `src/rmi/DESIGN.md`
lists "Auth hooks and capability negotiation" as **Future** work (line
732); this feature is the first consumer of that hook.

---

## Design Goals

1. **Off by default.** No behavior change for any existing server/client
   unless both an auth module and a persistent sandbox root are explicitly
   configured.
2. **No new wire protocol.** Reuse the connect-time `dynamic` payload that
   already exists end-to-end, rather than inventing a new handshake
   message or envelope field.
3. **Minimal, reusable primitive in bison.** bison gets a generic, policy-free
   `auth_module_iface` (accept/reject + optional identity string) — it has
   no opinion on what "identity" means. wish is the one that interprets an
   identity string as "a directory name".
4. **Sandbox-escape safety carries over.** An identity string becomes a
   path segment under an operator-controlled root; it must be validated
   with the same rigor as any other client-influenced path component (see
   `file_service::resolve_path`, `src/context/file_service.cpp`).
5. **No embedder-facing API breakage.** Every new capability is additive:
   new opt-in setters/overloads, no existing signature changes.

---

## Architecture

```
Client                                   Server
------                                   ------
client.connect(params)                   client_worker (per connection)
  or wish::client.run(params)               on_create_context(session_id)
                                                → wish::context ctor
                                                  (temp resource_dir, as today)
  transport_.open(params)                    session registered in session_contexts_
  send OP_CONNECT { ...params,                on_session_created(ctx)
                     __version }                 → file_service::instantiate(temp resource_dir)
                                                   (as today, unconditionally)
                                              handle_connect(ctx, env)
                                                if auth_module_ set:
                                                  ok = auth_module_->authenticate(
                                                         ctx, env.payload, identity)
                                                  if !ok: reject (ERR_ACCESS_DENIED)
                                                  else:   on_authenticated(ctx, identity)
                                                            (wish::server override)
                                                            if persistent_sandbox_root_ set
                                                               && !identity.empty():
                                                              validate identity
                                                                (no '/', '\', "..")
                                                              resource_dir =
                                                                persistent_sandbox_root_/identity
                                                              re-populate res/ assets
                                                              file_service re-instantiated
                                                                against new resource_dir
                                                respond OP_CONNECT ack
  connect() future resolves
  (fs_proxy_ instantiated on first use,
   resolves to whichever file_service
   is live at that point)
```

Key ordering fact driving this design: `on_create_context`/
`on_session_created` fire **before** the client's `OP_CONNECT` payload is
even parsed (`client_worker`, `bison/src/rmi/server/server.cpp:287-368`).
`OP_CONNECT` is dispatched like any other request, after context creation.
So a persistent-directory decision cannot be made at context-construction
time; it can only be made once `handle_connect` sees the payload. The
context's `resource_dir` and `file_service` must therefore support being
**switched after construction**, not just set once.

---

## Key Abstractions

### `bison::rmi::auth_module_iface` (new, bison)

```cpp
// src/rmi/server/auth.hpp
class auth_module_iface {
 public:
  virtual ~auth_module_iface() = default;

  /// Called from handle_connect() once the client's connect payload has
  /// arrived. Returns true to accept the session; false to reject it
  /// (server responds with ERR_ACCESS_DENIED and the client's connect()
  /// future throws). out_identity may be set to a stable identity string
  /// for the caller to use as it sees fit -- bison attaches no semantics
  /// to it.
  virtual bool authenticate(context& ctx, const dynamic& payload,
                             std::string& out_identity) = 0;
};
using auth_module_ptr = std::shared_ptr<auth_module_iface>;
```

`bison::rmi::server` additions:
- `auth_module_ptr auth_module_;` set by `listen(dynamic params = {},
  auth_module_ptr auth_module = nullptr)`'s new second parameter --
  **implemented as a `listen()` parameter, not a `set_auth_module()`
  setter** as originally sketched above. The module cannot sensibly change
  once the accept loop it gates is already running, so there is no
  meaningful "set it again later" use case; a parameter makes that
  invariant compile-time-enforced instead of a "must call before start()"
  convention (the pattern `set_logger`/`set_allow_absolute_paths` use).
  `nullptr` (default) disables the feature, so existing `listen()` call
  sites are unaffected.
- New no-op-default virtual hook `on_authenticated(context& ctx, const
  std::string& identity)`, fired only when `auth_module_` is set and
  `authenticate()` returned `true`. This is the extension point wish uses.
- `handle_connect()`: if `auth_module_` is set, call
  `authenticate(ctx, env.payload, identity)`; on failure, respond with the
  existing `ERR_ACCESS_DENIED` constant (`src/rmi/shared/constants.hpp` --
  previously defined but unused anywhere) instead of the normal ack; on
  success call `on_authenticated(ctx, identity)` before the ack. With no
  `auth_module_` set, `handle_connect` is byte-for-byte unchanged.

No envelope/wire format changes: this reuses the existing connect payload
`dynamic`, so it is forward-compatible with old clients (they simply don't
send extra fields, and `authenticate()` sees an empty payload).

### Reusing `client::connect(dynamic params)` (bison) instead of a new hook

`client::connect(bison::dynamic params)` (`src/rmi/client/client.cpp:36-48`)
already accepts a caller-supplied `dynamic`. Today it is only forwarded to
the transport's `open(params)` (transport-specific config, e.g.
host/port); the separate `OP_CONNECT` payload sent to the server is a
second, unrelated local `dynamic` hardcoded to just `FIELD_VERSION`. No
concrete transport (`socket_client_transport::open`, etc.) reads any field
out of `params` today, so there is nothing to conflict with.

`connect()` is changed to forward the caller's `params` into *both*
places — transport `open()` and the `OP_CONNECT` payload:

```cpp
void client::connect(bison::dynamic params) {
  shared::register_all_schemas();
  bison::dynamic payload = params;               // copy before transport consumes it
  transport_.withRLock([&](auto& t) { t->open(std::move(params)); });
  running_.store(true);
  worker_ = std::thread(&client::worker_loop, this);
  event_thread_ = std::thread(&client::event_loop, this);
  payload[FIELD_VERSION] = int32_t{PROTOCOL_VERSION};
  auto f = send_request(OP_CONNECT, {}, std::move(payload), false);
  f.get();
  on_connect();
}
```

This is a deliberate rejection of a "connect payload hook" design: `params`
was already designed as an open-ended dynamic extension point, so the
right move is to use it for both purposes rather than add a parallel
mechanism. A caller wanting persistent sandboxes does
`client.connect({{"username"_key, "alice"}})`.

`wish::client::run()` (`src/client/client.hpp:64`, impl
`src/client/client.cpp:18-27`) gets the same treatment: an optional
pass-through parameter instead of a new setter —
```cpp
void run(bison::dynamic connect_params = {});
```
forwarding to `connect(std::move(connect_params))`.

### `wish::context` changes

Add one field:
```cpp
bool resource_dir_persistent = false;
```
The destructor (`context.cpp:75-81`) skips `remove_all` when this is
`true` — a persistent directory must survive the session, by definition.

Factor the temp-dir population logic currently inline in the constructor
(`context.cpp:59-72`: `create_directories` + `resource_store::extract_to` +
re-keying `embedded_crc32s`) into a small private helper, e.g.
`populate_resource_dir()`, reused both for the default temp dir and for a
persistent dir switched in later. This avoids duplicating that logic
rather than inventing a second code path for the persistent case.

### `wish::server` changes

- `start(bison::rmi::auth_module_ptr auth_module = nullptr)` — the auth
  module is a parameter of `start()` (forwarded to the base
  `bison::rmi::server::listen()`), not a setter, for the same reason as
  the bison-level change above: it cannot change once the accept loop is
  running. `nullptr` (default) disables the feature.
- `set_persistent_sandbox_root(std::filesystem::path root)` — a setter,
  same "must be called before `start()`" contract as `set_logger`/
  `set_allow_absolute_paths` (unlike the auth module, there's no strong
  reason to also make this a `start()` parameter, so it stays consistent
  with existing config setters). Stores `persistent_sandbox_root_`
  (default empty = feature disabled). **This is the privacy flag**:
  persistent, identity-keyed directories are only ever created when an
  operator has explicitly opted in with a root path, mirroring how
  `allow_absolute_paths` gates absolute-path widget references.

New override:
```cpp
void on_authenticated(bison::rmi::context& ctx, const std::string& identity) override final;
```
Unlike `on_session_created`/`on_session_destroyed`, this is a single
private `override final` -- there is no further wish-level protected hook
exposed to `wish::server` subclasses, since the design calls for exactly
one policy (switch to a persistent, identity-keyed sandbox) with no
per-application variation point. No-op if
`persistent_sandbox_root_` is empty or `identity` is empty (falls back to
the default temp directory for that session — a client that supplies no
identity, or a deployment with no persistent root configured, sees no
behavior change). Otherwise:
1. Validate `identity` is a single safe path segment: non-empty, contains
   no `/`, `\`, or `..`. This is deliberately simpler than
   `file_service::resolve_path`'s nested-relative-path escape check, since
   an identity is exactly one path segment, not an arbitrary relative
   path — but it exists for the same reason: an identity string ultimately
   originates from client-supplied connect-payload data (via whatever
   `auth_module_iface` extracted it), so it must never be trusted as a
   pre-sanitized path component.
2. `new_dir = persistent_sandbox_root_ / identity`; `create_directories`.
3. On the concrete `wish::context&` (`static_cast` from the base
   `bison::rmi::context&`, same pattern `on_session_created`'s bridge
   already uses): set `resource_dir = new_dir`,
   `resource_dir_persistent = true`, call `populate_resource_dir()` again
   so `res/` assets exist under the persistent dir too. Previously
   uploaded user files are untouched, since extraction only touches the
   `res/` subfolder (see `src/resources/DESIGN.md`'s "Relationship to
   file_service" section for why that convention is safe).
4. Re-instantiate `ctx.file_service = file_service::instantiate(ctx.resource_dir)`
   so the singleton `__WishFileSystem` object subsequently handed out by
   `on_create_object`/`find_singleton_service` (`context.cpp:29-41`)
   points at the persistent directory.

   `file_service` captures `resource_dir_` by value, and per the
   session-threading rules in the repo's `CLAUDE.md`, a `session&`/
   `context&` must never be stored long-lived. Re-instantiating this small
   object is therefore simpler and more correct than trying to make it
   track a mutable path by reference. This is safe to do inside
   `handle_connect`'s dispatch because `on_before_dispatch` already holds
   the session's `wlock` for the whole message, per the documented
   threading model.
5. `on_session_created` (`server.cpp:69-85`) itself is **not** changed — it
   still unconditionally builds the default temp-dir-backed `file_service`
   first, exactly as today. `on_authenticated` (which fires later, once
   the connect request is dispatched) simply replaces it if persistence
   kicks in for that session. This keeps the common (non-persistent) path
   completely untouched and avoids conditional logic inside the
   already-dense `on_session_created`.

### `wish::local_auth_module` (new, wish) — a ready-to-use trust-the-client module

To make the "run everything locally" use case usable without every
embedding application writing its own `auth_module_iface`:

```cpp
// src/auth/local_auth_module.hpp
// Accepts every connection; extracts a client-supplied "username" field
// from the connect payload as the identity. Does NOT verify the client's
// claim -- suitable only for trusted, local, or single-user deployments.
// Do not use where clients are untrusted or remote.
class local_auth_module : public bison::rmi::auth_module_iface {
  bool authenticate(bison::rmi::context&, const bison::dynamic& payload,
                     std::string& out_identity) override;
};
```
Reads a `"username"_key` field out of `payload` if present, else leaves
`out_identity` empty (session falls back to a non-persistent temp dir).
Always returns `true` — its only job is identity extraction, not
gatekeeping. This is deliberately the simplest possible module; anything
that needs real credential verification is expected to supply its own
`auth_module_iface` implementation (e.g. checking a signed token), which
this design's hook fully supports without further bison/wish changes.

---

## Public API Contract

| Symbol | Layer | Contract |
|---|---|---|
| `bison::rmi::auth_module_iface::authenticate` | bison | Pure policy hook: accept/reject + optional identity string. No I/O side effects implied; bison does not persist or interpret the identity. |
| `bison::rmi::server::listen`'s `auth_module` parameter | bison | Parameter, not a setter -- passed to `listen()` when the accept loop starts. `nullptr` (default) disables the feature — `handle_connect` behaves exactly as before. |
| `bison::rmi::server::on_authenticated` | bison | Default no-op virtual; fires once per successful `authenticate()`, never for rejected or auth-disabled connections. |
| `client::connect(dynamic params)` / `wish::client::run(dynamic params)` | bison / wish | `params` is forwarded to *both* the transport's `open()` and the server's connect-handshake payload. Existing callers passing no params see no behavior change. |
| `wish::server::start`'s `auth_module` parameter | wish | Parameter, not a setter, mirroring `bison::rmi::server::listen()`. Forwarded unchanged to the base `listen()`. |
| `wish::server::set_persistent_sandbox_root` | wish | Must be called before `start()`. Empty path (default) disables persistence entirely regardless of auth module / identity. |
| `wish::context::resource_dir_persistent` | wish | When true, the destructor does not delete `resource_dir`. Only ever set by `on_authenticated`, never by application code directly. |
| `rmi_client_connect(rmi_client_handle, bison_handle params)` | bison C ABI | Fixed as part of this work: previously discarded `params` and always connected with an empty `dynamic` (a pre-existing bug this design's "no ABI gap" claim below had assumed was not present); now forwards `params` via `bison_handle_to_dynamic()`, matching what the Python `Client.connect(params)` binding already promised. |
| `wish::local_auth_module` | wish | Optional convenience; never rejects; not a substitute for real authentication in untrusted deployments. |

---

## Public C ABI / Python Bindings

- `wish::server`'s existing config setters (`set_logger`,
  `set_allow_absolute_paths`) have **zero** ABI/Python exposure today —
  `wish::server` is not reachable from any C ABI at all; only bare
  `bison::rmi::server` lifecycle (`rmi_server_tcp_create`/`_listen`/`_stop`/
  `_release`) is exposed, with no way to reach virtual hooks, subclass
  setters, or (now) `start()`'s `auth_module`/`set_persistent_sandbox_root`.
  **No ABI/binding gap is introduced** for the server-config side; nothing
  needed to change there.
- bison's raw client ABI plumbs `dynamic` params end-to-end:
  `rmi_client_connect(rmi_client_handle, bison_handle params)` and Python
  `Client.connect(params)` (via the `_as_params` helper). **This needed a
  fix as part of this work**: `rmi_client_connect`'s C++ implementation was
  actually discarding `params` and always connecting with an empty
  `dynamic`, despite the Python binding faithfully forwarding whatever the
  caller passed — a pre-existing bug, now fixed (see the Public API
  Contract table above). Since the `connect()` change above alters
  *behavior*, not *signature*, callers already using bison's own C
  ABI/Python client can pass auth fields today with no further change.
- **The actual gap, now closed**: wish's own higher-level client
  ABI/Python wrapper didn't expose connect params.
  `wish_client_run_with_params(wish_client_handle client, wish_session_fn
  session_fn, void* userdata, bison_handle connect_params)` has been added
  to `include/wish_client_c.h`/`src/wish_client_c.cpp`, converting
  `connect_params` via the existing `bison_handle_to_dynamic()` helper and
  calling `wish::client::run(dynamic)`. `wish_client_run` is now a thin
  wrapper calling the new function with a null params handle — no behavior
  change for existing callers.

  `bindings/python/wish/client.py`'s `Client.run` takes an optional
  `params` argument (dict / `bison.Dynamic` / `None`), mirroring bison's
  own `Client.connect(params)` / `_as_params` conversion pattern, and
  always calls the new `wish_client_run_with_params` ABI entry (passing a
  null handle when `params` is `None`, so the default path is untouched).
  `bindings/python/wish/_native.py` declares the new function's `ctypes`
  signature alongside the existing ones.

---

## Sandbox / Security Considerations

- **Identity is client-influenced, not client-trusted.** `local_auth_module`
  takes the client's claimed username at face value; it is explicitly
  documented as unsuitable for untrusted/remote deployments. Any
  deployment where clients are not fully trusted must supply an
  `auth_module_iface` that actually verifies the claim (e.g. against a
  token) before deriving a directory from it.
- **Path escape via identity.** An identity string becomes one path
  segment under `persistent_sandbox_root_`; it is rejected outright if it
  contains `/`, `\`, or `..`, exactly the same class of check
  `file_service::resolve_path` (`src/context/file_service.cpp`) already
  applies to client-supplied file names — this is a second, independent
  instance of the same escape-prevention principle, not a reused code
  path, since the two check different shapes of input (a single segment
  vs. a possibly-nested relative path).
- **Opt-in twice over.** Both `set_persistent_sandbox_root` (a path) and a
  non-empty `identity` from a set `auth_module_`  are required before any
  session gets a directory outside the default temp location. Absence of
  either falls back to today's behavior, so this feature cannot be
  triggered by accident or by an old client that sends no extra connect
  fields.
- **No collisions with existing sandbox contents.** `populate_resource_dir()`
  only ever writes under `resource_dir/res/` (see
  `src/resources/DESIGN.md`), so re-running it against a persistent
  directory that already has previously-uploaded files at its top level
  cannot clobber them.

---

## Tests

- bison (`tests/rmi_tests.cpp`, `RmiAuth` suite): `authenticate()`
  returning `false` → the client's `connect()` future throws, and the
  session never reaches `on_authenticated`
  (`RejectingModuleFailsConnectAndSkipsOnAuthenticated`); returning `true`
  → `on_authenticated` fires with the right identity
  (`AcceptingModuleFiresOnAuthenticatedWithIdentity`); no auth module set →
  behavior identical to today (`NoModuleSetBehavesUnchanged`).
- wish (`tests/test_auth.cpp`, `AuthTest` suite): with
  `set_persistent_sandbox_root` unset, behavior is unchanged (temp dir,
  deleted on disconnect) even with an auth module set
  (`NoPersistentRootConfiguredBehavesUnchangedEvenWithAuthModule`), and
  with no auth module at all (`NoAuthModuleBehavesUnchanged`). With a root
  set + `local_auth_module` + a client calling
  `run({{"username"_key, "alice"}})`: upload a file, disconnect, reconnect
  with the same params — the file is still there via `download`
  (`UploadPersistsAcrossReconnectWithSameIdentity`). Reconnect with a
  different username — the file is *not* visible
  (`DifferentIdentityDoesNotSeeAnotherIdentitysFiles`). No identity at all
  → non-persistent, same as before (`NoIdentityFallsBackToNonPersistentTempDir`).
  An identity containing `..`/`/` is rejected and does not persist
  (`PathEscapingIdentityIsRejectedAndDoesNotPersist`).
- wish (`tests/test_context.cpp`): `resource_dir_persistent` defaults
  `false`; the destructor skips removal when it's `true`;
  `populate_resource_dir()` can be called again after `resource_dir`
  changes and re-populates `res/` + `embedded_crc32s` at the new location.
- wish C ABI/Python (`bindings/python/tests/test_client.py`,
  `TestRunWithConnectParams` suite): `Client.run(session_fn, params=...)`
  marshals a dict or `bison.Dynamic` through `wish_client_run_with_params`
  without crashing (no live server in the Python binding test harness, so
  this exercises the FFI plumbing against an unreachable host rather than
  a real round trip -- the actual persistence round trip driven by these
  same params is what `tests/test_auth.cpp` exercises in C++).

## Verification

`tests/test_auth.cpp`'s `AuthTest` suite runs the wish server/client
integration test harness (`memory_transport`, same pattern as
`test_integration.cpp`) exercising: upload → disconnect → reconnect with
the same identity → download succeeds, using `set_persistent_sandbox_root`
pointed at a temp test directory the test itself cleans up in `TearDown`.

---

## Resolved Implementation-Pass Decisions

These were open questions during design; here's how each was resolved:

- **Identity-sanitizing helper**: `is_safe_identity(const std::string&)`,
  a private free function in an anonymous namespace in `src/server/server.cpp`,
  next to `on_authenticated`. Not a public API.
- **bison's `src/rmi/DESIGN.md`**: resolved as part of this change. § 10.1
  ("connect") now describes the `auth_module_iface` hook and § 15
  ("Implementation Status") lists it under "Completed" instead of "Future".
- **`README.md` / `docs/`**: `README.md`'s "Security Considerations for AI
  Code Assist" section gets a new subsection alongside the existing
  absolute-paths one, pointing at `set_persistent_sandbox_root` and
  `start()`'s `auth_module` parameter.
