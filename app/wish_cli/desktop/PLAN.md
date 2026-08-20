# PLAN: Local wish-desktop discovery/notification mechanism

See [`app/DESIGN.md`](../../DESIGN.md)'s "desktop mode" section for the
existing architecture of `wish_desktop`/`wish_desktop_app` this plan extends.

## Problem

`wish desktop` multiplexes N downstream client connections into one upstream
wish server session. Today a downstream client can only connect if it already
knows the desktop's transport/host/port — there is no way for a client
process to discover a locally-running desktop, or to be told when one becomes
available after the client has already started looking. No
discovery/broadcast/registry mechanism of any kind exists anywhere in wish or
its `extern/bison` dependency today (confirmed by exploration: no
UDP/multicast, no signaling socket, no shared memory, no filesystem
registry).

## Goal

A client process can register a callback — in C++, via the C ABI, and via
every language binding (cpp/python/csharp/android) — that fires with a
desktop's connection info (transport, host, port/pipe-name, pid) whenever a
wish desktop is available locally, regardless of whether the desktop was
already running before registration or starts afterward. The callback fires
once per discovered desktop, so multiple concurrently-running desktops are
each reported.

## Confirmed design decisions (not open for re-litigation without cause)

1. **Discovery mechanism = polling filesystem registry.** The desktop writes
   a small JSON announce file to a well-known temp-dir subdirectory when it
   starts listening, and removes it on clean shutdown; a client-side
   background thread polls that directory (~500ms) to catch both orderings.
   No platform-specific file-watch APIs (inotify/ReadDirectoryChangesW/
   kqueue) — consistent with this repo's stated preference to avoid
   `#ifdef`-heavy platform branching.
2. **Multiple desktops supported** — one file per desktop process (keyed by
   pid); callback fires once per desktop.
3. **Full binding scope** — core C++, C ABI, and wrapper APIs in all four
   existing bindings (cpp, python, csharp, android).

## Architecture

A new shared module in `src/client/` (already linked by both the announcer
side and every C ABI consumer, via the `wish_client` static target in the
root `CMakeLists.txt`):

- **`src/client/desktop_registry.hpp/.cpp`** — registry directory path, JSON
  schema, atomic write, permission hardening, stale-pid reaping. Used by
  *both* the desktop-side announcer and the client-side watcher, so the file
  format is defined exactly once.
- **`src/client/desktop_watcher.hpp/.cpp`** — the polling background thread +
  callback registry (`bdg::wish::on_desktop_available`), built on
  `desktop_registry::list_live_desktops()`.

The desktop-side hook lives in the existing `wish_desktop_app.hpp/.cpp` in
this directory.

---

## Step 1 — Shared registry core

**Goal:** A dependency-free module can atomically publish one announce file
per desktop process and list/reap the set of currently-live ones, with
owner-only permission hardening against local cross-user spoofing.

**Deliverables:**
- New `src/client/desktop_registry.hpp/.cpp`, namespace
  `bdg::wish::desktop_registry`:
  - `std::filesystem::path registry_dir()` — returns
    `std::filesystem::temp_directory_path() / "wish_desktops"` (mirrors the
    existing precedent at `src/context/context.cpp:83`,
    `temp_directory_path() / ("wish_" + ...)`). Creates the directory on
    first use and hardens it to owner-only
    (`std::filesystem::permissions(dir, perms::owner_all,
    perm_options::replace, ec)`) — call unconditionally, no `#ifdef` (inert
    but harmless on Windows, which already has a per-user `%TEMP%`).
  - `struct info { int64_t pid; std::string transport, host, name; int port; };`
    — `transport` is one of `"tcp" | "pipe" | "tls"` (reuses
    `--downstream_transport`'s vocabulary from `wish_desktop_app.cpp:31`);
    `host`/`port` populated for tcp/tls, `name` (pipe/unix-socket path) for
    pipe. Normalize a wildcard bind host (`0.0.0.0`/`::`) to loopback
    (`127.0.0.1`/`::1`) before writing, since discovery is local-machine-only
    by construction.
  - JSON schema written to `<pid>.json`, versioned:
    `{"schema": 1, "pid": ..., "transport": "...", "host": "...", "port": ..., "name": "..."}`.
  - `class announcer` (RAII, non-copyable/movable): constructor atomically
    publishes `<pid>.json` — write to `<pid>.json.tmp`, harden its
    permissions to owner-read/write only, then `std::filesystem::rename()`
    into place (mirrors the staging-file pattern in
    `src/context/file_service.cpp`'s `.part`-suffix + rename); destructor
    removes the file (best-effort, ignore errors).
  - `std::vector<info> list_live_desktops()` — scans `registry_dir()`, parses
    every `*.json`, drops and best-effort-deletes (reaps) any entry whose pid
    is no longer alive, returns the rest. On POSIX, additionally
    `::stat()`-checks each file's owner UID against `geteuid()` and
    skips/reaps mismatches (second layer of the spoofing mitigation).
  - A small `is_pid_alive(int64_t)` helper, narrow `#if defined(_WIN32)`
    guard within one function (mirrors the style at
    `src/context/logger.cpp:21-24` and `src/web/web_renderer.cpp:136,138`
    — no existing pid-liveness helper to reuse): `OpenProcess`/
    `GetExitCodeProcess` on Windows, `::kill(pid, 0)` (treating `EPERM` as
    "alive, owned by another user") on POSIX.
- Add the two new files to the `wish_client` target in the root
  `CMakeLists.txt` (`add_library(wish_client STATIC ...)`).

**Tests — new `tests/test_desktop_registry.cpp`:**
- `announcer` writes a well-formed, owner-only-permission file at the
  expected path; removes it on destruction.
- `list_live_desktops()` returns an entry published by the current process
  (its own pid is always "alive").
- A hand-written `<huge-nonexistent-pid>.json` is reaped (file disappears)
  and excluded from results.
- Note in a comment: the cross-UID spoofing check has no automated test
  (can't be exercised without a second real UID in a single-user test
  environment) — documented gap, not silently skipped.

---

## Step 2 — Desktop-side announcer wiring

**Goal:** `wish_desktop` publishes an announce file exactly once, right after
listening starts, for every downstream transport except `term`, and removes
it automatically on shutdown.

**Deliverables:**
- `wish_desktop_app.hpp`, class `wish_desktop` (lines 36-86): add
  ```cpp
  void publish_announcement(const std::string& transport, const std::string& host,
                             int port, const std::string& name);
  ```
  and a private member (alongside `chrome_built_` etc. at lines 79-85):
  `std::optional<bdg::wish::desktop_registry::announcer> announcement_;`.
- `wish_desktop_app.cpp`:
  - Implement `publish_announcement()` alongside `build_chrome()`: return
    immediately if `transport == "term"` (a spawned terminal has no
    listening socket a second process could dial); otherwise build a
    `desktop_registry::info` from the arguments (current pid via a small
    helper in `desktop_registry.cpp`) and `announcement_.emplace(...)` it.
  - In `wish_desktop_app::on_listening()` (lines 207-211), add, right after
    `desktop_->build_chrome();`:
    ```cpp
    if (desktop_)
      desktop_->publish_announcement(FLAGS_downstream_transport, FLAGS_downstream_host,
                                      FLAGS_downstream_port, FLAGS_downstream_name);
    ```
    (`FLAGS_downstream_*` are defined in this same file at lines 31-34;
    `on_listening()` is `const` but `desktop_` is the existing non-owning raw
    pointer at `wish_desktop_app.hpp:167`, called the same way
    `build_chrome()` already is.)
  - Add a one-line comment on `wish_desktop::~wish_desktop() = default;`
    (line 115) noting `announcement_`'s destructor removes the file
    automatically.

**Tests — extend `tests/test_wish_desktop.cpp`** (reusing the existing
`WishDesktopTest` fixture, lines 47-64):
- `PublishAnnouncementWritesRegistryFile` — call `publish_announcement("tcp",
  "127.0.0.1", 7071, "")` directly; assert `list_live_desktops()` contains a
  matching entry for the current process's pid.
- `PublishAnnouncementSkippedForTermTransport` — call with `"term"`; assert
  no file appears.
- `DestructorRemovesAnnouncementFile` — publish, destroy the `wish_desktop`,
  assert the file is gone.

---

## Step 3 — Client-side watcher core

**Goal:** `bdg::wish::on_desktop_available(callback)` fires, from a
background polling thread, once per desktop already live at registration
time and once per desktop that becomes live afterward — correct regardless
of registration order relative to any other callback.

**Deliverables:**
- New `src/client/desktop_watcher.hpp/.cpp`:
  ```cpp
  namespace bdg::wish {
  void on_desktop_available(std::function<void(const desktop_registry::info&)> callback);
  }
  ```
  No unregister API (deliberate — mirrors `rmi_proxy_on_event`'s and
  `wish_list_apps`/`app_registry`'s process-lifetime, no-removal shape; see
  `bindings/android/jni/wish_rmi_jni.cpp`'s existing rationale for the same
  tradeoff on the closest analogous callback).
  - State: `bison::synchronized<watcher_state>` (per this repo's concurrency
    conventions), where `watcher_state` holds `callbacks`, a global
    `notified_pids` set (poll-loop dedup only), and `stop`/`started` flags.
    `bison::synchronized<T>` (`extern/bison/src/bison/bison_sync.hpp`) owns
    its own condition variable — `wait`/`wait_for`/`notify_one`/`notify_all`
    are member functions — so no separate `std::mutex`/`std::condition_variable`
    is needed.
  - `on_desktop_available()`: **first** synchronously calls `callback` for
    every desktop returned by `list_live_desktops()` right now (this is what
    makes a *late* registration still correctly see an *earlier* desktop,
    independent of the poll loop's own global dedup state); **then** adds the
    callback to `watcher_state.callbacks` and, on the very first registration
    only, starts the background poll thread and registers `stop_watcher` via
    `std::atexit()`.
  - `poll_loop()`: every `poll_interval()` (default 500ms, overridable via a
    `bdg::wish::detail::set_desktop_watcher_poll_interval_for_testing(...)`
    hook), calls `list_live_desktops()`; for each entry not yet in the global
    `notified_pids` set, snapshots the callback list under lock and invokes
    them all outside the lock.
  - `stop_watcher()`: sets the stop flag, notifies, joins the worker thread.
    Registered via `std::atexit()` (not a static destructor) so it is
    guaranteed to run — and join the thread — before `state()`'s
    function-local static is destroyed (per `[basic.start.term]`'s ordering
    guarantee relative to when the `atexit` handler was registered).
- Add the two new files to the `wish_client` CMake target alongside Step 1's.

**Tests — new `tests/test_desktop_watcher.cpp`:** set a short (few-ms) poll
interval in `SetUp()` via the testing hook; use a bounded-timeout
(e.g. 2s) wait helper so failures fail fast rather than hang:
- `FiresForAlreadyPublishedDesktop` — publish via `desktop_registry::announcer`
  before registering; assert the callback fires (synchronous catch-up path).
- `FiresForDesktopPublishedLater` — register first, publish after; assert it
  fires within a couple of poll intervals.
- `IgnoresStaleDeadPidEntries` — hand-write a fake dead-pid `.json`; assert no
  callback fires and the file is reaped.
- `FiresOncePerDesktopForMultipleDesktops` — publish two `announcer`s; assert
  exactly two distinct-pid firings.
- `LateRegistrationStillFiresForEarlierDesktop` — publish, register callback
  A, then register callback B; assert **both** fire.

---

## Step 4 — C ABI

**Goal:** `wish_on_desktop_available()` is callable with no
`wish_client_handle`/connection, and delivers each discovered desktop's info
as a scope-local `bison_handle`.

**Deliverables:**
- `include/wish_client_c.h` (new section near `wish_list_apps`, line 431):
  ```c
  typedef void (*wish_desktop_available_fn)(bison_handle info, void* userdata);
  WISH_API wish_error wish_on_desktop_available(wish_desktop_available_fn callback, void* userdata);
  ```
  Doc comment specifies: called from a background thread; `info` is
  scope-local and valid only for the call's duration (mirrors
  `rmi_proxy_event_fn`'s contract) — read with `bison_get_*()`, never
  `bison_release()`d by the caller; fields `pid`(int)/`transport`(string)/
  `host`(string)/`port`(int)/`name`(string); process-lifetime registration,
  no unregister call.
- `src/wish_client_c.cpp`: implement by wrapping
  `wish::on_desktop_available(...)` with a lambda that builds a `dynamic`
  from the `desktop_registry::info` fields and delivers it using the exact
  scope-local `bison_dynamic_ptr`/`as_bison_handle` idiom already used by
  `rmi_proxy_on_event` (`extern/bison/src/rmi/rmi_c.cpp:627-638`) — no heap
  allocation crossing the ABI, no caller-side free needed.

**Tests:** none dedicated at this layer (matches `wish_list_apps`'s own
precedent of no direct C-ABI gtest); covered by Step 3's core tests plus
Step 5+'s binding tests exercising the real ABI end to end.

---

## Step 5 — `bindings/cpp` (header-only C++ wrapper)

**Goal:** A free function `bdg::wish::binding::on_desktop_available(callback)`
usable with no `client` instance, mirroring `list_apps_json()`'s shape.

**Deliverables:**
- New `bindings/cpp/include/wish_cpp/desktop.hpp`: `struct desktop_info`
  (pid/transport/host/port/name) and `inline void
  on_desktop_available(std::function<void(const desktop_info&)>)`, wrapping
  `wish_on_desktop_available()` with a heap-allocated (intentionally leaked —
  process-lifetime, matches the ABI's own no-unregister contract)
  `std::function` as the trampoline's `userdata`. Verify
  `value::get_int`/`get_string` signatures in
  `bindings/cpp/include/wish_cpp/value.hpp` before implementing. Include from
  the `wish_cpp/wish.hpp` umbrella header.

**Tests:** add to `bindings/cpp/tests/test_client.cpp` — since this target
links `wish_client_dll` (not the internal `wish_client` static lib) and can't
include `src/client/desktop_registry.hpp` directly, write a fake registry
`.json` file at the well-known path by recomputing `temp_directory_path() /
"wish_desktops"` locally in the test (small, explicitly-commented
duplication), register a callback, assert it fires.

---

## Step 6 — Python binding

**Goal:** `wish.on_desktop_available(callback)` works with no `Client`
instance, keeping the ctypes trampoline alive for process lifetime.

**Deliverables:**
- `bindings/python/wish/_native.py`: add `DesktopAvailableFn =
  ctypes.CFUNCTYPE(None, _bison_native.Handle, ctypes.c_void_p)` near
  `SessionFn` (line 68), plus `argtypes`/`restype` registration for
  `wish_on_desktop_available`.
- New `bindings/python/wish/desktop.py`: module-level `_trampolines = []`
  list (keeps callback objects alive — mirrors bison's `Proxy.on_event()`'s
  `self._callbacks`, adapted to module scope since there's no owning
  instance), and `on_desktop_available(callback)` wrapping a `c_callback`
  that builds a `dict` (`pid`/`transport`/`host`/`port`/`name`) from a
  `bison.rmi.Dynamic(_handle=..., _owned=False)` view of the payload. Verify
  `Dynamic`'s constructor kwargs and `get_int`/`get_string` method names
  against `extern/bison/bindings/python/bison/rmi.py` before implementing.
  Export from `bindings/python/wish/__init__.py`.

**Tests** — new `bindings/python/tests/test_desktop.py`: write a fake
registry file directly (`tempfile.gettempdir()` + `"wish_desktops"`,
matching the C++ path logic), register a callback, assert it fires within a
bounded `threading.Event().wait(timeout=2)`. No poll-interval injection
available from this layer, so a real ~1s+ bounded wait is acceptable here
(unlike Step 3's millisecond-scale core tests).

---

## Step 7 — C# binding

**Goal:** `Bdg.Wish.Desktop.OnDesktopAvailable(callback)`, static (no
`Client` instance), keeping delegates alive for process lifetime.

**Deliverables:**
- `bindings/csharp/Wish/Native.cs`: add a `NativeDesktopAvailableFn` delegate
  type and the `wish_on_desktop_available` P/Invoke declaration, matching
  whichever calling-convention pattern `Client.cs`'s existing `Run()`/
  `NativeSessionFn` wiring uses (verify before implementing).
- New `bindings/csharp/Wish/Desktop.cs`: `record DesktopInfo(long Pid, string
  Transport, string Host, int Port, string Name)` and `static class Desktop`
  with a static `List<object> _callbacks` (adapts `Proxy.OnEvent`'s
  per-instance list to static scope) and `OnDesktopAvailable(Action<DesktopInfo>)`.
  Verify the exact "wrap a borrowed `bison_handle` as Dynamic" helper name
  (likely in bison's own `Bdg.Bison` C# binding) before implementing.

**Tests** — new `bindings/csharp/Wish.Tests/DesktopTests.cs`: same shape as
Step 6 — fake registry file + bounded-timeout `TaskCompletionSource`.

---

## Step 8 — Android/JNI + Kotlin/Java binding

**Goal:** `Client.onDesktopAvailable(DesktopAvailableCallback)` static
method, JNI trampoline mirroring the existing `event_ctx`/`event_trampoline`
pattern used for `rmi_proxy_on_event`.

**Deliverables:**
- New `DesktopAvailableCallback.java` (one-method interface) and
  `DesktopInfo.java` (data class: pid/transport/host/port/name), mirroring
  `AppInfo.java`'s style.
- `Client.java`: static `onDesktopAvailable(DesktopAvailableCallback)` calling
  a new native method, mirroring `listApps()`'s static-native shape.
- `bindings/android/jni/wish_rmi_jni.cpp`: `desktop_ctx`/`desktop_trampoline`
  pair mirroring `event_ctx`/`event_trampoline` (attach/detach current thread
  around each invocation, `NewGlobalRef` on registration since it's never
  released — matches the ABI's no-unregister contract — `DeleteLocalRef` on
  the constructed `DesktopInfo` object after the Java call returns, catch and
  log any Java exception rather than letting it cross back into C++).

**Tests:** Android instrumented tests need an emulator and aren't part of
this repo's normal test loop (verify: `rmi_proxy_on_event`'s own JNI wrapper
has no dedicated instrumented test either). Documented gap; correctness
coverage comes from Steps 3-4's C++/ABI tests plus a manual smoke check.

---

## Step 9 — Documentation

**Goal:** `app/DESIGN.md`, `docs/cli.md`, and `docs/bindings.md` accurately
describe the new discovery mechanism; no stale content remains.

**Deliverables:**
- `app/DESIGN.md` — extend the existing "desktop mode" section's lifecycle
  diagram (currently ending `on_listening() → desktop_->build_chrome() (menu
  bar + dockspace + clock)`) with a `publish_announcement()` line, plus a
  short "Desktop discovery" paragraph pointing at
  `src/client/desktop_registry.hpp` for the schema and explaining why `term`
  is skipped.
- `docs/cli.md` — in the `## wish desktop` section, right after the existing
  paragraph on `WISH_TRANSPORT`/etc. env vars (~lines 198-206), add a
  paragraph noting the discovery announcement published for tcp/pipe/tls
  downstream transports.
- `docs/bindings.md` — add a short "Desktop discovery" snippet to each
  per-language section (cpp/python/csharp; add an Android subsection if none
  exists yet).
- `README.md` — no change needed (no new `docs/` file is being added).
- Do **not** create `src/client/DESIGN.md` speculatively.

---

## Ordering

Step 1 gates Steps 2 and 3 (shared registry format). Steps 4-8 each depend on
Step 3 (the core `on_desktop_available`) and Step 2 (so binding tests have a
real desktop to discover), but are otherwise independent and can proceed in
any order. Step 9 (docs) lands last, once the real API shapes from Steps 4-8
are settled.

## Completion Criteria

- [ ] `test_desktop_registry`, `test_desktop_watcher`, and the extended
      `test_wish_desktop` all pass under `ctest`, with no added flakiness
      (watcher tests use the injectable short poll interval, not the
      production 500ms default).
- [ ] Manual end-to-end check, both orderings: (a) start `wish desktop`,
      then register a callback from a separate client process — it fires
      with the correct port; (b) register the callback first, then start
      `wish desktop` — it still fires, within one poll interval.
- [ ] Every implemented binding's own test suite passes
      (`bindings/cpp/tests`, `bindings/python/tests`,
      `bindings/csharp/Wish.Tests`); Android is verified by code review plus
      a manual smoke check given the emulator-test gap noted in Step 8.
- [ ] Full `ctest` run shows no regressions in `test_wish_desktop` or
      elsewhere.
- [ ] `app/DESIGN.md`, `docs/cli.md`, and `docs/bindings.md` are updated and
      accurate; no stale content describing the pre-discovery behavior
      remains.
