# dbg

A source-level debugger front end: attach by PID, set breakpoints by
right-clicking a source line, step, watch simple local variables, and see
debug/exception output — all in dockable windows, in the same "GUI frontend
over a real client-side backend" shape as `docker` and `git`. All debugger
interaction happens client-side; the server only renders whatever snapshot
it's given.

Backends behind one `debug_backend` interface, selected at launch with
`-- --backend <name>` (the repo-wide convention for module arguments):

- **`native`** (default) — debugs a native/compiled process:
  - **Windows** — the Win32 debug API + DbgHelp directly (`win32_debug_backend`).
  - **Linux / macOS** — drives a child `gdb --interpreter=mi` (or `lldb-mi`)
    over the GDB/MI protocol (`posix_debug_backend`). Needs `gdb` (or
    `lldb-mi`) on the client's `PATH`; override with `WISH_DBG_DEBUGGER`
    (absolute path or a bare name). On macOS `gdb` requires code-signing,
    so `lldb-mi` is usually the practical choice there.
- **`python`** — debugs a **Python** process by driving Microsoft's
  `debugpy` over the Debug Adapter Protocol, the same adapter/protocol VS
  Code's Python debugger uses (`python_debug_backend`, cross-platform).
  Needs `debugpy` installed (`pip install debugpy`); `WISH_DBG_PYTHON`
  overrides the interpreter. Two ways to attach:
  - **by PID** (default): `wish client --run=dbg -- --backend python`, then
    enter the target's PID in the Source toolbar. `python -m debugpy.adapter`
    injects `debugpy` into the process — which, like `ptrace` for the GDB
    backend, needs OS support (a `gdb`/`lldb` for `debugpy`'s injector, or a
    CPython new enough for `sys.remote_exec`); on a locked-down host the
    attach fails with a logged message in the Output window.
  - **connect**: `wish client --run=dbg -- --backend python --connect
    HOST:PORT` attaches to a `debugpy` server the target started itself with
    `debugpy.listen((HOST, PORT))` — no injection, works everywhere.
    `WISH_DBG_DAP_CONNECT=HOST:PORT` is equivalent. (The PID field is
    ignored in this mode.)

`wish client --run=dbg` opens six independently dockable windows: Source,
Threads, Call Stack, Watch, Breakpoints, Output. With no `--connect`, enter
a PID in the Source window's toolbar and click Attach.

**Deployment requirement:** the server hosting `dbg` must be started with
`--allow_absolute_paths` (`wish server --allow_absolute_paths` or `wish
standalone --run=dbg --allow_absolute_paths`). Source files are opened by
their absolute on-disk path as reported by the debug backend/PDB, and
`TextEditor` refuses to render an absolute path unless the server opts in —
without this flag, the Source window's tabs open with the right label but
stay empty. Only enable it for a trusted, single-operator deployment: it
lets any connected client make the server read arbitrary local files by
absolute path (see `wish::server::set_allow_absolute_paths()`'s doc
comment). `wish standalone` runs server and client in one process, so this
is always safe there.

- **server/**: `DebuggerFrontend` form (`register_dbg()`) — renders
  whatever snapshot it was last given via `update_threads` /
  `update_callstack` / `update_watch` / `update_breakpoints` /
  `update_source` / `append_output`, and emits `*_requested` events (see
  `server/dbg.hpp`'s class doc comment for the full contract) for the
  client to react to by calling into the debug backend.
- **client/**: `run_dbg(wish_app_host&)`, self-registered as the `"dbg"`
  embedded app. `client/debug_backend.hpp` is the platform-agnostic seam
  (`attach`/`detach`, `pause`/`resume`, `step_into`/`step_over`/`step_out`,
  `set_breakpoint`/`clear_breakpoint`, `get_threads`/`get_callstack`/
  `evaluate`, `on_stop`/`on_log`). `client/win32_debug_backend.hpp/.cpp`
  implements it on Windows (`DebugActiveProcess` / `WaitForDebugEvent` /
  `ContinueDebugEvent` on a dedicated debug thread, `DbgHelp` for
  symbol/line/type resolution, `INT3` software breakpoints);
  `client/posix_debug_backend.hpp/.cpp` implements it on Linux/macOS by
  spawning a child `gdb`/`lldb-mi` and driving it over GDB/MI, parsed by
  `client/mi_parser.hpp/.cpp`. `client/python_debug_backend.hpp/.cpp`
  implements it on all platforms for Python debuggees by driving `debugpy`
  over the Debug Adapter Protocol (JSON over a socket, framed/parsed by
  `client/dap_protocol.hpp/.cpp`), using libuv (`uv_spawn` / `uv_tcp_t`)
  for the child process and connection. `client/dbg.cpp`'s `run_dbg` picks
  the backend from the `-- --backend` argument. `client/dbg_source.hpp/.cpp`
  owns the RMI proxy, reacts to the form's `*_requested` events by calling
  the backend, and reacts to `debug_backend::on_stop`/`on_log` by pushing
  fresh `update_*` snapshots.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_DBG=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).
Builds on Windows, Linux, and macOS. Run-time dependencies (client machine):
the `native` backend needs `gdb`/`lldb-mi` on Linux/macOS; the `python`
backend needs `debugpy` (`pip install debugpy`).

See [DESIGN.md](DESIGN.md) for the full architecture and [PLAN.md](PLAN.md)
for what's implemented vs. deferred.

## Implementation status

All six windows and stepping/watch/output are implemented and verified
against a real attached process:

- **Source** — toolbar (PID field, Attach/Detach/Pause/Continue/Step
  Into/Step Over/Step Out); open files as read-only `TextEditor` tabs with
  a breakpoint-dot gutter and a current-execution-line highlight;
  right-click a line number to toggle a breakpoint.
- **Threads** — Id / State / Current Function; click a row to select it and
  refresh its Call Stack.
- **Call Stack** — # / Function / File / Line for the selected thread;
  click a frame to select it (drives the Watch window) and re-focuses that
  frame's file/line in the Source window.
- **Watch** — type an expression and click Add; each entry resolves (name /
  value / type) against the currently-selected frame — via `DbgHelp` symbol
  enumeration on Windows, `gdb`'s `-data-evaluate-expression` / `whatis` on
  Linux/macOS, or DAP `evaluate` (context `watch`) for the `python` backend.
- **Breakpoints** — File / Line / Enabled, with a per-row `...` menu to
  remove.
- **Output** — a FIFO-capped trace of attach/stop/exception events and
  debuggee output (Windows `OutputDebugString` calls; Linux/macOS output
  the debugger surfaces over MI), colour-coded by severity
  (info/warn/error).

## Known limitations (v1)

- **No remote (cross-machine) attach** — the debuggee must be on the same
  machine as `wish client`.
- **The debug engine must be installed on the client.** The `native`
  backend drives a child `gdb`/`lldb-mi` on Linux/macOS; the `python`
  backend drives `debugpy`. There is no dependency-free engine. See
  DESIGN.md §6.
- **`python` backend, PID attach needs an injector.** `debugpy` injects
  itself into a running process via `gdb`/`lldb` (or `sys.remote_exec` on a
  new enough CPython); where neither is available the attach fails — use
  `--connect host:port` against a `debugpy.listen()` the target opened
  itself instead.
- **No conditional breakpoints or logpoints.**
- **Watch resolves simple scalar local-variable reads only** — not
  arbitrary C++ expressions (no member access, indexing, arithmetic, or
  calls). On Windows a register-resident (optimized-out) local reports as
  `<unavailable>` and a non-scalar type is shown as a raw hex byte dump; on
  Linux/macOS the value/type are whatever `gdb`'s
  `-data-evaluate-expression` / `whatis` return. See DESIGN.md §1's "Watch
  scope (v1)" note.
- **No stdout/stderr capture.** `dbg` attaches to an already-running
  process, so there is no process-launch step at which to redirect its
  standard handles — on Windows only `OutputDebugString` calls reach the
  Output window; on Linux/macOS only output the debugger surfaces over MI
  (`@` target-stream records) does. See DESIGN.md §1's "Debuggee output
  (v1)" note.
- **No edit-and-continue, memory/disassembly view, or multi-process
  debugging** (no auto-attach to child processes).

See [PLAN.md](PLAN.md) for the complete list of deferred features.
