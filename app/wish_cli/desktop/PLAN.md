# PLAN: Local wish-desktop discovery/notification mechanism

See [`app/DESIGN.md`](../../DESIGN.md)'s "desktop mode" section for the
existing architecture of `wish_desktop`/`wish_desktop_app` this plan extends.

**Status:** not started. No step below is implemented yet. Code references
name symbols rather than line numbers, because the line numbers in the first
draft of this plan went stale after the desktop redesign (#63).

## Problem

`wish desktop` multiplexes N downstream client connections into one upstream
wish server session. Today a downstream client can only connect if it already
knows the desktop's transport/host/port. Apps the desktop launches itself get
that from the `WISH_TRANSPORT`/`WISH_HOST`/`WISH_PORT`/`WISH_NAME` variables
`wish_desktop_app::make_bridge()` passes via `set_launch_env()`. A process
started *independently* of the desktop (a service, a daemon, a script started
at login) has no way to find a running desktop, or to learn that one has
started after the process did.

No discovery/broadcast/registry mechanism exists anywhere in wish or its
`extern/bison` dependency today (no UDP/multicast, no signaling socket, no
shared memory, no filesystem registry).

## Goal

A long-running process can get a UI whenever a wish desktop is running
locally, whether the desktop was already running when the process started,
starts afterward, or is closed and restarted later. Concretely:

- A process can register a callback, in C++, through the C ABI, and through
  every language binding, that fires with a desktop's connection info
  whenever a desktop becomes available locally, once per desktop instance.
  A desktop that is closed and restarted is a new instance and fires again.
- When the desktop closes, the process's client session ends: the existing
  `wish::client::on_disconnect()` / `c_abi_client::on_disconnect()` already
  unblock `wish_client_wait()`, and `wish_client_run()` resets the handle's
  quit flag, so the same handle can connect again when the next desktop
  appears.

## Confirmed design decisions (not open for re-litigation without cause)

1. **Discovery mechanism = polling filesystem registry.** The desktop writes
   a small JSON announce file to a per-user registry directory when it
   starts listening, and removes it on clean shutdown. A client-side
   background thread polls that directory (~500ms) to catch both orderings.
   No platform-specific file-watch APIs (inotify/ReadDirectoryChangesW/
   kqueue), in line with this repo's preference to avoid `#ifdef`-heavy
   platform branching.
2. **Multiple desktops supported.** One file per desktop *instance*; the
   callback fires once per instance.
3. **Full binding scope.** Core C++, C ABI, and wrapper APIs in every
   existing binding: cpp, python, csharp, android, **rust, go** (rust and go
   were added after the first draft of this plan).

## Corrections to the first draft (applied below)

These were defects in the first draft, not design changes:

- **Registry directory must be per-user.** The draft used
  `temp_directory_path() / "wish_desktops"` hardened to `0700`. On Linux and
  MSYS2 `/tmp` is shared, so the first user to create the directory owns it
  and every other user's desktops can neither announce nor be discovered.
  Another user could also pre-create it to block or spoof. Step 1 now uses a
  per-user path and verifies ownership.
- **File name keyed by pid alone collides.** Two `announcer`s in one process
  (Step 3's `FiresOncePerDesktopForMultipleDesktops`) would write the same
  `<pid>.json`, and a reused pid after a crash would look like a live
  desktop. Files are now named `<pid>-<instance_id>.json`, and the watcher
  deduplicates by `instance_id`.
- **Tests must not share the real registry.** ctest runs each test as its
  own process in parallel, so tests writing into the real per-user registry
  see each other's announcements, plus any real desktop the developer has
  running. All tests now point the registry at a per-test directory through
  an override (Step 1).
- **Reuse `connectable_host()`.** `wish_desktop_app.cpp` already maps a
  wildcard bind host to loopback for the launch environment. Step 2 moves it
  into `desktop_registry` and both call sites use it, instead of
  reimplementing the mapping.
- **Doc/binding references updated**: `docs/bindings.md` has no Android
  section, rust/go bindings exist, and the C ABI section for apps sits around
  `wish_list_apps()` in `include/wish_client_c.h`.

## Architecture

A new shared module in `src/client/` (part of the `wish_client` static
target in the root `CMakeLists.txt`, which both `wish-desktop` and
`wish_client_dll` link):

- **`src/client/desktop_registry.hpp/.cpp`**: registry directory, JSON
  schema, atomic write, ownership/permission checks, stale-entry reaping.
  Used by both the desktop-side announcer and the client-side watcher, so
  the file format is defined exactly once.
- **`src/client/desktop_watcher.hpp/.cpp`**: the polling background thread
  and callback registry (`bdg::wish::on_desktop_available`), built on
  `desktop_registry::list_live_desktops()`.

The desktop-side hook lives in `wish_desktop_app.hpp/.cpp` in this
directory.

---

## Step 1: Shared registry core

**Goal:** A dependency-free module can atomically publish one announce file
per desktop instance and list/reap the set of currently-live ones, in a
per-user directory that other local users cannot write to.

**Deliverables:**
- New `src/client/desktop_registry.hpp/.cpp`, namespace
  `bdg::wish::desktop_registry`:
  - `std::filesystem::path registry_dir()`:
    1. If the `WISH_DESKTOP_REGISTRY_DIR` environment variable is set, use
       it (tests, and users who want an explicit location).
    2. Else on POSIX (Linux, MSYS2): `$XDG_RUNTIME_DIR/wish/desktops` when
       `XDG_RUNTIME_DIR` is set (already per-user, `0700`, cleaned on
       logout), otherwise `temp_directory_path() / ("wish_desktops_" +
       std::to_string(geteuid()))`.
    3. Else on native Windows: `temp_directory_path() / "wish_desktops"`
       (`%TEMP%` is already per-user).
    Creates the directory on first use and sets it to owner-only
    (`perms::owner_all`, `perm_options::replace`). On POSIX it then
    `::stat()`s the directory and throws `std::runtime_error` if the owner
    is not `geteuid()` or the mode allows group/other access. This is the
    defense against another user pre-creating the path. Keep the
    platform-specific parts inside one narrow `#if defined(_WIN32)` guard.
  - `std::string connectable_host(const std::string& bind_host)`: moved
    here from the anonymous namespace in `wish_desktop_app.cpp` (wildcard
    `0.0.0.0`/`::`/`[::]`/empty → `127.0.0.1`/`::1`).
  - `struct info { int64_t pid; std::string instance_id, transport, host,
    name, user; int port; };`
    - `transport` is `"tcp" | "pipe" | "tls"`, the vocabulary of
      `--downstream_transport`.
    - `host`/`port` are set for tcp/tls (`host` already passed through
      `connectable_host()`); `name` is the pipe/Unix-socket path for pipe.
    - `instance_id` is a random hex id generated per `announcer`.
    - `user` is the desktop's upstream user (`wish_desktop::set_user()`),
      so a process can pick the desktop of the user it belongs to.
  - JSON schema written to `<pid>-<instance_id>.json`:
    `{"schema": 1, "pid": ..., "instance_id": "...", "transport": "...",
    "host": "...", "port": ..., "name": "...", "user": "..."}`. Readers
    ignore unknown fields and skip files with a higher `schema` major.
  - `class announcer` (RAII, movable, non-copyable): the constructor writes
    `<file>.tmp` with owner read/write permissions, then
    `std::filesystem::rename()`s it into place (the staging-file pattern of
    `src/context/file_service.cpp`'s `.part` suffix). The destructor removes
    the file, best effort.
  - `std::vector<info> list_live_desktops()`: scans `registry_dir()`, parses
    every `*.json`, and drops and best-effort-deletes any entry whose pid is
    not alive or whose file is malformed. On POSIX it also skips and reaps
    files whose owner UID is not `geteuid()`. Ignores `*.tmp` files.
  - `bool is_pid_alive(int64_t)`: one function with a narrow
    `#if defined(_WIN32)` guard (`OpenProcess`/`GetExitCodeProcess` on
    Windows; `::kill(pid, 0)` on POSIX, with `EPERM` counted as alive).
    `src/web/web_renderer.cpp` shows the existing `_getpid()`/`getpid()`
    guard style; there is no existing liveness helper to reuse.
  - `int64_t current_pid()`.
- Add the two files to `add_library(wish_client STATIC ...)` in the root
  `CMakeLists.txt`.

**Tests: new `tests/test_desktop_registry.cpp`.** The fixture sets
`WISH_DESKTOP_REGISTRY_DIR` to a directory named after
`current_test_info()->name()` plus a random suffix, and removes it in
`TearDown()`:
- `announcer` writes a well-formed file with owner-only permissions at the
  expected path and removes it on destruction.
- `list_live_desktops()` returns an entry published by the current process.
- Two `announcer`s in one process produce two files and two entries.
- A hand-written entry with a nonexistent pid is reaped and excluded.
- A malformed `.json` and a leftover `.tmp` are ignored.
- `connectable_host()` maps the wildcard forms to loopback.
- POSIX only: `registry_dir()` throws when the override points at a
  directory whose mode is `0777`.
- A comment notes that the cross-UID ownership check has no automated test,
  because it needs a second real UID.

---

## Step 2: Desktop-side announcer wiring

**Goal:** `wish desktop` publishes an announce file once the desktop is
fully up, for every downstream transport except `term`, and removes it
automatically on shutdown.

**Deliverables:**
- `wish_desktop_app.hpp`, class `wish_desktop`: add
  ```cpp
  void publish_announcement(const std::string& transport, const std::string& host,
                            int port, const std::string& name);
  ```
  and a private member
  `std::optional<desktop_registry::announcer> announcement_;`.
- `wish_desktop_app.cpp`:
  - Remove the local `connectable_host()` and use
    `desktop_registry::connectable_host()` in `make_bridge()`.
  - Implement `publish_announcement()`: return immediately for `"term"` (a
    spawned terminal has no socket another process could dial). Otherwise
    build an `info` (pid, new instance id, `connectable_host(host)`, the
    user from `set_user()`) and `announcement_.emplace(...)`. A failure to
    write the file is logged and does not stop the desktop.
  - In `wish_desktop_app::on_listening()`, after `desktop_->build_chrome()`
    (so a client that connects immediately finds the shell built):
    ```cpp
    desktop_->publish_announcement(FLAGS_downstream_transport, FLAGS_downstream_host,
                                   FLAGS_downstream_port, FLAGS_downstream_name);
    ```
  - In `wish_desktop::stop_tasks()`, reset `announcement_` first, so the
    file disappears before `wish_desktop_app::wait_for_shutdown()` starts
    tearing the desktop down and new clients stop being directed to it.
    `~wish_desktop()` already calls `stop_tasks()`.

**Tests: extend `tests/test_wish_desktop.cpp`** (`WishDesktopTest` fixture,
which also sets `WISH_DESKTOP_REGISTRY_DIR` to a per-test directory):
- `PublishAnnouncementWritesRegistryFile`: `publish_announcement("tcp",
  "0.0.0.0", 7071, "")`; `list_live_desktops()` has one entry with this pid,
  host `127.0.0.1`, port 7071 and the fixture's user.
- `PublishAnnouncementSkippedForTermTransport`: no file appears.
- `StopTasksRemovesAnnouncementFile`: publish, `stop_tasks()`, no file.

---

## Step 3: Client-side watcher core

**Goal:** `bdg::wish::on_desktop_available(callback)` fires, from a
background polling thread, once per desktop instance that is live at
registration time or becomes live afterward, and the registration can be
cancelled.

**Deliverables:**
- New `src/client/desktop_watcher.hpp/.cpp`:
  ```cpp
  namespace bdg::wish {
  using desktop_callback = std::function<void(const desktop_registry::info&)>;
  /// @return id for cancel_desktop_watch(); never 0.
  uint64_t on_desktop_available(desktop_callback callback);
  /// Unregisters; after it returns the callback is not running and will not
  /// run again. Must not be called from inside the callback.
  void cancel_desktop_watch(uint64_t id);
  }
  ```
  - State: `bison::synchronized<watcher_state>` (`bison_sync.hpp`; it has
    its own `wait_for`/`notify_all`, so no separate mutex or condition
    variable). `watcher_state` holds the callbacks keyed by id, and for
    each callback the set of `instance_id`s it has already been told about.
    Per-callback sets (instead of the first draft's global set plus a
    synchronous catch-up) make "late registration still sees an earlier
    desktop" fall out of the normal poll loop with no special case.
  - `on_desktop_available()`: adds the callback, starts the poll thread on
    first use (registering `stop_watcher()` with `std::atexit()` so the
    thread is joined before the function-local state is destroyed), and
    wakes the thread so already-live desktops are reported immediately.
  - `poll_loop()`: every `poll_interval()` (default 500ms; overridable via
    `detail::set_desktop_watcher_poll_interval_for_testing()`), calls
    `list_live_desktops()`. For each callback and each instance it hasn't
    seen, it records the instance and invokes the callback outside the
    lock. Instances that are no longer live are pruned from the sets.
  - Callbacks run on the watcher thread, one at a time. The doc comment
    says they must return quickly: running a client session inside one
    would delay every other notification and hang `stop_watcher()` at exit.
    Step 4 provides the session loop that does this properly.
- Add the files to the `wish_client` target.

**Tests: new `tests/test_desktop_watcher.cpp`.** A per-test
`WISH_DESKTOP_REGISTRY_DIR`, a few-ms poll interval, and a bounded (2s)
wait on a synchronized counter (never a polled plain `bool`):
- `FiresForAlreadyPublishedDesktop`.
- `FiresForDesktopPublishedLater`.
- `IgnoresStaleDeadPidEntries`: no callback, file reaped.
- `FiresOncePerInstanceForMultipleDesktops`: two announcers, exactly two
  firings with distinct instance ids, and no more after further polls.
- `LateRegistrationStillFiresForEarlierDesktop`: register A, publish,
  register B; both fire.
- `RestartedDesktopFiresAgain`: publish, wait for the firing, destroy the
  announcer, publish a new one; the callback fires a second time.
- `CancelStopsFurtherCallbacks`: cancel, publish; no firing.

---

## Step 4: Desktop session loop (C++)

**Goal:** A service gets a UI on every desktop that appears, with one call,
without writing the wait/connect/run/reconnect loop itself.

**Deliverables:**
- In `src/client/desktop_watcher.hpp/.cpp`:
  ```cpp
  /// Runs `session` once for each desktop instance that appears: connects a
  /// fresh `wish::client` to it, calls `session(client)`, and disconnects
  /// when `session` returns or the desktop closes. Returns an id for
  /// cancel_desktop_watch(); cancelling stops future sessions and quits the
  /// running ones.
  uint64_t run_on_each_desktop(std::function<void(client&)> session,
                               bison::dynamic connect_params = {});
  ```
  Each session runs on its own thread, so the watcher thread is never
  blocked. A failed connect (a desktop that died between the poll and the
  connect, or a reused pid) is logged and that instance is skipped.
  `connect_params` is passed through so the service can set the
  `app`/`app_label`/`app_icon` fields the desktop uses for its taskbar
  entry (see `docs/bindings.md`, "Connecting from the environment, and
  `wish desktop`").
- `std::unique_ptr<bison::rmi::transport::client_transport_iface>
  make_transport(const desktop_registry::info&)`: builds the tcp/pipe/tls
  client transport for an entry. Shared by this step and Step 5.

**Tests: extend `tests/test_desktop_watcher.cpp`** with a real
`wish::server` on a free port (`tests/free_port_server.hpp`) announced
through an `announcer`:
- The session callback runs and can build a UI element (assert via the
  server's tree, not a sleep).
- Stopping the server ends the session; announcing a new instance (same
  server restarted on a new free port) runs the session again.

---

## Step 5: C ABI

**Goal:** Discovery is usable through the C ABI with no existing client
handle, and a discovered desktop can be turned into a client handle.

**Deliverables:**
- `include/wish_client_c.h`, new section after `wish_list_apps()`:
  ```c
  typedef void (*wish_desktop_available_fn)(bison_handle info, void* userdata);
  WISH_API wish_error wish_on_desktop_available(
      wish_desktop_available_fn callback, void* userdata, uint64_t* out_watch_id);
  WISH_API wish_error wish_cancel_desktop_watch(uint64_t watch_id);
  WISH_API wish_client_handle wish_client_desktop_create(bison_handle info);
  ```
  Doc comments: the callback runs on a background thread and must return
  quickly; `info` is scope-local and valid only during the call (same
  contract as `rmi_proxy_event_fn`), read with `bison_get_*()` and never
  released by the caller; fields `pid`, `instance_id`, `transport`,
  `host`, `port`, `name`, `user`. `wish_client_desktop_create()` copies what
  it needs, so it may be called from inside the callback and the handle
  used later on another thread, typically with `wish_client_run()`.
- `src/wish_client_c.cpp`: wrap `on_desktop_available()`, building a
  `dynamic` per call and passing it with the scope-local
  `bison_dynamic_ptr`/`as_bison_handle` idiom of `rmi_proxy_on_event()` in
  bison's `src/rmi/rmi_c.cpp`. `wish_client_desktop_create()` reuses
  Step 4's `make_transport()` and the existing `make_client_handle()`.

**Tests:** a small gtest that calls the three functions against a per-test
registry directory and a real server: the callback fires, and the handle
from `wish_client_desktop_create()` connects with `wish_client_run()`.

---

## Steps 6-11: Language bindings

**Goal (each):** The binding exposes `on_desktop_available` with
cancellation and a "client from a discovered desktop" constructor, in that
language's naming style, with a test that a fake registry entry fires the
callback within a bounded wait.

All binding tests set `WISH_DESKTOP_REGISTRY_DIR` to a private temporary
directory before the first call, instead of recomputing the real registry
path. They write a fake entry using the current process's pid (so it counts
as alive). Callback objects are kept alive in a module/static map keyed by
watch id and released on cancel.

| Step | Binding | Deliverables | Test |
|---|---|---|---|
| 6 | cpp (`bindings/cpp/`) | New `include/wish_cpp/desktop.hpp`: `struct desktop_info`, `on_desktop_available()` returning a movable RAII `desktop_watch` that cancels on destruction, `client::from_desktop(const desktop_info&)`. Include it from `wish.hpp`. | `tests/test_client.cpp` |
| 7 | python (`bindings/python/`) | `DesktopAvailableFn` in `wish/_native.py` (next to `SessionFn`), new `wish/desktop.py` with `on_desktop_available(callback) -> watch` (`watch.cancel()`), `Client.from_desktop(info)` next to `Client.from_env()`. Export from `wish/__init__.py`. | new `tests/test_desktop.py` with `threading.Event().wait(timeout=2)` |
| 8 | csharp (`bindings/csharp/`) | `Native.cs` P/Invoke and delegate type, new `Wish/Desktop.cs`: `record DesktopInfo(...)`, `Desktop.OnDesktopAvailable(Action<DesktopInfo>)` returning an `IDisposable`, `Client.FromDesktop(DesktopInfo)`. | new `Wish.Tests/DesktopTests.cs` with a `TaskCompletionSource` |
| 9 | android (`bindings/android/`) | `DesktopInfo.java`, `DesktopAvailableCallback.java` (style of `AppInfo.java`), static `Client.onDesktopAvailable(...)` returning a `DesktopWatch` with `cancel()`, `Client.fromDesktop(DesktopInfo)`. JNI trampoline in `jni/wish_rmi_jni.cpp` mirroring `event_ctx`/`event_trampoline` (attach/detach thread, `NewGlobalRef` released on cancel, catch and log Java exceptions). | No instrumented test (needs an emulator; no existing JNI callback has one). Code review plus a manual smoke check. |
| 10 | rust (`bindings/rust/`) | `sys.rs` declarations; `desktop.rs` with `DesktopInfo`, `on_desktop_available(impl Fn(&DesktopInfo) + Send + 'static) -> DesktopWatch` (cancels on `Drop`), `Client::from_desktop()`. Re-export from `lib.rs`. | `tests/client_tests.rs` |
| 11 | go (`bindings/go/`) | C trampoline in `wish/shim.c`/`shim.h` (Go functions cannot be passed to C directly), `desktop.go` with `OnDesktopAvailable(func(DesktopInfo)) (*DesktopWatch, error)` using a `cgo.Handle`, `NewClientFromDesktop()`. | `wish/client_test.go` |

---

## Step 12: Documentation and changelog

**Goal:** The docs describe discovery accurately and no stale content
remains.

**Deliverables:**
- `app/DESIGN.md`, "desktop mode": add `publish_announcement()` after
  `build_chrome()` in the `on_listening()` line of the lifecycle diagram,
  note the file is removed in `stop_tasks()`, and add a short "Desktop
  discovery" paragraph (registry location, why `term` is skipped, pointer
  to `src/client/desktop_registry.hpp` for the schema).
- `docs/cli.md`, `## wish desktop`: a paragraph on the announcement (which
  transports, where the file lives, `WISH_DESKTOP_REGISTRY_DIR`).
- `docs/bindings.md`: a short "Desktop discovery" snippet in each existing
  per-language section (C++, Python, C#, Rust, Go) showing the
  "UI on every desktop" loop; add an Android section with the same snippet.
- A runnable example: `bindings/python/examples/desktop/service.py`, a
  background process that shows a status window on every desktop that
  appears; mention it in that folder's README and in `docs/examples.md`.
- `CHANGELOG.md`, `## [Unreleased]` / `### Added`: desktop discovery API
  and the announce file.
- `README.md`: no change unless a new `docs/` file is added.
- Do **not** create `src/client/DESIGN.md` speculatively.

---

## Proposed improvements (awaiting a decision)

These change scope or touch bison, so they are not folded into the steps
above until approved.

1. **Ephemeral downstream port.** The default `--downstream_port` is 7071,
   so a second desktop with default flags fails to listen and the "multiple
   desktops" goal only works with explicit ports or pipe names. With
   discovery, clients no longer need a well-known port, so the desktop
   could accept `--downstream_port=0` and announce the port it actually
   bound. `bison::rmi::transport::socket_server_transport` does not expose
   its bound port today, so this needs a small bison change (a `port()`
   accessor after `start()`), which needs approval per `CLAUDE.md`.
2. **TLS connection details.** For `tls`, a client also needs a trust
   anchor and server name. Options: announce `server_name` and a
   `ca_file` path (never key material), or keep `tls` announced with
   host/port only and require the client to supply trust settings itself.
   Recommendation: announce `server_name` and `ca_file` when the desktop
   was given a CA, since both are already local files the user controls.
3. **Default downstream bind address.** The desktop binds `0.0.0.0` by
   default, so its downstream port is reachable from the network even
   though discovery only ever advertises loopback. Defaulting to
   `127.0.0.1` would match the local-only use case. This changes existing
   CLI behavior, so it should be decided separately from this feature.
4. **Event-driven wake-up later.** If 500ms polling turns out too slow or
   too costly (e.g. on battery), the poll loop can be replaced by a
   directory watch behind the same `on_desktop_available()` API without
   changing the file format. Not needed for v1.

## Ordering

Step 1 gates Steps 2 and 3. Step 4 depends on Step 3. Step 5 depends on
Steps 3-4. Steps 6-11 depend on Step 5 and are independent of each other.
Step 12 lands last, once the API shapes are settled.

## Completion Criteria

- [ ] `test_desktop_registry`, `test_desktop_watcher`, and the extended
      `test_wish_desktop` pass under parallel `ctest -j`, using per-test
      registry directories and the short test poll interval.
- [ ] Manual end-to-end check with a real `wish desktop` and the Python
      example service: (a) desktop first, then service: a window appears;
      (b) service first, then desktop: a window appears within one poll
      interval; (c) close the desktop: the service keeps running; reopen
      it: the window appears again. Verified with the automation module
      (widget tree), not only "didn't crash".
- [ ] Every binding's test suite passes (`bindings/cpp/tests`,
      `bindings/python/tests`, `bindings/csharp/Wish.Tests`,
      `bindings/rust/tests`, `bindings/go/wish`); Android by code review
      and a manual smoke check.
- [ ] Two users on one Linux machine each discover only their own desktop.
- [ ] Full `ctest` run shows no regressions.
- [ ] `app/DESIGN.md`, `docs/cli.md`, `docs/bindings.md`, and
      `CHANGELOG.md` are updated; no stale content remains.
