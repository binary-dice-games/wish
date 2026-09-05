# wish dbg Module — Implementation Plan

See [DESIGN.md](DESIGN.md) for the architecture this plan implements — the
client/server split (all Win32 debug API / DbgHelp interaction
client-side, server only renders), the `set_allow_absolute_paths(true)`
decision for source files, the `TextEditor` extension for breakpoints and
the current-execution-line indicator, and the six-window layout.

## Steps

1. **DESIGN.md + UI mockup** — this `DESIGN.md`, `dbg_mock.json` (renders
   in the `editor` tool) and `dbg_mock.html` (full-width review of all six
   windows: Source, Threads, Call Stack, Watch, Breakpoints, Output). To be
   validated with the user before implementation begins, per every other
   module's Step 1 convention.

2. **`TextEditor` extension** — add `breakpoint_lines` (int array) and
   `current_line` (int, 0 = none) fields plus a `"line_context_menu"` event
   to `src/ui/ui_elements/text_editor.cpp`, and the corresponding render
   support in `src/imgui/imgui_text_editor_renderer.cpp`: a
   `SetLineDecorator` callback drawing a breakpoint dot for each line in
   `breakpoint_lines` and a current-line arrow/highlight at `current_line`,
   and a `SetLineNumberContextMenuCallback` that emits `"line_context_menu"
   {line, has_breakpoint}`.
   - Tests: a renderer/RMI-level test confirming the new fields drive the
     vendored library's `SetUserData`/`SetLineDecorator` state correctly
     and the event payload shape on a simulated right-click.

3. **Scaffold + Threads/Call Stack windows against a fake backend** —
   `server/dbg.{hpp,cpp}` (`DebuggerFrontend` form, all six window
   registrations, `update_threads`/`update_callstack` with the
   `thread_id` staleness guard), `client/dbg.{hpp,cpp}` (`run_dbg` +
   registrar), `client/debug_backend.hpp` (the interface only).
   Wire `dbg_source` against a synthetic in-test `debug_backend`
   implementation first (no real process attach) to prove out the RMI
   contract before `win32_debug_backend` exists.
   - Tests: `tests/test_dbg.cpp` — window construction, `update_threads` /
     `update_callstack` full-rebuild correctness, stale `thread_id` no-op,
     Threads row click emits `select_thread_requested`.

4. **`win32_debug_backend`** — `client/win32_debug_backend.{hpp,cpp}`:
   `attach`/`detach` (`DebugActiveProcess`/`DebugActiveProcessStop`), the
   dedicated `WaitForDebugEvent`/`ContinueDebugEvent` thread,
   `pause`/`resume` (`DebugBreakProcess`), `DbgHelp` symbol handle setup
   (`SymInitialize`, `SymLoadModuleEx` on module-load events) and
   address→`{function, file, line}` resolution (`SymFromAddr`,
   `SymGetLineFromAddr64`).
   - Tests: `tests/test_win32_debug_backend.cpp` against a small, checked-in
     test executable with debug info — attach, resume, assert the initial
     stop reports the expected module/entry location; detach cleanly.
     Windows-only, skipped on other platforms.

5. **Source window + breakpoints end-to-end** — `open_file_requested` /
   tab management in `server/dbg.cpp`; `toggle_breakpoint_requested` wired
   through `dbg_source` to `win32_debug_backend::set_breakpoint`/
   `clear_breakpoint` (address resolution + `INT3` patch/restore via
   `WriteProcessMemory`); `update_breakpoints` full-rebuild; the debug
   thread's `EXCEPTION_DEBUG_EVENT`/`STATUS_BREAKPOINT` handling
   (restore original byte, rewind IP, report the stop) driving
   `update_source`'s `current_line`.
   - Tests: extend `test_win32_debug_backend.cpp` with a set-breakpoint /
     resume / assert-stop-at-expected-line / detach-leaves-no-`INT3` case;
     extend `test_dbg.cpp` with the right-click "Toggle Breakpoint" menu
     action emitting `toggle_breakpoint_requested` and
     `update_breakpoints` rebuild correctness.

6. **Step execution + Watch** — `step_requested{kind, thread_id}` wired to
   `win32_debug_backend::step_into/over/out` (trap-flag single-step for
   Into; a temporary return-address breakpoint for Over/Out);
   `add_watch_requested` + `evaluate()`/`update_watch` (scope: simple local
   variable reads via `DbgHelp` type information — document any expression
   syntax limits in DESIGN.md §1 rather than silently under-supporting
   them).
   - Tests: step-into/over/out cases in `test_win32_debug_backend.cpp`
     against known control flow in the test executable; `test_dbg.cpp`
     cases for `update_watch`'s `frame_id` staleness guard and the Watch
     window's Add flow.

7. **Output/Debug Log window** — `append_output` fed by debug events
   (attach, stop reason, exceptions) and debuggee output
   (`OUTPUT_DEBUG_STRING_EVENT`, plus redirected stdout/stderr pipes set up
   at attach/launch time), FIFO-capped exactly like `docker`'s Console /
   `tail`'s `push_lines`.
   - Tests: `test_dbg.cpp` cases for FIFO capping and severity colour
     coding.

8. **Docs** — this `PLAN.md`, `DESIGN.md` status/§10 update to
   "implemented", module `README.md`, `docs/building.md` CMake option
   (`WISH_MODULE_BDG_DEV_DBG`), `CHANGELOG.md` `### Added`,
   `modules/bdg/dev/README.md` row.

## Verification

- **Unit tests** (no live attach needed for most): `cmake --build build
  --target test_dbg` with `-DWISH_MODULE_BDG_DEV_DBG=ON`; `test_dbg` drives
  `DebuggerFrontend` over `memory_transport` with synthetic `update_*`
  snapshots and a fake `debug_backend`.
- **Windows-only backend tests**: `cmake --build build --target
  test_win32_debug_backend`, run on a Windows host; drives
  `win32_debug_backend` against a small checked-in fixture executable with
  PDB info.
- **End-to-end** (performed against a real attach, not just described):
  build a trivial C++ fixture executable with debug info; launch
  `wish standalone --run=dbg --renderer web` built with
  `-DWISH_ENABLE_AUTOMATION=ON`; attach to the fixture via the toolbar's
  PID field; drive/verify with `wish.automation.AutomationClient`
  (`get_tree()` + screenshot): set a breakpoint by right-clicking a source
  line and confirm the breakpoint dot appears and the row appears in the
  Breakpoints window; Resume and confirm execution stops at that line with
  the current-line indicator shown and the Threads/Call Stack/Watch windows
  populated; Step Into/Over/Out and confirm the current-line indicator and
  Call Stack update accordingly; Detach and confirm the fixture process
  resumes and exits normally (no leftover breakpoint crash).

## Not implemented (deferred future work)

- **Linux/macOS debug backend** (ptrace + DWARF) — `debug_backend` is
  defined to make this additive later, but no implementation exists in v1.
- **Remote (cross-machine) debuggee attach** — the client always attaches
  to a process on its own machine in v1.
- **Conditional breakpoints and logpoints.**
- **Full expression evaluation** in Watch — v1 supports simple variable
  reads resolvable via `DbgHelp` type info, not arbitrary C++ expressions.
- **Edit-and-continue.**
- **Memory view / disassembly window.**
- **Multi-process debugging** (child process auto-attach on
  `CreateProcess`/fork-equivalent).
- **True `docker logs -f`-style streaming isn't applicable here**, but
  analogously: no live register/memory polling view — see DESIGN.md §6 for
  why this would need `docker`'s Stats-window bounded-poll shape if added.
