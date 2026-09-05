# wish dbg Module — Architecture & Design

**Status: proposed, not yet implemented.** This document is the
pre-implementation design artifact — see [PLAN.md](PLAN.md) for the
step-by-step build sequence and "10. Implementation Status" below.

## 1. Purpose / Scope

The `dbg` module (`wish client --run=dbg`) is a VS Code-style debugger
frontend: it attaches to a process on the **client** machine, drives that
OS's native debugging API to control and inspect it, and renders the
debugger UI (source view, breakpoints, call stack, threads, watch, output)
on the **server**. Like every other tool in this repo, it is a **pure
frontend** over an existing capability — here, the OS debug API — never a
reimplementation of it.

Scope for v1, established with the user up front:

- **Backend**: Windows only. The client-side debug engine uses the Win32
  debug API (`DebugActiveProcess` / `WaitForDebugEvent` /
  `ContinueDebugEvent`) and `DbgHelp` (`SymInitialize` /
  `SymLoadModuleEx` / `SymFromAddr` / `SymGetLineFromAddr64`) for
  PDB-based symbol and file/line resolution — the API the user pointed at
  directly ([Win32 Debugging Functions](https://learn.microsoft.com/en-us/windows/win32/debug/debugging-functions)).
  Behind a small `debug_backend` interface (§3) so a Linux ptrace+DWARF
  backend can be added later without redesigning the rest of the module —
  explicitly deferred, see §10.
- **Debugging functionality**: attach/detach, pause/resume, step
  into/over/out, source-line breakpoints, call stack + thread list
  (clicking a thread swaps the shown call stack), a variable watch, and a
  debug output/log window.
- **Source files**: read-only, opened server-side via `TextEditor`. The
  server is configured with `wish::server::set_allow_absolute_paths(true)`
  and the client sends absolute source paths resolved from the debuggee's
  PDB info; see §6 for the tradeoff this accepts and §7 for the deployment
  constraint it implies.
- **Look and feel**: Visual Studio Code is the explicit visual/UX
  reference. One deliberate divergence is called out in §6 (breakpoint
  toggling is right-click, not left-click).

Explicitly deferred — see §10 and [PLAN.md](PLAN.md): a Linux/macOS
backend, remote (cross-machine) debuggee attach, conditional/logpoint
breakpoints, full expression evaluation, edit-and-continue, a
memory/disassembly view, and multi-process debugging.

This directory owns:

- `server/dbg.hpp`/`.cpp` — the `DebuggerFrontend` form (six windows, the
  `update_*` render methods, the `*_requested` events).
- `client/dbg.hpp`/`.cpp` — the client runner (`run_dbg`, event wiring, app
  registration).
- `client/debug_backend.hpp` — the platform-agnostic debug-engine
  interface.
- `client/win32_debug_backend.hpp`/`.cpp` — the Windows implementation
  (Win32 debug API + DbgHelp).
- `client/dbg_source.hpp`/`.cpp` — owns the backend + proxy, translates
  backend callbacks into `update_*` RMI calls and `*_requested` events into
  backend calls.
- `README.md` — user-facing usage.
- `dbg_mock.json` / `dbg_mock.html` — the UI mockup validated in the
  `editor` tool before implementation.

This module also extends an existing core widget — `TextEditor`
(`src/ui/ui_elements/text_editor.cpp`, `src/imgui/imgui_text_editor_renderer.cpp`)
gains `breakpoint_lines`, `current_line`, and a `"line_context_menu"` event
(§6, §8). This is the one piece of shared wish code the module touches; no
other core file changes.

## 2. Design Goals

1. **The client owns 100% of OS/debug-API interaction.** The debuggee, its
   memory, its registers, and the OS debug event stream are only ever
   touched by `win32_debug_backend` on the client. The server never
   attaches to a process, reads memory, or knows what OS the debuggee runs
   on — it only renders snapshots and emits `*_requested` events, the same
   split `docker`/`git`/`kubectl` use for their own real-world resource.

2. **A rebuild is a full rebuild, never an incremental patch.** Every
   `update_threads` / `update_callstack` / `update_watch` /
   `update_breakpoints` call clears and fully repopulates the table/tree it
   owns. Simpler and correct-by-construction, matching `docker`/`git`'s
   Design Goal 4 — acceptable because these updates fire only on
   stop/step/selection events (Goal 3), never a hot loop.

3. **No background polling of Threads / Call Stack / Watch / Breakpoints —
   every update is event-driven.** These views change only when the
   debuggee actually stops (breakpoint hit, step complete, pause) or the
   user changes thread/frame selection; pushing an update at any other time
   would cause the same window "vibration" `git`/`docker` documented and
   removed. A future live register/memory view, if ever added, should
   instead follow `docker`'s Stats-window pattern (a single bounded poll
   thread updating one plot/table in place) — not extend this rule.

4. **Async, selection-driven updates carry a staleness guard.** Call-stack
   and watch data is resolved off a background debug-event thread and
   arrives at the server asynchronously; by the time it lands, the user may
   have selected a different thread or frame. `update_callstack` /
   `update_watch` echo the `thread_id`/`frame_id` they were computed for
   and the server drops the call if it no longer matches the current
   selection — the exact `do_update_diff` / `update_logs` pattern from
   `git`/`docker`.

5. **No vendored-library modifications.** Breakpoint markers and the
   current-execution-line indicator are built by extending wish's own
   `TextEditor` wrapper, using hooks the vendored
   `extern/imgui_color_text_edit/TextEditor.h` already exposes
   (`SetLineDecorator`, `SetUserData`, `SetLineNumberContextMenuCallback`) —
   the vendored file itself is never edited.

## 3. Key Abstractions

### `DebuggerFrontend` (server, `form`)

Bison class `"DebuggerFrontend"` in the `"wish"` namespace. Owns **six
independently dockable `Window`s**, each hand-registered in `on_init()` via
the same idiom every multi-window module uses (`sess().ui_objects.merge(tree,
root_key)` + `sess().top_level_objects[root_key]` +
`sess().top_level_handlers[root_key] = this` + `(*root)["__path__"] =
root_key`). Root keys are `internal_root_key_` (Source, the main window)
and `internal_root_key_ + "_threads" / "_callstack" / "_watch" /
"_breakpoints" / "_output"`.

- **Source** (`internal_root_key_`, the main window): a toolbar
  (Attach/Detach, Pause/Continue, Step Into/Over/Out — enabled/disabled per
  `set_run_state`) and a `TabBar` of `TabItem`s, one `TextEditor`
  (extended, `read_only = true`) per open file. `breakpoint_lines` and
  `current_line` are set on the `TextEditor` instance for the file that
  currently has the execution pointer.
- **Threads** (`internal_root_key_ + "_threads"`): a `Table` (Id, State,
  Current Function). Row click emits `select_thread_requested{thread_id}`.
- **Call Stack** (`internal_root_key_ + "_callstack"`): a `Table` (# /
  Function / File / Line) for the currently selected thread. Frame click
  emits `select_frame_requested{frame_id}`, which also drives the Source
  window to that frame's file/line and refreshes Watch for that frame.
- **Watch** (`internal_root_key_ + "_watch"`): a toolbar (`InputText` +
  Add button, emitting `add_watch_requested{expr}`) and a `Table`
  (Name / Value / Type) rebuilt from the currently selected frame.
- **Breakpoints** (`internal_root_key_ + "_breakpoints"`): a `Table`
  (File, Line, Enabled) listing every breakpoint, with a per-row
  enable/remove `MenuButton`. Populated only from the Source window's
  gutter toggles (§6) — there is no separate "add breakpoint by
  file:line" entry point in v1.
- **Output** (`internal_root_key_ + "_output"`): a FIFO-capped `Table`
  (`kMaxOutputRows`, mirroring `docker`'s `kMaxConsoleRows = 500`
  convention), one row per debug event (attach, stop reason, exception) or
  debuggee stdout/stderr line, colour-coded by severity.

Closing any window emits `"closed"` and tears down all six subtrees; the
client should `detach()` the backend and `signal_done()`.

### `debug_backend` (client, interface)

Platform-agnostic surface `dbg_source` drives:

```cpp
class debug_backend {
 public:
  virtual ~debug_backend() = default;
  virtual bool attach(uint32_t pid) = 0;
  virtual void detach() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void step_into(uint32_t thread_id) = 0;
  virtual void step_over(uint32_t thread_id) = 0;
  virtual void step_out(uint32_t thread_id) = 0;
  virtual bool set_breakpoint(const std::string& file, int line) = 0;
  virtual void clear_breakpoint(const std::string& file, int line) = 0;
  virtual std::vector<thread_info> get_threads() = 0;
  virtual std::vector<frame_info> get_callstack(uint32_t thread_id) = 0;
  virtual std::vector<watch_entry> evaluate(uint32_t frame_id,
                                             const std::vector<std::string>& exprs) = 0;
  // Fired from the backend's own debug-event thread on any stop
  // (breakpoint hit, step complete, exception, pause):
  using stop_callback = std::function<void(const stop_event&)>;
  virtual void on_stop(stop_callback cb) = 0;
};
```

`stop_event` carries `{thread_id, reason, file, line}` — the information
needed to drive the Source/Threads/Call Stack refresh fan-out (§4).

### `win32_debug_backend` (client, v1 implementation)

Implements `debug_backend` on Windows:

- `attach()` calls `DebugActiveProcess`, then starts a **dedicated debug
  thread** that loops on `WaitForDebugEvent`/`ContinueDebugEvent` — per
  the Win32 API's own constraint that only the thread which attached to
  the process may wait for its debug events, this loop cannot run on the
  RMI dispatch thread or any general worker thread.
- Symbol/line resolution uses `DbgHelp`: `SymInitialize` once per attached
  process, `SymLoadModuleEx` on each `LOAD_DLL_DEBUG_EVENT` /
  `CREATE_PROCESS_DEBUG_EVENT`, `SymFromAddr` + `SymGetLineFromAddr64` to
  turn an instruction pointer into `{function, file, line}` for the call
  stack and the current-execution-line indicator.
- Breakpoints are software breakpoints: `set_breakpoint` resolves
  file:line to an address via `DbgHelp`, saves the original byte, and
  writes `0xCC` (`INT3`) via `WriteProcessMemory`; the debug thread
  restores the original byte and rewinds `EIP`/`RIP` by one on hit before
  reporting the stop.
- Stepping uses the trap flag (single-step) for Step Into, and a
  temporary breakpoint at the return address (read off the stack) for
  Step Over/Out — the same approach cited in the researched Win32
  debugging references.

### `dbg_source` (client)

Owns the proxy and a `debug_backend`. Wires every `*_requested` event to a
backend call (mirrors `docker_source`/`git_repo_source`'s one-event-one-method
shape), and wires `debug_backend::on_stop` to push the full `update_*` fan-out
described in §4. `evaluate()` results become `update_watch`; `get_threads()`
becomes `update_threads`; `get_callstack()` becomes `update_callstack`.

## 4. Data Flow / Architecture

```
Startup:
  run_dbg(host)
    instantiate DebuggerFrontend -> proxy
    wire proxy.onEvent(...) for every *_requested / closed event
    dbg_source owns a win32_debug_backend, wires backend.on_stop(...)

Attach flow:
  User enters PID, clicks Attach -> attach_requested{pid}
    dbg_source: backend->attach(pid)
      win32_debug_backend: DebugActiveProcess(pid), spawn debug thread
      debug thread: WaitForDebugEvent loop; CREATE_PROCESS_DEBUG_EVENT ->
        SymInitialize + SymLoadModuleEx for the main module
    initial stop (process creation halts the debuggee) -> on_stop(evt)
      dbg_source: get_threads() -> update_threads({threads:[...]})
                  get_callstack(evt.thread_id) -> update_callstack({...})
                  open_file_requested-equivalent: push update_source /
                    ensure a TextEditor tab exists for evt.file, set
                    current_line = evt.line
                  set_run_state("paused")

Breakpoint set flow:
  User right-clicks a line number in an open TextEditor tab -> Source
  window's "line_context_menu" event {line, has_breakpoint: false}
    -> Toggle Breakpoint menu item -> toggle_breakpoint_requested{path, line}
      dbg_source: backend->set_breakpoint(path, line)
        win32_debug_backend: resolve address via DbgHelp, patch INT3
      -> update_breakpoints({breakpoints:[...]})  (full rebuild)
      -> the open tab's breakpoint_lines field gains `line`

Breakpoint hit / step flow:
  debug thread: WaitForDebugEvent returns EXCEPTION_DEBUG_EVENT
    (STATUS_BREAKPOINT at a known INT3 address, or a single-step trap)
    -> restore original byte / rewind IP as needed -> on_stop(evt)
      dbg_source: get_threads() -> update_threads({...})
                  get_callstack(evt.thread_id) -> update_callstack({thread_id, frames:[...]})
                  evaluate(top_frame_id, watch_exprs) -> update_watch({frame_id, entries:[...]})
                  ensure the Source tab for evt.file is open, set current_line = evt.line
                  set_run_state("paused")

Thread / frame selection:
  Threads row click -> select_thread_requested{thread_id}
    dbg_source: get_callstack(thread_id) -> update_callstack({thread_id, frames:[...]})
  Call Stack row click -> select_frame_requested{frame_id}
    dbg_source: evaluate(frame_id, watch_exprs) -> update_watch({frame_id, entries:[...]})
                open/focus the frame's file at its line in the Source window

Step flow:
  Step Into/Over/Out button -> step_requested{kind, thread_id}
    dbg_source: backend->step_into/over/out(thread_id)
      win32_debug_backend: set trap flag or temp return-address breakpoint,
        ContinueDebugEvent(DBG_CONTINUE)
    -> next WaitForDebugEvent single-step/breakpoint event -> on_stop(evt)
      (same fan-out as "Breakpoint hit / step flow" above)

Pause/Resume/Detach:
  pause_requested  -> backend->pause() (DebugBreakProcess) -> on_stop(evt), set_run_state("paused")
  resume_requested -> backend->resume() (ContinueDebugEvent(DBG_CONTINUE)) -> set_run_state("running")
  detach_requested -> backend->detach() (clear all breakpoints' INT3 patches, DebugActiveProcessStop)
```

## 5. Public API Contract

| Symbol | Contract |
|---|---|
| `DebuggerFrontend.update_threads(args)` | RMI method. `args.threads` — dynamic array, each `{ id (uint32), state ("running"/"suspended"), current_function (string) }`. Fully rebuilds the Threads table. |
| `DebuggerFrontend.update_callstack(args)` | `{ thread_id, frames: [{ index, function, file, line }] }`. `thread_id` is echoed from the request that triggered it; discarded if it no longer matches the Threads window's current selection. |
| `DebuggerFrontend.update_watch(args)` | `{ frame_id, entries: [{ name, value, type }] }`. Same staleness guard on `frame_id`. |
| `DebuggerFrontend.update_breakpoints(args)` | `{ breakpoints: [{ file, line, enabled }] }`. Full rebuild of the Breakpoints table. |
| `DebuggerFrontend.update_source(args)` | `{ path, breakpoint_lines: [int], current_line (int, 0 = none) }`. Ensures a `TabItem`/`TextEditor` exists for `path` (opening one if needed) and sets its `breakpoint_lines`/`current_line` fields. |
| `DebuggerFrontend.append_output(args)` | `{ text, level ("info"/"warn"/"error") }`. Appends one row to the Output table; FIFO-capped at `kMaxOutputRows`. |
| `DebuggerFrontend.set_run_state(args)` | `{ state ("detached"/"running"/"paused") }`. Drives toolbar button enablement (Pause enabled only when running; Continue/Step only when paused; Attach only when detached). |
| `"attach_requested"` event | `{ pid (uint32) }` — from the toolbar's process-id field + Attach button. |
| `"detach_requested"` event | No payload. |
| `"pause_requested"` / `"resume_requested"` event | No payload. |
| `"step_requested"` event | `{ kind ("into"/"over"/"out"), thread_id }`. |
| `"toggle_breakpoint_requested"` event | `{ path, line }` — from a Source tab's right-click "Toggle Breakpoint" menu item, or the Breakpoints window's per-row remove action. |
| `"select_thread_requested"` event | `{ thread_id }` — Threads table row click. |
| `"select_frame_requested"` event | `{ frame_id }` — Call Stack table row click. |
| `"add_watch_requested"` event | `{ expr }` — Watch window's inline field. |
| `"open_file_requested"` event | `{ path, line }` — double-clicking a Call Stack frame whose file has no open tab yet. |
| `"closed"` event | Any window's X was clicked; all six subtrees torn down. The client should detach and `signal_done()`. |
| `"line_context_menu"` event (on `TextEditor`, §8) | `{ line, has_breakpoint }` — fired by the extended `TextEditor` widget's right-click line-number menu; the Source window's own handler turns this into `toggle_breakpoint_requested`. |
| `wish client --run=dbg` | No positional args in v1 — the process to attach to is chosen via the toolbar's PID field once the UI is up. |

The internal `ui_element` tree of every window is private — clients must
not address its nodes by dot-path (they instantiate `DebuggerFrontend` and
use only the methods/events above).

## 6. Design Decisions

- **Source files are opened via `set_allow_absolute_paths(true)`, not a
  private sandboxed project root.** The alternative considered was a
  `project_root_` field on the form validated with the standalone
  `file_service::resolve_path()` utility, with matched files mirrored into
  the session's own sandbox `resource_dir` before handing a relative path
  to `TextEditor` — avoiding any change to the server-wide sandbox posture.
  The user chose the simpler `set_allow_absolute_paths(true)` route
  instead: the client resolves the debuggee's PDB-reported source path and
  passes it straight through as an absolute `TextEditor.file_path`.
  **Tradeoff accepted**: per `file_service.hpp`'s documented contract, this
  flag disables path sandboxing for *every* widget in the session, not
  just the debugger's — acceptable here because `dbg` is inherently a
  trusted-developer tool typically run client and server on the same
  machine, unlike a multi-tenant server. §7 states this as a deployment
  constraint.

- **Breakpoint markers and the current-line indicator extend wish's own
  `TextEditor` wrapper, not the vendored library, and not a custom
  `Table`-based source view.** `extern/imgui_color_text_edit/TextEditor.h`
  already exposes `SetLineDecorator` (custom-drawn gutter column),
  `SetUserData`/`GetUserData` (stable line-keyed data), and
  `SetLineNumberContextMenuCallback` (a popup on right-clicking a line
  number) — none of which `src/ui/ui_elements/text_editor.cpp` currently
  surfaces as bison fields/events. Adding `breakpoint_lines`,
  `current_line`, and a `"line_context_menu"` event to the wish wrapper
  (§8) reuses these hooks without editing the vendored file, and keeps
  real syntax highlighting for the source view — the alternative (a
  `Table` with one row per line, `git`'s diff-viewer technique) needs no
  widget changes at all but gives up highlighting entirely, which would be
  a poor fit for a tool whose explicit north star is VS Code.

- **Breakpoints toggle via right-click, not left-click — a deliberate,
  called-out divergence from VS Code.** The vendored library's only
  line-number interaction hook is `SetLineNumberContextMenuCallback`
  (right-click); there is no left-click gutter hook to bind a toggle to.
  Toggling is therefore: right-click a line number → "Toggle Breakpoint"
  menu item → `"line_context_menu"` event → `toggle_breakpoint_requested`.
  A left-click gutter toggle would require adding a *new* hook to the
  vendored library, which Design Goal 5 rules out for v1.

- **Windows-only v1, behind a `debug_backend` interface.** The user's own
  reference and hardware are Windows; a Linux ptrace+DWARF backend is a
  substantial, independently-scoped effort (different attach model,
  different symbol format, different breakpoint/step mechanics). Defining
  `debug_backend` now (§3) means `DebuggerFrontend`, `dbg_source`, and
  every RMI contract are backend-agnostic from the start — adding
  `linux_debug_backend` later is additive, not a redesign. This mirrors
  bison's own `debugger_posix.cpp` / `debugger_win.cpp` split (TracerPid
  polling vs. `IsDebuggerPresent()`) for a comparable OS-detection
  divergence, and follows CLAUDE.md's Platform Support guidance to keep
  genuine platform differences behind a narrow, well-defined seam rather
  than scattering `#ifdef`s.

- **Full rebuild per update, never incremental, with staleness guards on
  selection-driven data.** Directly reused from `docker`/`git` (§2 Goals
  2 and 4) — Threads/Call Stack/Watch/Breakpoints updates are infrequent
  (only on stop/step/selection) so the simplicity of "clear and
  repopulate" costs nothing, and async call-stack/watch resolution
  (running off the debug-event thread, §3) needs the same
  request-echo-and-drop-if-stale pattern `git`'s `do_update_diff` and
  `docker`'s `update_logs`/`update_inspect` established.

- **No background polling for Threads / Call Stack / Watch / Breakpoints.**
  These only change when the debuggee actually stops or the user changes
  selection — polling them on a timer would rebuild live UI state with no
  new information most of the time, the exact "vibration" `docker`/`git`
  identified and removed. If a future live register/memory view is added,
  it should copy `docker`'s Stats-window shape instead (a single bounded
  poll thread updating one plot/table in place, never a full-tree
  rebuild) — not be used as precedent for polling here.

- **Each table's row-key counter is local to its rebuild, never a shared
  member.** `bison::dynamic::size()` reports "highest numeric key + 1", not
  a true count (`bison_object.hpp`) — the same gotcha documented in
  `docker`/`git`'s Design Decisions. Since `update_threads` /
  `update_callstack` / `update_watch` / `update_breakpoints` each rebuild
  one table in a single call, a `size_t` local at the top of each rebuild
  loop is correct by construction.

## 7. Constraints and Invariants

- The server form never touches the debuggee process, the OS debug API, or
  any client-machine file directly; `win32_debug_backend` / `dbg_source`
  (client-only) own that entirely.
- The debug-event `WaitForDebugEvent` loop runs on its own dedicated
  thread, never the RMI dispatch thread — only the thread that attached to
  a process may wait for its debug events (a hard Win32 constraint, not a
  style choice).
- All debuggee memory/register access (`ReadProcessMemory`,
  `WriteProcessMemory`, `GetThreadContext`/`SetThreadContext`) is confined
  to `win32_debug_backend`; no other client or server code touches the
  debuggee's address space.
- **Deployment requirement**: because source files are opened via
  `set_allow_absolute_paths(true)` (§6), the server process must be run
  with that flag enabled, and the deployment must trust the client's
  reported source paths — document this plainly in the module's
  `README.md`, the same way `docker`'s README documents its own "`docker`
  must be on `PATH`" requirement.
- Every `update_*` RMI handler fully clears its owned table/tree before
  repopulating — never appends.
- `update_callstack` / `update_watch` calls that fail their staleness
  guard (thread/frame no longer selected) are dropped silently.
- Removing a breakpoint on detach: `win32_debug_backend::detach()` must
  restore every patched `INT3` byte back to its original value before
  calling `DebugActiveProcessStop` — a debuggee that continues running
  after detach must not crash on a leftover breakpoint.

## 8. Integration Boundaries

Depends on:

- `wish::form`, `ui_root::on_event`'s catch-all dispatch, `import_json()` —
  server-side UI construction, the same pattern every other module uses.
- `TextEditor` (`read_only`, `language`, plus the new
  `breakpoint_lines`/`current_line`/`"line_context_menu"` additions this
  module introduces), `TabBar`/`TabItem`, `Table`/`TableColumn`/`TableRow`,
  `MenuButton`/`MenuItem`, `InputText`/`InputInt`, `HorizontalLayout` /
  `VerticalLayout` — all existing wish elements except the `TextEditor`
  extension.
- `style_service` — the `"dark"` preset for a VS Code-like theme.
- `wish::server::set_allow_absolute_paths(true)` (§6, §7).
- Win32 `debugapi.h` (`DebugActiveProcess`, `WaitForDebugEvent`,
  `ContinueDebugEvent`, `DebugBreakProcess`, `DebugActiveProcessStop`) and
  `DbgHelp` (`SymInitialize`, `SymLoadModuleEx`, `SymFromAddr`,
  `SymGetLineFromAddr64`) — Windows-only, linked only into
  `win32_debug_backend`'s translation unit.

Extends:

- `src/ui/ui_elements/text_editor.cpp` + `src/imgui/imgui_text_editor_renderer.cpp`
  — new `breakpoint_lines` (int array) and `current_line` (int) fields, and
  a new `"line_context_menu"` event wired to the vendored library's
  `SetLineDecorator` (breakpoint dot + current-line arrow) and
  `SetLineNumberContextMenuCallback` (right-click menu). This is a small,
  additive change to shared widget code — no existing field, event, or
  rendering behavior changes for callers that don't set the new fields.

Depended on by: nothing else in wish; this is a leaf module.

## 9. Testing

- **`tests/test_dbg.cpp`** — instantiate `DebuggerFrontend` over
  `memory_transport`, feed hand-built `update_threads` / `update_callstack`
  / `update_watch` / `update_breakpoints` / `update_source` snapshots, and
  assert: table/tab construction and full-rebuild correctness; a
  `update_callstack`/`update_watch` call with a stale `thread_id`/`frame_id`
  is a no-op; a Threads row click emits `select_thread_requested`; a
  right-click "Toggle Breakpoint" menu action emits
  `toggle_breakpoint_requested`; `set_run_state` correctly enables/disables
  the toolbar buttons. Mirrors `tests/test_docker.cpp`'s shape. No real
  process attach required.
- **`tests/test_win32_debug_backend.cpp`** — drives `win32_debug_backend`
  against a small, throwaway test executable built with debug info:
  attach, set a breakpoint, resume, assert the stop event reports the
  expected file/line; single-step; detach and confirm the process
  continues to completion with no leftover `INT3` (i.e. the patched byte
  was restored). Windows-only test, skipped on other platforms.
- **`tests/test_text_editor_breakpoints.cpp`** (or added to an existing
  `TextEditor` test file) — the extended fields/event: setting
  `breakpoint_lines`/`current_line` and asserting the renderer invokes the
  vendored library's decorator/user-data hooks with the expected lines;
  the `"line_context_menu"` event payload shape on a right-click.
- **End-to-end**: automation module against `wish client --run=dbg`
  attached to a real fixture executable — see [PLAN.md](PLAN.md)'s
  Verification section.

## 10. Implementation Status

**Not yet implemented.** This DESIGN.md and the accompanying
[PLAN.md](PLAN.md) are the pre-implementation design pass; see PLAN.md's
Steps for what ships in what order and its "Not implemented" section for
what is explicitly out of scope for v1 (Linux/macOS backend, remote
attach, conditional/logpoint breakpoints, full expression evaluation,
edit-and-continue, memory/disassembly view, multi-process debugging).
