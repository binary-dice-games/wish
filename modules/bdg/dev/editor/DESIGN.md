# wish Editor Module — Architecture & Design

**Status: core implemented.** See `server/editor.hpp`/`.cpp`,
`client/editor.hpp`/`.cpp`. Palette, autocomplete, and a cursor-driven help
panel are **not implemented** — see "10. Implementation Status" below.

## 1. Purpose / Scope

The `editor` module (`wish client --run=editor -- path/to/ui.json`) is a
live JSON UI mock editor: a syntax-highlighted source panel next to a
continuously re-parsed, live-instantiated preview of the UI that JSON
describes, plus an event log showing every interaction the preview widget
tree fires. It exists so a developer — or the agentic AI itself, per the
`wish-module`/`wish-ui` skills — can iterate on a UI layout and see the
result immediately, without a build/re-run cycle.

This directory owns:
- `server/editor.hpp`/`.cpp` — the `editor` form (chrome, reparse loop,
  event log, save/close state machine).
- `client/editor.hpp`/`.cpp` — the client runner (local file I/O, external
  file-change watcher).
- `README.md` — user-facing usage.

It does **not** own the fixes this module's development surfaced in shared
wish core code — those live in `src/imgui/imgui_ui_renderer.cpp` (the
`with_id()` ID-stability fix and the `Table` auto-scroll-on-growth
behavior) and `src/web/` (the clipboard bridge and the merged-modifier-key
fix). Those are documented in `src/web/DESIGN.md`'s "Clipboard bridging"
section and this document's "9. Shared Core Fixes" section, not owned by
this module going forward.

## 2. Design Goals

1. **No flicker on a syntax error.** An invalid intermediate edit must
   never blank or reset the preview — the last successfully-parsed UI stays
   on screen, with an error banner, until the source is valid again.
2. **Preview state survives re-edits.** Position, size, and focus of the
   preview window (and of the source editor itself) must not reset on
   every keystroke-triggered reparse — only content should change.
3. **The client never touches the server's filesystem and vice versa.**
   Mirrors every other wish module's file-handling rule (see Notepad):
   the server form only ever reads/writes its session sandbox; the client
   runner owns the real local file and moves bytes across the wire.
4. **Explicit save, not silent autosave.** In-editor edits update the live
   preview immediately but are only written back to the user's local file
   on Ctrl+S (or a confirmed close) — matching Notepad's save contract.
5. **Generic across arbitrary JSON UIs.** The event log and preview
   instantiation must work for any valid wish UI descriptor, not a
   hardcoded set of element types.

## 3. Key Abstractions

### `editor` (server, `form`)

Owns two independent subtrees registered in the session:
- **Chrome** (`internal_root_key_`): filename label, error banner, source
  `TextEditor` (`language: "json"`), close-confirmation panel, event log
  `Table`. Built once in `on_init()`.
- **Preview** (`mock_root_key_ = internal_root_key_ + "_mock"`): the
  live-instantiated result of parsing the current source. Torn down and
  rebuilt on every successful reparse via `try_reparse()`.

Both subtrees are registered as independent entries in
`sess().top_level_objects`/`top_level_handlers`, both pointing `this` as
the event handler — `ui_root::on_event()` is a single catch-all dispatch
per top-level root, so one `editor::on_event()` override handles chrome
button clicks *and* arbitrary preview-widget events, distinguished by
checking known chrome ids first and falling back to a `mock_id_to_path_`
lookup for anything else (see "6. Design Decisions").

Invariant: `mock_window_id_` (the preview root `Window`'s `__wish_id`) is
**reused** across every successful reparse, never regenerated. See "6.
Design Decisions" — this is what makes Goal 2 hold.

### `mock_id_to_path_` (server, private state)

`unordered_map<uint32_t, std::string>` from a preview widget's
`__wish_id.id` to its dot-path within the preview tree, rebuilt on every
successful `try_reparse()`. Used purely to turn an arbitrary preview
widget's event into a readable log line (`"main.ok clicked"`); has no
bearing on rendering.

### `sandbox_source_state` (client, private state)

Tracks the sandbox filename currently backing the source `TextEditor` and
the content the client itself last pushed there, so the external-file
change-watcher thread can distinguish "the file changed because we wrote
it back" from a genuine external edit (avoiding a re-upload feedback
loop).

## 4. Data Flow / Architecture

```
Startup:
  run_editor(host)
    → instantiate Editor
    → push_local_file(): upload local JSON under a fresh sandbox name,
      call set_source({path, display_path})
        → editor::do_set_source(): points source TextEditor.file_path at
          the sandbox file, clears dirty_, calls try_reparse()

In-editor edit:
  TextEditor renderer auto-writes the sandbox file on every undo-stack
  advance, enqueues "changed"
    → editor::on_event(): dirty_ = true, update_path_label(), try_reparse()

try_reparse():
  read sandbox file bytes → import_json()
    on throw (invalid JSON / unknown element type):
      set_banner(e.what()); PREVIOUS preview subtree untouched → no flicker
    on success:
      clear_mock() (removes old preview's ui_objects/top_level_* entries
        and ctx().objects entries)
      assign ids to the new tree -- reuse mock_window_id_ for the root,
        fresh ids for everything else
      merge into ui_objects under mock_root_key_; register as a new
        top_level_objects/top_level_handlers entry
      set_banner("")

Preview widget event (e.g. a mock Button clicked):
  render loop → editor::on_event(widget_id, event, payload)
    → not a known chrome id → look up widget_id in mock_id_to_path_
    → append_log_row("<path> <event>" [+ formatted payload])
      → evict oldest row past kMaxLogRows (200)

External file change (edited outside the tool):
  client's poll thread notices local_path's mtime changed and its content
  differs from sandbox_source_state.last_uploaded_content
    → push_local_file() again under a NEW sandbox name (required: the
      TextEditor renderer only reloads its buffer when file_path itself
      changes) → do_set_source() → try_reparse()

Save (Ctrl+S, or "Save & Close"):
  editor emits "on_source_saved" → client downloads the sandbox file,
  writes it to the local path, calls mark_saved()
    → editor::do_mark_saved(): dirty_ = false; if a close was pending
      (pending_close_after_save_), completes it now

Close:
  window "closed" event
    if dirty_: show the inline confirm panel (Save & Close / Discard &
      Close / Cancel) instead of closing
    else: request_close() -- clear_mock(), emit("closed"), remove chrome
```

## 5. Public API Contract

| Symbol | Contract |
|---|---|
| `Editor.set_source(path, display_path?)` | RMI method. `path` sandbox-relative, required. `display_path` optional, shown in the filename label. Clears `dirty_` (sandbox is now known to match disk) and immediately reparses. Safe to call repeatedly (startup, and every external-file-change detection). |
| `Editor.mark_saved()` | RMI method, no args. Clears `dirty_`; if a "Save & Close" was waiting on this call, completes the close and emits `"closed"`. Must be called by the client only after the sandbox file has actually been downloaded and written to local disk. |
| `"closed"` event | Chrome and preview subtrees fully torn down; the client should call `signal_done()`. Only fires once the user has resolved any unsaved-changes prompt. |
| `"on_source_saved"` event | The client should `download_file()` the current sandbox path, write it to the original local path, then call `mark_saved()`. Fired by Ctrl+S and by "Save & Close". |
| `wish client --run=editor -- <path>` | `<path>` is required (not optional, unlike Notepad's startup-file param) — the editor has nothing useful to show without a source file. Created empty if it doesn't exist yet. |

## 6. Design Decisions

- **Preview registered as an independent top-level root, not nested inside
  the chrome window.** A `form` only auto-manages one root
  (`internal_root_key_`, via `form::init()`/`remove_internal_objects()`).
  The preview needs its own lifecycle (torn down and rebuilt every
  reparse, independently of chrome), so it's registered/deregistered
  directly against `sess().top_level_objects`/`top_level_handlers` in
  `try_reparse()`/`clear_mock()`, with the *same* `editor` instance as
  handler for both roots. Rejected alternative: nesting the preview inside
  a child panel of the chrome window — real wish UIs are typically
  standalone `Window`s themselves, so nesting one Window's render inside
  another doesn't reflect what the described UI actually looks like
  running standalone.

- **Reusing `mock_window_id_` across reparses, not regenerating it.**
  ImGui keys a window's position/size/dock/focus state off its ID string
  (`with_id()` in `imgui_ui_renderer.cpp`, `label + "###" + wish_id`).
  Assigning every reparse's root a fresh `__wish_id` made ImGui treat each
  edit as a brand-new window, resetting any position/size the user had
  dragged it to *and* stealing focus away from the source editor back to
  the (newly-appeared) preview window. Reusing the id keeps ImGui's
  per-window state continuous. Non-root preview elements still get fresh
  ids every reparse — only the root's identity needs to survive, since
  ImGui doesn't track meaningful per-frame state for arbitrary interior
  widgets the way it does for a window's position/focus/dock state.

- **`with_id()` fixed to use `"###"` instead of `"##"` (shared core fix,
  `imgui_ui_renderer.cpp`).** `"##"` hides the suffix from *display* but
  still folds the visible prefix into ImGui's ID hash — so even with a
  stable `wish_id`, editing a `Window`'s own `title` field alone still
  changed its ID and reset position/focus. `"###"` makes the ID depend
  *only* on what follows it, matching the visible-label-independent
  identity `__wish_id` is meant to represent everywhere in wish, not just
  here. Applied globally (`with_id()` is used by every widget type, not
  just `Window`), since wish_id already fully guarantees uniqueness
  regardless of label content — tying ImGui identity additionally to
  label text was a latent bug for any widget whose label a client renames
  at runtime, not an editor-specific concern.

- **A `Table` (with `ImGuiTableFlags_ScrollY`), not a new log-widget
  primitive.** No scrolling-log element existed anywhere in wish. Rather
  than add one, the event log reuses `Table` (headers + fixed
  `outer_height`) with rows appended the same way Notepad appends
  `TabItem`s into `tab_bar`'s `children` — raw `dynamic` children-map
  manipulation, not `ui_objects`-registered dot-paths (so appended rows
  don't pollute `mock_id_to_path_`/automation's tree queries; they render
  but aren't independently addressable by path).

- **Table auto-scroll-to-bottom is a shared core addition
  (`imgui_ui_renderer.cpp`'s `render_table`), not editor-local.** Any
  `ImGuiTableFlags_ScrollY` table now sticks to the bottom when its row
  count grows since the last frame (tracked via a small per-widget
  `table_row_count_cache`), and stops fighting a user who scrolled up when
  the count is unchanged. This benefits any future scrollable-table use
  case, not just this module's log.

- **Event log capped at `kMaxLogRows` (200), oldest evicted.** Uncapped,
  a long-running session's log would grow the `Table`'s `children` map —
  and, more importantly, leak three `ctx().objects` RMI-table entries
  (`TableRow` + 2 `Label` cells) per row — without bound. Eviction removes
  all three ids from `ctx().objects` in addition to the row's slot in the
  table's children map, not just hiding it from rendering.

- **Event names resolved via a hand-maintained lookup table, not a
  generic reverse-hash.** Widget event names (`"clicked"_key`,
  `"changed"_key`, ...) are plain `_key` literals in every wish renderer,
  not `_rkey` — so they're never registered in bison's key-name registry
  and there is no generic hash→string reversal available (unlike class
  names, which `build_display_dict()` can resolve via `DisplayName`
  attributes). `event_name_string()`/`format_payload()` in `editor.cpp`
  hand-list the known vocabulary (grepped from every
  `enqueue_event(...)`/`payload["..."]` call site across
  `src/imgui/imgui_ui_renderer.cpp`); anything unrecognized falls back to
  `"event#<hash>"` rather than silently dropping the log entry.

- **Close confirmation is an inline panel, not a modal dialog.** wish has
  no dedicated modal/popup element. The panel (`vbox.confirm`) is an
  ordinary child layout that starts `"visible": false` and is toggled by
  setting that field — not a true blocking overlay. A second X-click while
  it's already showing just re-shows it (idempotent), since the window
  itself is never actually torn down while `dirty_` is true.

- **`with_session()` dual dispatch/non-dispatch path.** `try_reparse()`
  and `clear_mock()` need `wish::context&` (for `resource_dir`,
  `ui_objects`, `top_level_objects`/`_handlers`) from both `do_set_source`
  (RMI dispatch — `sess()` works) and `on_event()` (render loop, *outside*
  the session lock per `form::on_event`'s contract — `sess()` throws
  there). `with_session()` mirrors `form::remove_internal_objects()`'s own
  branch: use `detail::current_context` if set, else acquire
  `context_wlock{*sync_ctx_}` directly.

## 7. Constraints and Invariants

- The server form never reads/writes any path outside its session
  sandbox; all local-disk access is the client runner's responsibility
  (`file_service::resolve_path` sandboxing applies as usual to
  `current_source_path_`).
- `try_reparse()` must not mutate the preview subtree until the new
  content has *fully* parsed — `clear_mock()` is only called after
  `import_json()` returns successfully (Goal 1).
- A fresh sandbox filename is required on every client-side upload after
  the first (`push_local_file()`'s counter), since `TextEditor`'s renderer
  cache only reloads its buffer when `file_path` itself changes.
- `append_log_row()` must keep `log_rows_.size() <= kMaxLogRows` after
  every call, evicting from the front (oldest) and erasing all three of
  that row's `ctx().objects` entries, not just its table slot.

## 8. Integration Boundaries

Depends on:
- `wish::form`, `ui_root::on_event`'s catch-all dispatch, `import_json()`
  (`src/ui/ui_importer.hpp`) — server-side UI construction.
- `file_service::resolve_path` — sandboxed source file access.
- `wish_app_host` (`upload_file`/`download_file`/`instantiate`) — client
  local-file bridging, same contract as Notepad's client runner.
- `TextEditor` (`src/ui/ui_elements/text_editor.cpp`,
  `src/imgui/imgui_text_editor_renderer.cpp`) — JSON syntax highlighting,
  file-backed autosave-to-sandbox, `"changed"`/`"saved"` events.
- `Table` with `ImGuiTableFlags_ScrollY` — event log rendering/scrolling.
- The web renderer's clipboard bridge (`src/web/DESIGN.md`) — needed for
  copy/paste in the source editor to interoperate with the OS clipboard
  when running under `--renderer=web`.

Depended on by: nothing else in wish; this is a leaf module. The
`wish-module`/`wish-ui` skills reference it as the recommended way to
preview a UI mock during development.

## 9. Shared Core Fixes

Development of this module surfaced three bugs in shared wish core code
(not specific to the editor, fixed at the source):

1. **`with_id()` "##" → "###"** (`src/imgui/imgui_ui_renderer.cpp`) — see
   "6. Design Decisions" above.
2. **`Table` auto-scroll-to-bottom on row growth**
   (`src/imgui/imgui_ui_renderer.cpp`'s `render_table`) — see "6. Design
   Decisions" above.
3. **`web_renderer` never sent merged `ImGuiMod_Ctrl`/`Shift`/`Alt`/`Super`
   key events**, only the literal `LeftCtrl`/`RightCtrl`/etc (`src/web/
   web_renderer.cpp`). `ImGui::IsKeyDown(ImGuiMod_Ctrl)` — which every
   Ctrl-modified shortcut in ImGui, including `TextEditor`'s own
   Ctrl+A/C/V/X, reads — checks a *separate* pseudo-key slot
   (`ImGuiKey_ReservedForModCtrl`) that only an explicit
   `io.AddKeyEvent(ImGuiMod_Ctrl, ...)` populates; sending only the literal
   Left/Right key events (as this renderer previously did) left that slot
   permanently "up". This is why **no** Ctrl-modified keyboard shortcut
   ever worked in the web renderer before this fix — not clipboard-source
   specific. Fixed by tracking each Left/Right modifier's raw down state
   in `web_renderer` and additionally emitting the merged `ImGuiMod_*`
   event on every change, mirroring `imgui_impl_sdl3.cpp`'s
   `ImGui_ImplSDL3_UpdateKeyModifiers()` (the reference pattern every
   official ImGui backend follows). Regression test:
   `WebRendererTest.BeginFrame_LeftCtrlKeyEventAlsoSetsMergedModFlag`
   (`tests/test_web_renderer.cpp`).
4. **Browser↔server OS clipboard bridge** (`src/web/draw_protocol.hpp`/
   `.cpp`, `web_renderer.hpp`/`.cpp`, `resources/embedded/web/client.js`)
   — new `CLIPBOARD_TEXT` (browser→server) / `CLIPBOARD_WRITE`
   (server→browser) message types. Without it, `ImGui::GetClipboardText()`/
   `SetClipboardText()` fall back to ImGui's own session-local buffer —
   copy/paste works *within* one running app but never interoperates with
   content copied from outside the browser tab. See `src/web/DESIGN.md`
   for the full protocol and the async-ordering subtlety in
   `client.js`'s keydown handler (a paste's clipboard read is async, but
   `GetClipboardTextFn` is a synchronous ImGui callback — the client must
   push `CLIPBOARD_TEXT` and queue any other in-flight key events so
   nothing races ahead of the deliberately-delayed paste keydown).

## 10. Implementation Status

**Implemented**: source panel with JSON syntax highlighting and live
reparse, no-flicker error banner, live preview with stable
position/size/focus across edits, generic event log (path + event +
payload) with a 200-row cap and auto-scroll, filename label with
`[MODIFIED]` state, Ctrl+S / close-confirmation save flow, external
file-change watching, OS clipboard copy/paste interop (as of the shared
core fixes above).

**Not implemented** (explicit non-goals for this pass; see the module's
`README.md` "Future work" section for the same list from a user-facing
angle):
- **Help panel**: detecting the UI element type at the text cursor's
  position and showing its description/properties/methods (sourced from
  the same `dynamic` class registry the preview already parses against).
- **Autocompletion**: JSON-schema-driven completion for element types and
  fields as the user types.
- **Element palette**: a browsable list of every registered wish UI class,
  grouped by collection (`ui`, `plot2d`, `plot3d`, ...).

None of these three have any code scaffolding yet — they were scoped out
at the start of this module's implementation (a deliberate, explicit user
decision, not an oversight) precisely because each needs a real design
pass of its own (in particular, mapping a `TextEditor` cursor position
back to a JSON AST node for the help panel has no existing wish primitive
to build on).
