# dbg

A source-level debugger front end for native Windows processes: attach by
PID, set breakpoints by right-clicking a source line, step, watch simple
local variables, and see debug/exception output — all in dockable windows,
in the same "GUI frontend over a real client-side backend" shape as `docker`
and `git`. All Win32 debug API / DbgHelp interaction happens client-side;
the server only renders whatever snapshot it's given.

`wish client --run=dbg` (no positional args — enter a PID in the Source
window's toolbar and click Attach). Opens as six independently dockable
windows: Source, Threads, Call Stack, Watch, Breakpoints, Output.

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
  `evaluate`, `on_stop`/`on_log`); `client/win32_debug_backend.hpp/.cpp` is
  the only implementation (Windows-only — `DebugActiveProcess` /
  `WaitForDebugEvent` / `ContinueDebugEvent` on a dedicated debug thread,
  `DbgHelp` for symbol/line/type resolution, `INT3` software breakpoints).
  `client/dbg_source.hpp/.cpp` owns the RMI proxy, reacts to the form's
  `*_requested` events by calling the backend, and reacts to
  `debug_backend::on_stop`/`on_log` by pushing fresh `update_*` snapshots.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_DBG=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).
Windows-only in practice: the module configures on Linux/macOS too, but
`win32_debug_backend` compiles to nothing there and no other backend exists
yet (see "Known limitations" below).

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
- **Watch** — type an expression and click Add; each entry resolves as a
  simple local-variable read (name / value / type) against the
  currently-selected frame via `DbgHelp` symbol enumeration.
- **Breakpoints** — File / Line / Enabled, with a per-row `...` menu to
  remove.
- **Output** — a FIFO-capped trace of attach/stop/exception events and
  debuggee `OutputDebugString` calls, colour-coded by severity
  (info/warn/error).

## Known limitations (v1)

- **Windows only.** No Linux/macOS backend, no remote (cross-machine)
  attach.
- **No conditional breakpoints or logpoints.**
- **Watch resolves simple scalar local-variable reads only** — not
  arbitrary C++ expressions (no member access, indexing, arithmetic, or
  calls); a register-resident (optimized-out) local reports as
  `<unavailable>`; a non-scalar type is shown as a raw hex byte dump. See
  DESIGN.md §1's "Watch scope (v1)" note.
- **No stdout/stderr capture.** `dbg` attaches to an already-running
  process (`DebugActiveProcess`), so there is no process-launch step at
  which to redirect its standard handles — only `OutputDebugString` calls
  reach the Output window. See DESIGN.md §1's "Debuggee output (v1)" note.
- **No edit-and-continue, memory/disassembly view, or multi-process
  debugging** (no auto-attach to child processes).

See [PLAN.md](PLAN.md) for the complete list of deferred features.
