# editor

Live JSON UI mock editor: open a wish UI JSON file, see it re-parsed and
live-instantiated as you (or an agent) type, with a log of every event the
instantiated preview fires. Meant for iterating on a UI's design and for the
wish skills to visualize a mock to the user during development — see
`.claude/skills/wish-module` and `.claude/skills/wish-ui`.

A syntax error in the source shows an error banner above the source panel
but leaves the last valid preview on screen, so a typo never makes the
preview flicker or disappear. The preview window's position, size, and
focus survive every re-edit — only its content changes. The underlying
file is also watched for changes made outside the tool (e.g. in the user's
own editor) and reloaded automatically. A filename label shows the local
path and an `[MODIFIED]` marker when there are unsaved edits; closing with
unsaved edits shows an inline Save & Close / Discard & Close / Cancel
prompt instead of closing immediately. Copy/paste in the source editor
interoperates with the real OS clipboard (see `src/web/DESIGN.md`'s
"Clipboard Bridging" section).

A **help panel** next to the source shows the description and fields of
whatever element type encloses the cursor, updated live as the cursor
moves. The source editor also **autocompletes** element type names, field
names, and enum/flag values as you type, sourced from the same registered
class registry `Editor` parses against (see
[src/ui/ui_schema_help.hpp](../../../../src/ui/ui_schema_help.hpp)). The
**exact preview widget** the cursor is currently editing is boxed in gold
in the live preview, moving as the cursor moves — so it's obvious at a
glance which widget an edit is about to affect.

- **server/**: `Editor` form (`register_editor()`) — owns the filename
  label, error banner, source `TextEditor` (JSON syntax highlighting,
  `wish_ui_schema` enabled), a help panel label, the close-confirmation
  panel, the event log table, and the preview subtree. Re-parses on every
  source edit and on every `set_source` call; a successful parse swaps in a
  new preview registered as its own top-level window (handled by the same
  form instance, so its events reach the same `on_event`), reusing the same
  preview window id across reparses so ImGui's own position/size/focus
  state isn't reset by every edit; a failed parse only updates the banner.
  Logs every preview widget event as `"<dot-path> <event>"` plus a compact
  rendering of its payload, e.g. `"main.volume changed {value=75}"`; the
  log is capped at 200 rows (oldest evicted) and auto-scrolls to the newest
  entry. Also updates the help panel and the preview highlight box on
  every source `TextEditor` `"cursor_moved"` event, via a hand-rolled
  JSON-cursor scanner (`ui_schema_help::scan_cursor_context()`) that
  tolerates the transiently invalid JSON typical of an in-progress edit
  and resolves the cursor to both an element *type* (for the help panel)
  and its exact dot-*path* within the preview tree (for the highlight).
- **client/**: `run_editor(wish_app_host&)`, self-registered as the
  `"editor"` embedded app — owns the local JSON file (`upload_file`/
  `download_file`, same sandbox-bridging rule as Notepad) and a background
  poll loop that re-uploads the file under a fresh sandbox name whenever it
  changes on disk outside the tool. Requires a startup file path via
  `app_args()`: `wish client --run=editor -- path/to/ui.json` (created
  empty if it doesn't exist yet). Only Ctrl+S (or a confirmed close)
  persists edits back to the local file — in-editor edits update the live
  preview immediately but are not written to disk until saved.
- **resources/**: none.

See [DESIGN.md](DESIGN.md) for the full architecture, including the shared
wish-core bugs this module's development surfaced and fixed (ImGui window
identity stability, `Table` auto-scroll, and the web renderer's clipboard/
modifier-key handling).

## Future work

Not implemented in this pass:

- **Element palette**: a browsable list of every registered wish UI class,
  grouped by collection (`ui`, `plot2d`, `plot3d`, ...), for reference and
  drag-in.
