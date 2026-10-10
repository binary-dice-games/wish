# Persistent Stores

wish gives tools two places to keep data between sessions and server restarts:

| Store | Scope | Who can reach it | File |
|-------|-------|------------------|------|
| **Server store** | One per server, shared by every session | Server-side code only (forms, `wish::server` subclasses) | `<store_dir>/server_store.bison` |
| **User store** | One per authenticated identity | That identity's clients (every client API) and server-side code handling its sessions | `<store_dir>/users/<identity>.bison` |

`<store_dir>` defaults to `~/.wish` on the server machine (`$HOME`, or
`%USERPROFILE%` on native Windows). Change it with `wish server --store_dir`,
`wish standalone --store_dir`, `wish::server::set_store_dir()`,
`wish::standalone::set_store_dir()`, or the `store_dir` param of
`wish_server_start()`.

Each store holds **named entries**, and each entry is a bison object
(`bison::dynamic`). Nested objects, typed fields (`bool`, `int32_t`, `float`,
`std::string`, `key_t`, vectors) and hashed field names all round-trip exactly.

## Identity and anonymous sessions

A session has a user store only if it has an **identity**. The identity comes
from the server's auth module when the client connects:

- `wish server` (CLI) and `wish_server_start()` (C ABI) always install
  `wish::local_auth_module`. It trusts the client's `username` connect field
  (`wish client --username NAME`, or `{"username": ...}` in the connect params
  of any binding).
- A client without a username is **anonymous** and has no user store. The
  exception is `wish server --sandbox_root`, which keeps its `default`
  identity fallback, so every client gets one.
- `wish standalone` is single-user. It always uses `--username`, or `default`
  when none is given.
- An embedder that calls `wish::server::start()` with no auth module has only
  anonymous sessions. Pass an `auth_module_iface` to identify clients (see
  [src/auth/DESIGN.md](../src/auth/DESIGN.md)).

Identities must be a single safe path segment: no `/`, `\` or `..`. Any other
identity is treated as anonymous.

`local_auth_module` trusts whatever name the client sends. Use it only for
local, single-user or trusted deployments. A remote or multi-tenant server
must supply an auth module that verifies identities. Otherwise one client can
claim another's name and read that user's store.

## Security model

- **Clients reach only their own user store.** The `__WishUserStore` RMI
  service is bound to the session's identity when the client authenticates.
  Its methods take an entry name, never a store or a user, so a client cannot
  name another user's store.
- **The server store is never reachable over RMI.** No RMI class wraps it.
  Only C++ code running in the server process can use it: `server_store()` on
  `wish::server`/`wish::standalone`, or `context::server_store`.
- Entry names are 1-256 printable ASCII characters and are never used as
  file paths.

## Naming entries

All tools share one user store, so prefix entry names with the tool's
qualified module name, e.g. `bdg.desktop.tail` or `bdg.dev.curl.history`.
Keep one entry per logical setting group rather than one huge entry: each
write rewrites the whole store file, and keeping entries small also keeps
concurrent tools from overwriting each other's data.

The same rule applies to files: a tool keeps its own files in its private
sandbox directory, `private/apps/<org>.<collection>.<name>/`, and scratch
copies in a session-owned temp dir from `create_temp_dir()`, never at the
shared sandbox root (see `src/auth/DESIGN.md`).

## Durability and concurrency

- Each store file is loaded once, when it is first used, and rewritten after
  every `set`/`erase`. A call returns only after the data has reached disk.
  The write is atomic: the new content goes to `<file>.tmp`, which is then
  renamed over the old file.
- A file that can't be read or parsed is renamed to `<file>.corrupt` (so its
  data isn't lost) and the store starts empty.
- Concurrent sessions of the same identity share one in-memory store, so
  their writes don't overwrite each other.
- The stores are thread-safe and carry their own lock. Server-side code
  doesn't need the session lock to use them.
- Two server *processes* sharing one `store_dir` don't coordinate: the last
  writer wins. Give each server its own `--store_dir` if several run at once.
- Files are created only on the first write. A server that never writes
  creates nothing.

The file format is a 9-byte `WISHSTORE` magic, a format-version byte (`1`),
then one bison object in standard binary format (see bison's `FORMAT.md`).
That object is an array with one `{name, value}` record per entry.

## Client API

Every method except `has_user_store()` fails when the session is anonymous:

- C++ throws `std::logic_error`.
- The C ABI returns `WISH_ERR_UNAVAILABLE`.
- Each binding raises its own error type carrying that code.

| Operation | C++ (`wish::client`, `wish_app_host`, `wish::standalone`) | C ABI | Python | C# | Rust | Go | Java (Android) |
|---|---|---|---|---|---|---|---|
| Available? | `has_user_store()` | `wish_user_store_available` | `has_user_store()` | `HasUserStore` | `has_user_store()` | `HasUserStore()` | `hasUserStore()` |
| Read | `user_store_get(name)` → `optional<dynamic>` | `wish_user_store_get` (`WISH_ERR_NOT_FOUND` if missing) | `user_store_get(name)` → `Dynamic \| None` | `UserStoreGet(name)` → `Dynamic?` | `user_store_get(name)` → `Option<Value>` | `UserStoreGet(name)` → `*Value` (nil if missing) | `userStoreGet(name)` → `Dynamic` (null if missing) |
| Write | `user_store_set(name, dynamic)` | `wish_user_store_set` | `user_store_set(name, Dynamic \| dict)` | `UserStoreSet(name, Dynamic \| IDictionary)` | `user_store_set(name, &Value)` | `UserStoreSet(name, *Value)` | `userStoreSet(name, Dynamic)` |
| Delete | `user_store_erase(name)` → `bool` | `wish_user_store_erase` | `user_store_erase(name)` | `UserStoreErase(name)` | `user_store_erase(name)` | `UserStoreErase(name)` | `userStoreErase(name)` |
| List | `user_store_keys()` → sorted names | `wish_user_store_keys` (JSON array) | `user_store_keys()` | `UserStoreKeys()` | `user_store_keys()` | `UserStoreKeys()` | `userStoreKeys()` |

The C++ methods return `std::future`s, like the other `wish::client` helpers.
The C ABI and the bindings block until the call completes.

### C++ (module client code)

```cpp
void run_tail(wish_app_host& host) {
  std::string filter;
  if (host.has_user_store()) {
    if (auto saved = host.user_store_get("bdg.desktop.tail").get())
      filter = saved->get_as<std::string>("filter"_key, "");
  }
  // ... later, when the filter changes:
  bison::dynamic settings;
  settings["filter"_key] = filter;
  if (host.has_user_store())
    host.user_store_set("bdg.desktop.tail", std::move(settings)).get();
}
```

### C++ (server side)

```cpp
// Inside a form or server subclass -- no session lock needed for the stores.
if (sess().user_store)
  sess().user_store->set("bdg.desktop.tail", settings);
sess().server_store->set("bdg.desktop.tail.defaults", defaults);
```

### Python

```python
client = wish.Client.tcp("127.0.0.1", 7075)

def session(c):
    if c.has_user_store():
        c.user_store_set("my.tool", {"filter": "error|warn", "lines": 200})
        saved = c.user_store_get("my.tool")
        print(saved["filter"])

client.run(session, params={"username": "alice"})
```

## Related

- [src/auth/DESIGN.md](../src/auth/DESIGN.md): auth modules, identities and
  persistent sandbox directories.
- [docs/cli.md](cli.md): `--store_dir` and `--username`.
- [docs/bindings.md](bindings.md): per-language setup.
