# editor

Live JSON UI mock editor: open a wish UI JSON file, see it re-parsed and
live-instantiated as you (or an agent) type, with a log of every event the
instantiated preview fires. Meant for iterating on a UI's design and for the
wish skills to visualize a mock to the user during development — see
`.claude/skills/wish-module` and `.claude/skills/wish-ui`.

A syntax error in the source shows an error banner above the source panel
but leaves the last valid preview on screen, so a typo never makes the
preview flicker or disappear. The underlying file is also watched for
changes made outside the tool (e.g. in the user's own editor) and reloaded
automatically.

- **server/**: `Editor` form (`register_editor()`) — owns the source
  `TextEditor` (JSON syntax highlighting), the error banner, the event log
  table, and the preview subtree. Re-parses on every source edit and on
  every `set_source` call; a successful parse swaps in a new preview
  registered as its own top-level window (handled by the same form
  instance, so its events reach the same `on_event`); a failed parse only
  updates the banner. Logs every preview widget event as
  `"<dot-path> <event>"`, e.g. `"main.ok clicked"`.
- **client/**: `run_editor(wish_app_host&)`, self-registered as the
  `"editor"` embedded app — owns the local JSON file (`upload_file`/
  `download_file`, same sandbox-bridging rule as Notepad) and a background
  poll loop that re-uploads the file under a fresh sandbox name whenever it
  changes on disk outside the tool. Requires a startup file path via
  `app_args()`: `wish client --run=editor -- path/to/ui.json` (created
  empty if it doesn't exist yet).
- **resources/**: none.

## Future work

Not implemented in this pass — see the module's server/client sources for
where each would hook in:

- **Help panel**: detect the UI element type at the text cursor's position
  and show its description/properties/methods (from the same `dynamic`
  class registry `Editor` already parses against) in a side panel.
- **Autocompletion**: JSON-schema-driven completion for element types and
  fields as the user types, sourced from the same registry.
- **Element palette**: a browsable list of every registered wish UI class,
  grouped by collection (`ui`, `plot2d`, `plot3d`, ...), for reference and
  drag-in.
