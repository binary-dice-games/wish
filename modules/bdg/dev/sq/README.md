# sq

A DBeaver-style, **query-only** GUI frontend for the local
[`sq`](https://github.com/neilotoole/sq) command line tool: connect to
different kinds of databases (PostgreSQL, MySQL, SQL Server, SQLite,
DuckDB, ...), browse their tables and structure, run SQL queries into a
results grid, and export a result to CSV. This is a pure frontend — every
action runs the real `sq` binary already on the machine, against the
sources in your own `sq` configuration; it never links a database driver.

`wish client --run=sq` (no positional args). Opens as seven independently
dockable windows: SQL Editor, Results, Chart, Connections, Navigator,
Structure, Console.

If `sq` is not on `PATH` the windows still open, with a message and a
dialog pointing at <https://github.com/neilotoole/sq> (see its Install
section); restart the tool once it is installed.

- **server/**: `SqFrontend` form (`register_sq()`) — renders whatever
  snapshot it was last given via `update_connections` / `update_drivers` /
  `update_schema` / `update_result` / `command_result` /
  `append_command_log` / `show_unavailable`, and emits `*_requested`
  events (see `server/sq.hpp`'s class doc comment for the contract).
- **client/**: `run_sq(wish_app_host&)`, self-registered as the `"sq"`
  embedded app. `sq_process` is a small libuv (`uv_spawn`) "run this argv,
  capture stdout/stderr/exit code" helper (a copy of `docker_process` plus
  stdin, used to pass `sq add -p` the password). `sq_source` runs every
  `sq` invocation and pushes the parsed result. `sq_result_parser` parses
  `--jsonl` output and masks passwords in locations; `sq_query_guard`
  rejects non-read-only SQL.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_SQ=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).

## Test database

`testdata/sample.db` (generated from `testdata/sample.sql`) is a SQLite
database for trying the tool: `sq add ./testdata/sample.db --handle
@sample`, then `wish standalone --run=sq`. It has a small relational schema
(artists/albums/tracks/playlists with foreign keys, a composite key and a
view), an `edge_cases` table (NULL vs empty, multi-line, long, unicode and
CSV-hostile text), tables whose names need quoting (`order details`,
`say "hi"`), an empty table, a 30-column `wide` table, and a 12,000-row
`measurement` table for the row limit and full-result CSV export.

## Windows

- **SQL Editor** — Run, an *active database* picker (switch between your
  sq sources), a row-limit picker (100–5000 displayed rows) and a
  SQL-syntax-highlighted `TextEditor` that fills the rest of the window.
  (The editor edits a file in the session sandbox; the form reads the text
  from it on Run.)
- **Results** — Export CSV (target path + Overwrite) above the last result
  as a grid (`#` + one column per result column; SQL NULL shown as
  `NULL`), with a "N rows in T ms" status.
- **Chart** — plots the displayed rows of the last result with `Plot`
  widgets. Pick the chart type (line, scatter, stairs, stems, area, bars,
  horizontal bars, histogram, pie), the X column (or the row number) and
  any number of Y columns (checkboxes; multiple bar series are grouped
  side by side). Non-numeric and NULL cells are skipped; a non-numeric X
  falls back to row numbers. Histograms take a bins choice; pies use the
  first Y column for slice sizes and the X column for labels (first 50
  rows). Choices are remembered across queries by column name.
  **Save PNG** (target path + Overwrite) writes the chart as a 1000x600
  image on the server, in the session folder (see below). The image is drawn by a
  small built-in rasterizer (`server/sq_chart_render.*`, anti-aliased, text in the UI's own default font), so it resembles
  the on-screen plot but is not pixel-identical.
- **Connections** — your sq sources (handle / driver / masked location),
  a per-row menu (Connect, Ping, Remove) and an inline form to add one
  (driver, handle, location, password).
- **Navigator** — a tree of the active database: tables and views with row
  counts, their columns, primary keys and foreign-key references. Each
  table has **View data** (puts `SELECT * FROM "table"` in the editor and
  runs it) and **Structure** buttons.
- **Structure** — the selected table's columns: type, key, nullable and
  what a foreign key references.
- **Console** — a trace of every `sq` command run (green/red by exit code).

## Behavior worth knowing

- **Read-only.** Nothing in the UI can modify data, and the client
  refuses anything but a single `SELECT` / `WITH` / `VALUES` / `SHOW` /
  `DESCRIBE` / `EXPLAIN` statement (and any `INSERT`/`UPDATE`/`DELETE`/
  DDL keyword outside literals and comments). This is a lexical safety
  net, not a security boundary — for untrusted use, connect with a
  read-only database account. "Remove" only unregisters the source from
  `sq`; the database is untouched.
- **The active database is per session.** Every command passes
  `--src <handle>`; the tool never runs `sq src`, so your shell's active
  `sq` source does not change. On start it picks up `sq`'s active source.
- **Only the first N rows are displayed** (the row-limit picker), but
  **Export CSV** re-runs the last query in full (`sq --csv --output` into
  a client-side temp file) and uploads it to the server.
- **Export CSV and Save PNG write on the server** (the **Open folder** button next to each opens that folder in the server machine's file manager), into the session's
  sandbox folder: the path must be relative (no absolute paths, no `..`),
  subfolders are created, and an existing file is only replaced when
  *Overwrite* is ticked. The folder is deleted when the client disconnects
  unless the server was started with `wish server --sandbox_root PATH`
  (files then persist under `PATH/<--username of the client>`), so copy the file out (e.g. with a file-service
  download) before closing.
- **Passwords never appear on a command line**: the Add form passes the
  password to `sq add -p` on stdin, and locations are masked in the
  Connections table and the Console.
- Table names in *View data* are quoted for the driver (`"x"`, `` `x` ``
  for MySQL, `[x]` for SQL Server).

## Known limitations (v1)

- SQL only — sq's own SLQ query language is not exposed.
- An empty result shows no columns (`sq --jsonl` prints nothing for it).
- The whole result is read into memory before the first N rows are
  displayed; very large queries should use a `LIMIT` (Export CSV reads the result into memory too, on upload).
- One query at a time, no query history/tabs, no cancel.
- `sq` must be on `PATH`; sources are added/removed through `sq` itself.
