# notepad

Multi-file, syntax-highlighted text editor. The server-side form never
touches the client's local filesystem — it only edits files already
uploaded into its session sandbox; the client-side runner bridges the two
via `upload_file`/`download_file`.

- **server/**: `Notepad` form (`register_notepad()`) — owns tabs and the
  text editor UI; files live in the session's sandboxed resource directory.
- **client/**: `run_notepad(wish_app_host&)`, self-registered as the
  `"notepad"` embedded app — reacts to the form's high-level events
  (open/new/sync) by moving bytes into and out of the sandbox. Accepts an
  optional startup file path via `app_args()`
  (`wish client --run=notepad -- path/to/file`).
- **resources/**: none.
