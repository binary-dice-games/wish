# Persistent Stores

wish gives tools two places to keep data between sessions and server restarts:

| Store | Scope | Who can reach it | File |
|-------|-------|------------------|------|
| **Server store** | One per server, shared by every session | Server-side code only (forms, `wish::server` subclasses) | `<wish_home>/server/store.bison` |
| **User store** | One per authenticated identity | That identity's clients (every client API) and server-side code handling its sessions | `<wish_home>/users/<identity>/store.bison` |

`<wish_home>` defaults to `~/.wish` on the server machine (`$WISH_HOME`, else
`$HOME`, or `%USERPROFILE%` on native Windows). Change it with
`--wish_home` (`wish server`, `wish standalone`, `wish desktop`),
`wish::server::set_home()`, `wish::standalone::set_home()`, or the
`wish_home` param of `wish_server_start()`. See [wish-home.md](wish-home.md)
for everything else kept there.

Each store holds **named entries**, and each entry is a bison object
(`bison::dynamic`). Nested objects, typed fields (`bool`, `int32_t`, `float`,
`std::string`, `key_t`, vectors) and hashed field names all round-trip exactly.

## Identity and anonymous sessions

A session has a user store only if it has an **identity**. The identity comes
from the server's auth module when the client connects:

- `wish server` (CLI) and `wish_server_start()` (C ABI) always install
  `wish::local_auth_module`. It trusts the client's `username` connect field.
- Clients send the logged-in **OS user** by default (`wish::client::run()`,
  the C ABI and so every binding, `wish client`). Override it with
  `wish client --username NAME` or `{"username": ...}` in the connect params.
- A client that passes `--anonymous` (`{"anonymous": true}`), or a server
  with no auth module, gives an **anonymous** session: no user store.
- `wish standalone` is single-user. It uses `--username`, else the OS user,
  unless `--anonymous`.
- An embedder that calls `wish::server::start()` with no auth module has only
  anonymous sessions. Pass an `auth_module_iface` to identify clients (see
  [src/auth/DESIGN.md](../src/auth/DESIGN.md)).

Identities are normalized before use: ASCII letters are lowercased and
spaces become `_`, so `Alice` and `alice` share one profile. The result must
be 1-64 characters of `[a-z0-9._-]`, not starting with `.`; any other
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
- The **Settings** tool (`bdg/desktop/conf`) is server-side code: it edits
  the server store in-process, and only for sessions whose identity is a
  settings admin (`wish server --settings_admins`). See
  [docs/settings.md](settings.md).

The wish CLI itself keeps the defaults saved with `wish server
--save_defaults` / `wish standalone --save_defaults` in the server store,
under `wish.cli.server` and `wish.cli.standalone` (one field per flag; see
[docs/cli.md](cli.md#saved-flag-defaults)).

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
- Two server *processes* sharing one wish home don't coordinate: the last
  writer wins. Give each server its own `--wish_home` if several run at once.
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

- [docs/settings.md](settings.md): describing settings kept in the stores
  so the Settings tool can edit them.
- [src/auth/DESIGN.md](../src/auth/DESIGN.md): auth modules, identities and
  persistent sandbox directories.
- [docs/cli.md](cli.md): `--wish_home`, `--username` and `--anonymous`.
- [docs/bindings.md](bindings.md): per-language setup.
