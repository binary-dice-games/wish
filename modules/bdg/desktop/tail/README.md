# tail

`tail`-like log viewer, with a command-line surface modeled on the Linux
`tail` tool (`-f`, `-n`). Raw lines are colorized by severity and filterable
by a live regex; any line carrying a `[Tag]` token (e.g. `[Renderer]`) is
also mirrored into its own dedicated tab.

- **server/**: `Tail` form (`register_tail()`) -- parses/classifies
  raw lines (via `log_line_parser`, configured from `patterns.json` below)
  and renders them into a scrolling, auto-following table per tab. The
  server never touches the filesystem; it only renders whatever
  `push_lines` gives it.
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
  | `-n N`, `--lines N` | Show the last N lines initially (default 10). |
  | `-n +N` | Show lines starting from line N (from the start of the file). |
  | `-e REGEX`, `--filter REGEX` | Set the form's initial filter regex. |
  | `-q`, `-v` | Accepted for command-line compatibility with real `tail`; every row already carries its own Source column, so there is no separate per-file header to suppress/force. |

- **resources/**: `patterns.json` -- the line-parsing rule set: regexes that
  extract a timestamp/severity/message from a raw line (`line_formats`, first
  match wins), regexes that classify a severity level and its display color
  (`level_rules`), and the regex used to spot `[Tag]` tokens (`tag_pattern`).
  Lands per-session at `res/bdg/desktop/tail/patterns.json` (see
  `modules/README.md`'s "Per-module embedded resources"); edit the source
  file and rebuild to add support for your own log format, or to change how
  an existing one is colorized. Ships with rules for `docker logs
  --timestamps`, classic syslog, Python's default `logging` format, ISO-8601
  timestamp + severity word, and a couple of bare `LEVEL: message` shapes,
  falling back to an unparsed catch-all so any line is still shown.

A filter set via the toolbar (or `-e`) applies prospectively only: it does
not retroactively hide/show already-received rows, mirroring how
`tail -f | grep pattern` only filters output going forward.
