# wish nymph Module — Architecture & Design

**Status: implemented**, except the cursor-tracked Help window (see "10.
Implementation Status"). [PLAN.md](PLAN.md) records the steps and how each
was verified. Section 9 lists the changes made outside this directory.

## 1. Purpose / Scope

`nymph` turns a text description of a chart into a PNG image drawn with
wish's own `Plot` / `Plot3D` widgets — what mermaid does for diagrams, for
plots. The text that produced an image is stored inside the PNG as
metadata, so any nymph image can be given back to the tool as its input.

It has two modes, sharing one parser and one rasterizer:

- **Silent** (`render`): source in, PNG (or an error message and a
  non-zero exit code) out. No window, no display needed. This is the mode
  AI agents use to put a chart in a markdown file or a chat reply.
- **Edit** (`edit`): a wish UI in the shape of the [editor](../editor/DESIGN.md)
  module — the source text, a live interactive preview built from real plot
  widgets, and a table of the input data. Saving writes the PNG with the
  updated source embedded.

This directory owns:

- `server/nymph_document.hpp`/`.cpp` — split a source text into its three
  parts and join them back.
- `server/nymph_csv.hpp`/`.cpp` — CSV to a column table.
- `server/nymph_png_meta.hpp`/`.cpp` — read and write the PNG metadata
  chunks.
- `server/nymph_bind.hpp`/`.cpp` — validate the format YAML and replace
  column references with data.
- `server/nymph_figure_renderer.hpp`/`.cpp` — the offscreen rasterizer
  (`render_figure()`).
- `server/nymph.hpp`/`.cpp` — the `Nymph` form (`nymph_form`,
  `register_nymph()`).
- `client/nymph.hpp`/`.cpp` — `run_nymph(wish_app_host&)`: command line,
  local file I/O.
- `nymph_mock.yaml` — the edit-mode layout as a mock for the `editor` tool.
- `README.md` — usage and the source format reference.

It does **not** own any plot widget or plot attribute. A chart feature
wish's plot elements lack (per-series color, subplots, ...) is added to
`src/ui/plot_elements/` / `src/ui/plot3d_elements/`, never as a
nymph-private YAML key.

## 2. Design Goals

1. **Round trip.** `extract(render(text)) == text`, byte for byte. An image
   is its own source file.
2. **One rasterizer.** Silent mode and edit mode's Save produce the same
   pixels for the same source, because both run the same code at the same
   size. The interactive preview is a convenience, not the output.
3. **The format is wish's plot schema, not a second one.** Every registered
   plot class and every one of its fields works with no nymph code naming
   it. A plot field added to wish later works in nymph with no change here.
4. **Usable by an agent without a display.** Silent mode opens no window,
   needs no GPU or X/Wayland server, reports every input error as one
   `line:column: message` on stderr, and exits non-zero.
5. **Deterministic.** Same source and same build give the same PNG bytes:
   no clock, no mouse state, no animation in the rendered frame.
6. **Sandboxed like every other module.** The server form reads and writes
   only its session sandbox; the client runner owns local files. A source
   text can never make the server read a file.

## 3. Source Format

A source is UTF-8 text in three parts, separated by a line that is exactly
`--` (trailing whitespace ignored). Only the first two such lines are
separators, so the data part may contain `--` lines; the description may
not.

```
Monthly revenue against cost, 2025.
--
image:
  width: 800
  height: 450
root:
  type: Plot
  title: Revenue vs cost
  x_label: month
  y_label: kUSD
  children:
    - type: PlotLine
      label: revenue
      xs: $month
      ys: $revenue
    - type: PlotBars
      label: cost
      xs: $month
      ys: $cost
--
month, revenue, cost
1, 12, 9
2, 15, 9.5
```

| Part | Content | Used for |
|---|---|---|
| 1. Description | Plain text. May be empty. | PNG `Description` text chunk (alt text); shown in edit mode. Never parsed. |
| 2. Format | YAML with two top-level keys, `image` (optional) and `root` (required). | What to draw. |
| 3. Data | CSV. May be empty if the format uses only literal arrays. | The columns the format refers to. |

### `image`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `width` / `height` | int | `800` / `500` | Image size in pixels, 16–4096. |
| `scale` | int | `1` | Supersampling factor 1–4: the PNG is `width*scale` x `height*scale` with fonts and line widths scaled to match. |
| `theme` | string | `light` | A wish theme: `light`, `dark`, `classic`, `wish`. |
| `padding` | int | `8` | Pixels between the image edge and `root`. |

### `root`

One wish UI descriptor, in exactly the form `import_yaml()` accepts
(`type`, the class's fields, `children`). Allowed `type` values:

- every class registered from `src/ui/plot_elements/` and
  `src/ui/plot3d_elements/` (`Plot`, `PlotLine`, ..., `Plot3D`,
  `Plot3DSurface`, ...);
- `VerticalLayout`, `HorizontalLayout`, `Spring`, `Label`, `Separator`, to
  put several plots in one image.

Any other type is an error (see "7. Design Decisions"), as is a field the
class does not have, and a series outside its kind of plot (a `PlotLine`
must be a direct child of a `Plot`, a `Plot3DLine` of a `Plot3D`: the
ImPlot item calls are only valid inside their plot). Integer fields with
named values take the names (`flags: NoLegend|NoTitle`). When `root` is a
`Plot` / `Plot3D` with no `height`, it gets `height: -1`, which ImPlot reads
as "all the space left": the whole image when rendered, the whole Preview
window in edit mode.

### Column references

A string value that starts with `$` is a reference to a data column, by
header name (`$revenue`, `"$unit price"`) or by 1-based position (`$2`).
`$$` at the start of a string is a literal `$`. References are resolved by
the registered type of the field they are assigned to:

| Field type | Value | Result |
|---|---|---|
| `float[]` / `int32[]` | `$col` | The column's numbers, one per data row. |
| `float[]` / `int32[]` | `[$a, $b, $c]` | The columns interleaved by row (row 0 of a, b, c, then row 1, ...): the row-major grid `PlotHeatmap.values` and `Plot3DSurface.zs` expect. |
| `string` | `$col` | The column's cells joined with `\n` (the form `PlotPieChart.labels` expects). |
| any other | `$col` | Error. |

A list mixing references and numbers is an error. Literal arrays
(`ys: [1, 2, 3]`) stay valid and need no data part.

### CSV rules

Comma-separated, first row is the header, RFC 4180 quoting. Whitespace
around an unquoted cell is trimmed. Blank lines are skipped. Every row must
have as many cells as the header. In a column used as numbers, an empty
cell is NaN (ImPlot draws a gap) and any other non-number is an error
naming the line and column. Columns nothing refers to are never checked.

## 4. Key Abstractions

### `nymph::document` (`nymph_document`)

`{description, format, data}` as three strings. `parse_document(text)` and
`compose_document(doc)` are exact inverses for any text with at least two
separator lines; a text with fewer is an error. No YAML or CSV knowledge.

### `nymph::table` (`nymph_csv`)

Header names plus cells kept as strings, with the source line of each row.
Number conversion is done per column, on demand, by `nymph_bind` — the
table itself never decides a column's type.

### `nymph::figure` (`nymph_bind`)

`bind(document) -> figure`: the `image` options, the parsed `table`, and a
`bison::dynamic` descriptor of `root` (the shape `build_ui_node()` takes)
with every reference replaced by a literal array or string. It parses the
YAML with libyaml's document API, walks the tree, checks each `type`
against the allow list and its place, and reads each field's registered
type off a fresh instance of the class (`dynamic::create_instance`); named
enum/flag values are checked against `find_ui_element_class()`
(`src/ui/ui_schema_help.hpp`). Throws `nymph::error{line, column, message}`
(positions are in the whole source text, not the part). `bind` is the only
place a source can be rejected for its content, and both modes call it.

### `nymph::render_figure()` (`nymph_figure_renderer`)

`render_figure(const ui_element& root, const context&, image_options) ->
image{width, height, rgba}`. For the duration of one call it owns a private
Dear ImGui / ImPlot / ImPlot3D context trio (with its own font atlas), an
`imgui_renderer` used only for its `render_node()` dispatch, and an SDL
software renderer drawing into an in-memory surface. `scale` is applied as
`DisplayFramebufferScale` plus `SDL_SetRenderScale()` on the software
renderer (the backend leaves vertices in logical units and expects the SDL
renderer to carry the pixel scale), so the layout is identical at every
scale and only the pixel density changes. Invariants:

- It makes its contexts current on entry and restores whatever was current
  on exit, including on exception. It may therefore be called while another
  ImGui frame is open on the same thread.
- It must only be called on the server's render thread (see "5. Data
  Flow") — ImGui's current-context pointer is a process global.
- It creates no window, never initializes SDL's video subsystem, reads no
  input, and uses a fixed `DeltaTime`.
- It renders three frames and keeps the third: ImPlot auto-fits axes one
  frame late, and wish's measure/arrange pass settles one frame late.

### `nymph::png_meta` (`nymph_png_meta`)

`encode_png(image, source, description) -> bytes` and
`read_source(bytes) -> optional<string>`. The source is stored in an
`iTXt` chunk with keyword `nymph`, placed directly after `IHDR`:
uncompressed up to 64 KiB (so `strings` and a plain PNG chunk reader can
see it), zlib-compressed above that (miniz). The description goes in a
second `iTXt` chunk with the standard keyword `Description`. `read_source`
checks the PNG signature and each chunk's CRC and never decodes pixels.

### `nymph_form` (`server/nymph`)

The `Nymph` RMI class. In both modes its only auto-registered root is an
invisible `Window` (`visible: false`), purely so the form has an event
handler entry. Silent mode keeps the loaded text verbatim plus the `figure`
bound at `load`. Edit mode keeps a dirty flag and the UI subtrees described
in "5. Data Flow", built on the first `load`; it re-reads the three editor
files and binds again for every preview rebuild and every save.

## 5. Data Flow / Architecture

```
            local file                                    local file
  (.nymph text or nymph .png)                               (.png)
            │ client: upload                         client: download ▲
            ▼                                                         │
  sandbox input ── load ──► document ── bind ──► figure ── render ──► image
                 (png_meta   (3 parts)  (csv +    (bound     (figure_   │
                  if PNG)                registry) descriptor) renderer) │
                                           │                            ▼
                              edit mode:   └─► live preview      png_meta.encode_png
                                               (import + instantiate)   │
                                                                 sandbox output
```

**Reaching the render thread.** RMI methods run on a dispatch thread;
`render_figure()` must run on the render thread. `Nymph.render` therefore
does not render: it appends a self-addressed event (`"render_requested"`,
`root_key` = the form's own root) to `context::pending_events`. The render
loop delivers it to `nymph::on_event()` on the render thread, after that
session's render pass, with no session lock held — the same path a button
click takes. Edit mode's Save button and Ctrl+S reach `on_event()`
directly. Every render in both modes thus happens in `nymph::on_event()`.

**Silent** (`wish standalone --renderer none --run=nymph -- render in.nymph -o out.png`):

1. Client reads the local input, uploads it as `in_0`, instantiates `Nymph`
   with `{silent: true}`, calls `load({path})`, then `render({})`.
2. `load`: if the bytes start with the PNG signature, `read_source()` (no
   chunk → error "not a nymph image"); then `parse_document()` + `bind()`.
   Failure throws; the client prints the message and exits 1.
3. `render` → `on_event`: `build_ui_node()` on the bound descriptor,
   `render_figure()`, `encode_png()`, write `nymph_out_<n>.png` to the
   sandbox, emit `"rendered" {path}` — or `"render_failed" {message}`.
4. Client downloads, writes the local file through a temporary file and a
   rename, exits 0.

**Edit** (`wish client --run=nymph -- edit chart.png`):

1. As silent step 1 with `{silent: false}`; `load` failure is shown in the
   banner instead of ending the run, so a broken source can be fixed.
2. The form builds three dockable windows, each its own top-level root
   registered the way the editor's Help window is, arranged by a default
   `DockLayout` (Source left, Preview over Data right) inside one embedded
   `DockSpaceViewport` tile:
   - **Source** — a toolbar (Save, file name with `[MODIFIED]`, error
     banner) over a `TabBar` with three file-backed `TextEditor`s:
     Description (plain), Format (`language: yaml`, `wish_ui_schema: true`),
     Data (plain). Each edits its own sandbox file.
   - **Preview** — the bound `root`, instantiated as live widgets. ImPlot's
     own zoom, pan, legend toggles and context menus work here.
   - **Data** — a `Table` of the parsed CSV: header row plus up to the
     first 1000 data rows, with a "Showing N of M rows" label.
3. A `"changed"` event from any of the three editors re-reads the files,
   rebuilds the `document`, and calls `bind()`. On failure the banner shows
   the error — located within its own editor ("Format, line 11: ..."), not
   within the whole source — and the previous preview stays (editor's
   no-flicker rule). On success the preview is rebuilt, reusing the preview
   window's `__wish_id` so its position and dock state survive. The Data
   table is rebuilt whenever the data part changed and parses as CSV, even
   while the format part does not bind.
4. Save (the button, or Ctrl+S in an editor): `on_event` composes the three
   editors' current text into a source, runs silent step 3 on it, and emits
   `"on_image_saved" {path}`. The client downloads, writes the local PNG,
   calls `mark_saved()`. A source that does not bind is not saved: the
   banner says why and `"render_failed"` is emitted.
5. Close with unsaved changes: a `MessageBox` (yes / no / cancel), exactly
   the editor module's flow.

## 6. Public API Contract

### Command line

`run_nymph` reads the arguments after `--`. Under `wish standalone` and
`wish client` alike:

| Command | Behavior |
|---|---|
| `render <in> [-o <out.png>]` | Silent mode. `<in>` is a source text or a nymph PNG; `-` reads the source from console input. Default output: `<in>` with the extension replaced by `.png` (a PNG input is rewritten in place). `-` as input requires `-o`. |
| `edit <in> [-o <out.png>]` | Edit mode, same input and output rules. A missing `<in>` starts from a small template. |
| `extract <in.png> [-o <out>]` | Write the embedded source to `<out>`, or to stdout. |

Exit code 0 on success; 1 with `nymph: <file>:<line>:<column>: <message>`
on stderr for a bad source, `nymph: <file>: <message>` for anything else
(missing file, not a nymph image, built without SDL3), and the usage text
for bad arguments. Nothing is written to the output path on failure: the
client writes through a temporary file and a rename.

### `Nymph` RMI class

| Symbol | Contract |
|---|---|
| constructor `{silent: bool}` | `silent: true` builds no visible UI. Default `false`. |
| `load({path, display_path?, validate?})` | `path` sandbox-relative, text or PNG. Replaces the current document. Silent: throws on any error; `validate: false` skips parsing and binding (for `extract`, so a source that no longer renders can still be read back). Edit: throws only for an unusable `path` — a bad source is shown in the banner. Clears the dirty flag. |
| `render({})` | Queues a render of the current document. Returns at once; the result is the `"rendered"` / `"render_failed"` event. Throws if nothing is loaded. |
| `source({})` | Returns `{path}` of a sandbox file holding the current composed source (used by `extract`). |
| `mark_saved({})` | Client confirms the last `"on_image_saved"` file reached local disk. Clears dirty; completes a pending close. |
| `"rendered"` `{path}` | A finished PNG is in the sandbox at `path`. |
| `"render_failed"` `{message}` | The render did not produce an image. |
| `"on_image_saved"` `{path}` | Edit mode: Save produced `path`; download it, write it, call `mark_saved()`. |
| `"closed"` | The form is gone; the client calls `signal_done()`. |

## 7. Design Decisions

- **The format YAML is a wish UI descriptor plus one extension (`$column`),
  not a chart vocabulary of its own** (e.g. `lines2d: {x: time, y: value}`).
  A private vocabulary would need a mapping entry per plot class and per
  attribute, would lag behind wish's plot elements, and could not reuse the
  editor module's schema autocomplete and field help. Reusing the
  descriptor meets Goal 3 with one generic tree walk. The cost is
  verbosity (`type: PlotLine`, `children:`), which the README's examples
  and edit mode's autocomplete offset.

- **References are explicit (`$name`), not inferred from "a string where an
  array was expected".** One rule covers array and string fields, a typo in
  a column name is reported as an unknown column rather than as a type
  error, and a real string that happens to match a header is never
  silently replaced.

- **An allow list of element types, not "anything `import_yaml()`
  accepts".** `Image.src` and `TextEditor.file_path` take file paths; a
  source text is untrusted input (an agent may be rendering text it was
  handed), and an image tool has no use for interactive widgets. The list
  is derived from the registry for the two plot directories (so new plot
  classes are allowed automatically) plus a fixed handful of layout
  classes.

- **A private offscreen renderer, not a screenshot of the preview.** A
  screenshot has the window's size, depends on what overlaps it, exists
  only under the SDL3 renderer, and cannot exist at all in silent mode.
  The private renderer gives a fixed-size, reproducible image in both
  modes and under either live renderer (`sdl3` or `web`).

- **The format YAML is parsed with libyaml directly, not through
  `import_descriptor_yaml()`.** The generic `bison::dynamic` tree that
  importer returns cannot carry a node's line and column, and Goal 4 needs
  both in every error. `bind()` therefore builds the same descriptor shape
  itself (`__type__`, `__name__`, `order`, `children`), and hands it to the
  shared `build_ui_node()`. Scalar typing follows the importer's rules
  (plain `true`/`yes`/`on`, integers, floats), with one deliberate
  difference: a value assigned to a text field stays text (`title: 2025`).

- **Unknown fields and unknown flag names are errors, although the wish
  importer silently ignores both.** A typo in a template is a cosmetic bug;
  in a tool an agent drives blind, a silently ignored `titel:` produces a
  wrong image with exit code 0.

- **Lines are anti-aliased by geometry, not by ImGui's line texture**
  (`AntiAliasedLinesUseTex = false` in the private context). The texture
  method depends on the bilinear filtering a GPU does; SDL's software
  renderer samples it unevenly, which showed as gridlines with gaps and
  plot borders of uneven width in the first renders.

- **SDL's software renderer on an in-memory surface, driven by the existing
  `ImGui_ImplSDLRenderer3` backend.** `SDL_CreateSoftwareRenderer()` needs
  no window and no video subsystem, and this backend with the software
  driver is already the project's documented headless recipe
  (`SDL_RENDER_DRIVER=software`), so its output quality is known. Rejected:
  (a) a hand-written `ImDrawData` triangle rasterizer — no SDL dependency,
  but a few hundred lines of new, subtle code (anti-aliased fringes, font
  sampling) to get what SDL already does; (b) extending
  `sq_chart_render` — it redraws charts with its own primitives and would
  never match the widgets (Goal 2, Goal 3); (c) a hidden second
  `sdl3_renderer` — SDL's video driver is process-global, so it cannot
  coexist cleanly with a live windowed renderer. Plan step 1 confirmed it
  works: no `SDL_Init`, no window, and the same bytes whether called under
  `null_renderer`, inside the web renderer's frame, or inside
  `sdl3_renderer`'s. (a) remains the fallback behind `render_figure()` if
  that ever stops holding. Consequence: nymph renders only in builds with
  `WISH_ENABLE_SDL3=ON`; without it the module still builds and `render`
  fails with a clear message.

- **Rendering happens in `on_event()`, reached through a self-addressed
  pending event.** It is the one place a form already runs on the render
  thread without a lock, so no new "run on the render thread" hook is
  needed in the server, and silent and edit mode share one code path. It
  runs between the session's render pass and `end_frame()`, i.e. inside the
  live renderer's open ImGui frame — hence `render_figure()`'s
  save/restore invariant. A render blocks the render loop for its duration
  (tens of milliseconds for typical charts), which is acceptable for an
  explicit Save.

- **Silent mode runs under `wish standalone --renderer none`**, a new
  renderer choice mapping to `null_renderer` (section 9). Without it,
  standalone always opens an SDL window or starts the web server, which
  violates Goal 4. The render loop still runs with `null_renderer`, so
  pending events are still delivered. Silent mode also works through
  `wish client` against a running server; that is slower to reach and not
  the documented path for agents.

- **Three text editors, one per part, not one editor holding the whole
  source.** The schema scanner behind autocomplete and help
  (`scan_cursor_context_yaml()`) reads from the start of the buffer, so it
  only works on pure YAML; a YAML highlighter over CSV rows is noise; and
  an edit to one part should not re-tokenize megabytes of data. Because
  `compose_document()` is the exact inverse of `parse_document()`, the
  embedded text is still the single source of truth (Goal 1).

- **Interactive changes in the preview are not written back to the
  source.** Plot elements emit no events, so the form cannot see a zoom or
  a hidden series. Save always renders from the text. The preview's
  interaction is for inspecting the data; a persistent change is a text
  edit (`x_min`/`x_max`, removing a child). Writing view state back needs
  new plot events in wish core and is left out of this design.

- **The source goes in an `iTXt` chunk, uncompressed when small.** `iTXt`
  is the PNG text chunk defined as UTF-8 and has a standard compression
  flag, so no private chunk type is needed and generic tools
  (`exiftool`, `pngcheck`) show it. Placing it right after `IHDR` lets
  `read_source()` stop early. Rejected: appending the text after `IEND`
  (many tools truncate trailing data) and steganography in pixels (lost on
  any re-encode, and unreadable by other tools).

- **PNG encoding stays with `stb_image_write` plus a chunk splice**, as
  `sdl3_renderer` and sq already encode PNGs. stb cannot write text chunks,
  so `encode_png()` inserts them into stb's output; CRC-32 comes from
  miniz.

- **Parsing and PNG handling live in `server/`, not `client/`.** The client
  only moves bytes, as in every other module, so `wish client` against a
  remote server, the C ABI and the Python binding all get the full tool.
  `extract` goes through the server too, for the same reason.

## 8. Constraints and Invariants

- `compose_document(parse_document(t)) == t` for every accepted `t`, and
  `read_source(encode_png(img, t, d)) == t`. Line endings and trailing
  whitespace are preserved, never normalized.
- `bind()` is pure: no file access, no session state, no ImGui. It is the
  only validator, and the preview and the PNG are both built from its
  result.
- `render_figure()` is called only from `nymph_form::on_event()` (and,
  through the close dialog's callback, from the `MessageBox`'s own
  `on_event()`), i.e. always on the render thread. It leaves the calling
  thread's current ImGui, ImPlot and ImPlot3D contexts exactly as it found
  them.
- The rendered frame depends on nothing but the `figure`: no wall clock, no
  mouse position (kept at `-FLT_MAX`), `IniFilename` null.
- Limits, checked before any allocation they bound: source text 16 MiB;
  image `width*scale` and `height*scale` at most 8192; CSV at most 1,000,000
  data rows. Exceeding one is an ordinary `nymph::error`.
- The server form touches only sandbox paths, each through
  `file_service::resolve_path()`. Output names are generated
  (`nymph_out_<n>.png`), never taken from the source text.
- Preview and Data elements get a `__wish_id` but are not put in
  `ctx().objects`: they are rebuilt on every edit, and nothing addresses
  them over RMI.
- Session data is reached through `with_session()`-style access, as in the
  editor form: `on_event()` runs without the session lock, RMI methods run
  with it.
- A failed Save or render leaves the previous sandbox output and the local
  file untouched.
- Metadata survives only byte-exact copies. Services that re-encode images
  (many chat and image hosts) drop it; the README must say so and point at
  keeping the `.nymph` text next to the image when that matters.

## 9. Integration Boundaries

Depends on:

- `wish::form`, `ui_root::on_event`, `context::pending_events` — form
  lifecycle and the render-thread hop.
- `import_yaml()` / `build_ui_node()` (`src/ui/ui_importer.hpp`) — turning
  the bound descriptor into elements, for both the preview and the
  offscreen render.
- `ui_schema_help` — field types for `bind()`, autocomplete and help for
  the Format editor.
- `imgui_renderer::render_node()`, `find_theme()`, ImPlot, ImPlot3D.
- SDL3 (`SDL_CreateSoftwareRenderer`) and `imgui_impl_sdlrenderer3`;
  `stb_image_write`; miniz (CRC-32, zlib); libyaml.
- `TextEditor`, `Table`, `TabBar`, `MessageBox`, `dock::` layout helpers,
  `wish_app_host`.

Changes made outside this directory:

| Where | Change | Why |
|---|---|---|
| `app/wish_cli/standalone/` | `--renderer none` → `null_renderer`; with it, the log goes to the temp directory, not `./wish_logs` | Silent mode without a window or web server, leaving nothing in the caller's directory. |
| `src/client/wish_app_host.hpp`, `wish_client_app`, `wish_standalone_session` | `set_exit_code(int)` (default: no-op, which the C ABI host keeps), returned from `on_session()` | A failed `render` must exit non-zero; an app could only `signal_done()`. |
| `src/ui/ui_descriptor.cpp`, `src/ui/ui_importer.cpp` | Arrays of non-integer numbers are imported, and integer arrays widen for `float[]` fields | Found while building this: the importer dropped float arrays, so no JSON/YAML descriptor could carry plot data. `build_ui_node()` needs it for the bound descriptor. |
| `src/imgui/imgui_renderer.hpp`/`.cpp` | `find_theme(name)` | Styling a private ImGui context with a registered theme; the registry was file-local. |
| root `CMakeLists.txt` | libyaml include/link for `wish_server` when this module is on | `bind()` includes `<yaml.h>`. |
| `tests/CMakeLists.txt`, `docs/building.md`, `docs/cli.md`, `modules/bdg/dev/README.md`, `CHANGELOG.md` | Module rows / test entries | Standard for a new module. |

Depended on by: nothing. `sq`'s Save PNG could later call
`render_figure()` instead of its own rasterizer; that is not part of this
work.

## 10. Implementation Status

**Implemented and verified** (unit tests, plus a live run through the
automation module — see PLAN.md): `render`, `extract` and `edit`; all
registered plot classes; column references; PNG metadata round trip;
identical output from silent mode and edit mode's Save, and from
`null_renderer`, web and SDL3 hosts.

**Not implemented:**

1. **Help window.** The plan called for the editor module's cursor-tracked
   field help next to the Format tab. That code lives inside the `editor`
   form; reusing it means first moving it into a shared component under
   `src/ui/`, which was left out to keep this change from rewriting a
   working module. Schema completion in the Format tab (type names, field
   names, flag values) is on and covers the same registry.
2. *(Done since.)* Per-series colour and styling: the plot elements gained
   `color`, `fill_color`, `line_weight`, `fill_alpha`, `marker`,
   `marker_size` and `colormap` (`src/ui/plot_elements/plot_style_fields.hpp`),
   and nymph picked them up with one addition of its own: `bind()` checks
   that a colour field holds `#RRGGBB[AA]`, and reports an empty unquoted
   one (`color: #C0392B` is a YAML comment) instead of treating it as
   automatic.
3. **Writing preview interaction back to the source** (see "7. Design
   Decisions").

**Known limits:** the saved image's anti-aliasing is the software
renderer's, different from the on-screen preview. A one-pixel series line
that runs exactly along a pixel row (a horizontal reference line) has no
solid core after anti-aliasing and comes out pale, close to grey, at any
`scale`; sloped lines keep their colour. `line_weight: 2` on the series
fixes it. Turning line anti-aliasing off is worse (the software renderer
then drops axis-aligned one-pixel lines, gridlines included);
text uses ImGui's default font at 16 px,
the same default the live renderers use; duplicate keys in the format YAML
are not reported (the last one wins).
