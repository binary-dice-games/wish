# tail

`tail`-like log viewer, with a command-line surface modeled on the Linux
`tail` tool (`-f`, `-n`). Raw lines are colorized by severity and filterable
by a live regex; any line carrying a `[Tag]` token (e.g. `[Renderer]`) is
also mirrored into its own dedicated tab.

- **server/**: `Tail` form (`register_tail()`) -- parses/classifies
  raw lines (via `log_line_parser`, configured from `patterns.json` below)
  and renders them into a scrolling table per tab. The toolbar's Follow
  checkbox (on by default) controls whether each table auto-scrolls to the
  newest row as lines arrive; unchecking it lets you browse older rows
  without being pulled back to the bottom. The server never touches the
  filesystem; it only renders whatever `push_lines` gives it.
- **client/**: `run_tail(wish_app_host&)`, self-registered as the
  `"tail"` embedded app -- owns actual `tail`-style file reading (and,
  with `-f`, following) of files on the client's local machine, one
  background thread per file. Usage:

  ```sh
  wish client --run=tail -- [-f] [-n N|+N] [-e REGEX] FILE [FILE...]
  ```

  | Flag | Meaning |
  |------|---------|
  | `-f`, `--follow` | Keep watching each file for appended lines. |
  | `-n N`, `--lines N` | Show the last N lines initially (default 100); also sets the toolbar's live row cap (see below). |
  | `-n +N` | Show lines starting from line N (from the start of the file). |
  | `-e REGEX`, `--filter REGEX` | Set the form's initial filter regex. |
  | `-q`, `-v` | Accepted for command-line compatibility with real `tail`; every row already carries its own Source column, so there is no separate per-file header to suppress/force. |

- **resources/**: `patterns.json` -- the line-parsing rule set: regexes that
  extract a timestamp/severity/message from a raw line (`line_formats`, first
  match wins), regexes that classify a severity level and its display color
  for both a light and a dark background (`level_rules`, `light_color` /
  `dark_color`), and the regex used to spot `[Tag]` tokens (`tag_pattern`).
  Lands per-session at `res/bdg/desktop/tail/patterns.json` (see
  `modules/README.md`'s "Per-module embedded resources"); edit the source
  file and rebuild to add support for your own log format, or to change how
  an existing one is colorized. Ships with rules for `docker logs
  --timestamps`, classic syslog, Python's default `logging` format, ISO-8601
  timestamp + severity word, and a couple of bare `LEVEL: message` shapes,
  falling back to an unparsed catch-all so any line is still shown.

A filter set via the toolbar (or `-e`) controls row *visibility*, not
admission: every line is always added as a row, and changing the pattern
immediately shows/hides already-received rows to match -- so you can edit
the filter dynamically to search through lines already on screen.

The toolbar's Lines field starts at `-n`'s count and doubles as a *live*
cap on each table's row count from then on (clamped to at most 2000):
whenever a table would hold more rows than the field currently says, the
oldest are dropped immediately -- whether that's from new lines arriving
while following or from lowering the field itself. Since the server never
touches the filesystem, raising the field from the UI additionally clears
the table and asks the client to re-read each tailed file's last N lines,
so raising it really does surface more history, not just relax future
eviction.
