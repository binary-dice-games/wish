# PLAN: Well-known desktop endpoint (discovery + single instance)

See [`app/DESIGN.md`](../../DESIGN.md)'s "desktop mode" section for the
existing architecture of `wish_desktop`/`wish_desktop_app` this plan extends.

**Status:** Step 1 done (bison `85326cd`, on bison's
`claude/desktop-app-notification-3wn7d7` branch). Steps 2-6 not started.

## Problem

A process started independently of `wish desktop` (a service, a daemon, a
script started at login) has no way to find the desktop, or to learn that
it started after the process did. Today only apps the desktop launches
itself know where it is, through the `WISH_TRANSPORT`/`WISH_HOST`/
`WISH_PORT`/`WISH_NAME` variables `wish_desktop_app::make_bridge()` passes
via `set_launch_env()`. Nothing stops two desktops from being started
either.

## Goal

- There is at most **one** wish desktop on the machine. Starting a second
  one fails immediately with a clear message.
- The desktop always listens on one **fixed, system-wide** local endpoint.
  Any local process can open a UI on it by connecting there; no address
  needs to be configured or looked up.
- A long-running process can keep a UI on the desktop across desktop
  restarts: it retries the connection while no desktop is running, runs its
  UI session when one is, and goes back to retrying when the desktop closes.

## Design decisions

1. **The endpoint is the lock.** Only one process can listen on a local
   pipe/socket name, and the OS releases it when that process dies, even on
   a crash. Binding the fixed endpoint therefore enforces the single
   instance; no lock file, pid file or registry is needed.
2. **System-wide, not per user.** One endpoint name for the whole machine;
   no user is checked on either side.
3. **Endpoint name:**
   - Native Windows and MSYS2 (Windows libuv): named pipe
     `\\.\pipe\wish-desktop`. libuv's `uv_pipe_bind()` creates the first
     instance with `FILE_FLAG_FIRST_PIPE_INSTANCE` and returns
     `UV_EADDRINUSE` when another process already owns the name
     (`src/win/pipe.c`), which is exactly the single-instance check.
     Pipes vanish with their process.
   - Linux: abstract-namespace Unix socket `@wish-desktop`. Abstract
     sockets have no file system entry, so there is no stale socket file
     after a crash; `bind()` fails with `EADDRINUSE` while another desktop
     holds it, and the kernel releases it when the process exits. This
     needs a small bison change (Step 1).
   - The `WISH_DESKTOP_ENDPOINT` environment variable overrides the name
     (tests use a unique name per test).
4. **No push notification.** Nothing can notify a process before the
   desktop exists, so a waiting process retries the connect, by default
   once a second. A failed connect to a local pipe costs microseconds.
5. **The fixed endpoint is an extra downstream listener.** The desktop
   keeps its configured downstream transport (`--downstream_transport`,
   default tcp:7071) and additionally accepts clients on the fixed
   endpoint, so existing setups and launched apps keep working.
6. **No Android binding.** The desktop does not run on Android, so there
   is nothing local to discover there.

---

## Step 1: Abstract socket names in bison's pipe transport (bison) -- DONE

**Goal:** On Linux, `named_pipe_server_transport`/`named_pipe_client_transport`
accept a name starting with `@` as an abstract-namespace socket.

Approved and implemented. Besides the deliverables below, the same commit
fixes three bugs in `named_pipe_transport.cpp` found while testing: `open()`
spinning forever when `uv_pipe_connect2()` fails synchronously, a failed
`start()` leaking its loop, and `on_new_connection()` closing a
stack-allocated `uv_pipe_t` asynchronously (use after scope; valgrind).

**Deliverables (in bison):**
- `src/rmi/transport/named_pipe_transport.cpp`: when `path_` starts with
  `@`, replace the `@` with `\0` and call `uv_pipe_bind2()` /
  `uv_pipe_connect2()` with the explicit length (libuv >= 1.46; the bundled
  libuv is 1.52) instead of `uv_pipe_bind()` / `uv_pipe_connect()`. Other
  names keep the current code path. On Windows `@` names are left as
  they are (named pipes have no abstract namespace).
- `named_pipe_server_transport::start()` already throws on a bind error;
  make sure the message keeps libuv's `EADDRINUSE` text so callers can
  recognize "already in use", or throw a dedicated
  `address_in_use_error : std::runtime_error`.
- Doc comment in `named_pipe_transport.hpp` describing `@` names.
- `CHANGELOG.md` entry in bison; submodule update in wish (see
  `CLAUDE.md`, "Updating the submodule after a bison commit").

**Tests (bison):**
- Linux: a client connects to a server listening on a unique `@name`.
- A second server on the same `@name` fails to start with the
  address-in-use error, and succeeds once the first one has stopped.
- Windows: a second server on the same pipe name fails the same way.

---

## Step 2: Endpoint name helper

**Goal:** Desktop and clients agree on the endpoint name in exactly one
place.

**Deliverables:**
- New `src/client/desktop_endpoint.hpp/.cpp` in the `wish_client` target
  (linked by both `wish-desktop` and `wish_client_dll`):
  ```cpp
  namespace bdg::wish {
  /// Name of the desktop's fixed local endpoint: `WISH_DESKTOP_ENDPOINT` if
  /// set, else `\\.\pipe\wish-desktop` on Windows, `@wish-desktop` on Linux.
  std::string desktop_endpoint();
  }
  ```
  The platform difference is one narrow `#if defined(_WIN32)` in this
  function.

**Tests:** new `tests/test_desktop_endpoint.cpp`: the default name, and the
environment override.

---

## Step 3: Desktop listens on the endpoint and enforces one instance

**Goal:** `wish desktop` refuses to start when a desktop is already
running, and otherwise accepts clients on the fixed endpoint as well as on
its configured downstream transport.

**Deliverables:**
- New `app/wish_cli/desktop/multi_server_transport.hpp/.cpp`: a
  `bison::rmi::transport::server_transport_iface` that combines several
  server transports (some borrowed, some owned). `start()` starts each,
  `accept(timeout)` returns the first connection from any of them, `stop()`
  stops all. Small, local to the desktop; bison's `bridge` stays unchanged
  because it only sees one transport.
- `wish_desktop_app::run()`: right after parsing flags, before
  `bridge_app::run()` connects upstream or spawns any terminal, create a
  `named_pipe_server_transport(desktop_endpoint())` and `start()` it. On
  the address-in-use error print
  `wish desktop is already running` to stderr and return 1. Keep the
  started transport in a new member, `endpoint_transport_`.
- `wish_desktop_app::make_bridge()`: build a `multi_server_transport` from
  the `downstream_transport` it is given (borrowed) and
  `endpoint_transport_` (already started; `named_pipe_server_transport::
  start()` returns early when started), keep it in a member, and construct
  `wish_desktop` with it.
- The endpoint is released when `wish_desktop_app` is destroyed, or by the
  OS if the process dies.

**Tests:**
- `tests/test_multi_server_transport.cpp`: clients on either of two memory
  transports are accepted; `stop()` stops both.
- `tests/test_wish_desktop.cpp` (or a new `tests/test_desktop_single_instance.cpp`),
  with a unique `WISH_DESKTOP_ENDPOINT` per test:
  - With the endpoint already held by a listener, the startup check
    reports "already running".
  - A wish client connecting to the endpoint gets a desktop session (the
    desktop's `__WishDesktop` object can be instantiated).

---

## Step 4: Client side: run a UI whenever the desktop is up

**Goal:** A process gets a UI on the desktop, and gets it back after the
desktop restarts, with one call.

**Deliverables:**
- C++ (`src/client/desktop_endpoint.hpp/.cpp`): a client for the endpoint
  is just `named_pipe_client_transport(desktop_endpoint())`. Add a loop:
  ```cpp
  /// Repeats until `stop` is set: connect to the desktop (retrying every
  /// `retry_interval` while none is running), call `session`, then go back
  /// to waiting once `session` returns or the desktop closes.
  void run_on_desktop(std::function<void(client&)> session,
                      std::stop_token stop,
                      bison::dynamic connect_params = {},
                      std::chrono::milliseconds retry_interval = std::chrono::seconds{1});
  ```
  `connect_params` lets the process set the `app`/`app_label`/`app_icon`
  fields the desktop shows on its taskbar.
- C ABI (`include/wish_client_c.h`, `src/wish_client_c.cpp`):
  ```c
  /// Client handle for the desktop's fixed endpoint.
  WISH_API wish_client_handle wish_client_desktop_create(void);
  /// Loops like wish_client_run_with_params(), reconnecting whenever the
  /// desktop comes back, until wish_client_desktop_stop() is called.
  WISH_API wish_error wish_client_run_on_desktop(
      wish_client_handle client, wish_session_fn session_fn, void* userdata,
      bison_handle connect_params, uint32_t retry_ms);
  /// Ends wish_client_run_on_desktop(): quits the running session, if
  /// any, and stops retrying. Safe from any thread.
  WISH_API void wish_client_desktop_stop(wish_client_handle client);
  ```
  Inside `session_fn` the caller uses `wish_client_wait()` as today. It
  already returns when the desktop closes (`c_abi_client::on_disconnect()`
  sets `quit_`), and `wish_client_run()` resets `quit_` on each run, so the
  same handle is reused across reconnects.

**Tests:** in `tests/` with a unique `WISH_DESKTOP_ENDPOINT` and a real
`wish::server` listening on that endpoint as a stand-in for the desktop:
- Client started first, server second: the session runs.
- Server stopped: the session ends and the loop keeps running; server
  started again: the session runs a second time.
- `wish_client_desktop_stop()` while waiting and while in a session: the
  call returns promptly.

---

## Step 5: Bindings

**Goal:** Each desktop-capable binding exposes the endpoint client and the
reconnecting loop in its own style.

All bindings already wrap `wish_client_pipe_create`; they add only
`wish_client_desktop_create`, `wish_client_run_on_desktop` and
`wish_client_desktop_stop`. Each test sets `WISH_DESKTOP_ENDPOINT` to a
unique name and uses a bounded wait.

| Binding | API |
|---|---|
| cpp (`bindings/cpp/`) | `client::desktop()`, `client::run_on_desktop(session, params)`, `client::stop_desktop()` |
| python (`bindings/python/`) | `Client.desktop()`, `client.run_on_desktop(session, params=None)`, `client.stop_desktop()` |
| csharp (`bindings/csharp/`) | `Client.Desktop()`, `RunOnDesktop(...)`, `StopDesktop()` |
| rust (`bindings/rust/`) | `Client::desktop()`, `run_on_desktop(...)`, `QuitHandle`-style stop |
| go (`bindings/go/`) | `NewDesktopClient()`, `RunOnDesktop(...)`, `StopDesktop()` |

Android is out of scope (see design decision 6).

---

## Step 6: Documentation and changelog

**Deliverables:**
- `app/DESIGN.md`, "desktop mode": the endpoint, the single-instance check
  in `run()`, `multi_server_transport`, and why the endpoint doubles as the
  lock.
- `docs/cli.md`, `## wish desktop`: only one desktop per machine; the fixed
  endpoint name per platform; `WISH_DESKTOP_ENDPOINT`.
- `docs/bindings.md`: a short "UI on the desktop, across restarts" snippet
  per binding.
- A runnable example, `bindings/python/examples/desktop/service.py`: a
  background process that shows a status window whenever the desktop is
  running. Mention it in that folder's README and in `docs/examples.md`.
- `CHANGELOG.md`, `## [Unreleased]`: `### Added` for the endpoint and the
  client API, `### Changed` for "only one desktop can run at a time".

---

## Ordering

Step 1 is done. Steps 3 and 4 depend on Step 2 and
are independent of each other. Step 5 depends on Step 4. Step 6 lands last.

## Completion Criteria

- [ ] Starting a second `wish desktop` prints "already running" and exits
      with 1, on Linux and Windows.
- [ ] After a desktop is killed (`kill -9` / Task Manager), a new desktop
      starts without any cleanup.
- [ ] Manual end-to-end check with a real `wish desktop` and the example
      service: (a) desktop first, then service: a window appears;
      (b) service first, then desktop: a window appears within about a
      second; (c) close the desktop: the service keeps running; reopen it:
      the window appears again. Verified with the automation module
      (widget tree), not only "didn't crash".
- [ ] New tests pass under parallel `ctest -j`; every touched binding's
      test suite passes; no regressions.
- [ ] `app/DESIGN.md`, `docs/cli.md`, `docs/bindings.md` and `CHANGELOG.md`
      are updated.
