# wish nymph Module — Implementation Plan

See [DESIGN.md](DESIGN.md) for the architecture this plan implements, and
the [editor](../editor/DESIGN.md) module for the form patterns edit mode
reuses (independent top-level windows, no-flicker reparse, save / close
flow).

Steps are ordered so the riskiest assumption is tested first and every step
leaves something that passes its own tests.

**Status: steps 1-11 done, with two recorded exceptions** — the Help window
in step 9 was not built, and step 11's no-SDL3 check was a compile of this
module's sources in a no-SDL3 configuration rather than a full build. Each
step below has a *Result* line saying what was done and where it departed
from the plan as first written.

All tests are GoogleTest files in `tests/`, registered with
`wish_add_optional_test(WISH_MODULE_BDG_DEV_NYMPH ...)`. Build with
`-DWISH_MODULE_BDG_DEV_NYMPH=ON` and at most `-j2`.

## Steps

### 1. Offscreen rasterizer spike

*Result: done.* SDL's software renderer works with no `SDL_Init`, no window
and no display, also inside another context's open frame. The class became
a free function, `render_figure()`. One fix came out of step 10: the
private context turns off ImGui's texture-based line anti-aliasing, which
the software renderer draws with gaps. `tests/test_nymph_render.cpp`.

**Goal:** A wish element tree containing a `Plot` renders to an RGBA buffer
with no window and no video subsystem, while another ImGui context is
mid-frame on the same thread.

**Deliverables:**
- `server/nymph_figure_renderer.hpp`/`.cpp` — `render_figure(const
  ui_element& root, const context&, image_options{width, height, scale,
  theme, padding}) -> image{width, height, rgba}`. Private ImGui /
  ImPlot / ImPlot3D contexts, private font atlas, `SDL_CreateSoftwareRenderer`
  on an RGBA32 surface, `ImGui_ImplSDLRenderer3`, three frames, context
  save / restore through a scope guard. A single `#if` guard returns a
  "built without SDL3" error when `WISH_ENABLE_SDL3` is off.
- `server/nymph.hpp`/`.cpp` — only an empty `register_nymph()` so the
  module links.
- `tests/test_nymph_render.cpp`, entry in `tests/CMakeLists.txt`.

**Tests:**
- Rendering a `Plot` with one `PlotLine` at 400x300 returns a 400x300
  buffer in which more than one distinct color appears inside the plot area
  and the corner pixel equals the light theme's window background.
- `scale: 2` returns 800x600.
- Rendering a `Plot3D` with one `Plot3DLine` returns a non-uniform buffer.
- Two renders of the same tree give identical bytes.
- With a headless ImGui context created by the test and `NewFrame()` called
  on it, `render()` returns, `ImGui::GetCurrentContext()` is unchanged, and
  the outer `EndFrame()` succeeds.
- The test passes with `SDL_VIDEODRIVER` unset and no display.

If the software renderer cannot do this, stop and update DESIGN.md section
7 to the fallback (a direct `ImDrawData` rasterizer behind the same
`figure_renderer` interface) before going on.

### 2. Document split / join

*Result: done.* `tests/test_nymph_document.cpp` (shared with step 3).

**Goal:** A source text splits into its three parts and joins back byte for
byte.

**Deliverables:**
- `server/nymph_document.hpp`/`.cpp` — `document`, `parse_document()`,
  `compose_document()`, `nymph::error{line, column, message}`, and the
  offset of each part's first line (for error positions).
- `tests/test_nymph_document.cpp` (pure, no wish dependencies).

**Tests:**
- The DESIGN.md example splits into the expected three strings.
- `compose(parse(t)) == t` for: LF and CRLF line endings, no trailing
  newline, empty description, empty data, separator lines with trailing
  spaces, a data part that itself contains a `--` line.
- Zero or one separator line is an error naming line 1.
- A text over 16 MiB is rejected.

### 3. CSV table

*Result: done.* Cell errors carry the row's source line and name the
column; the character column is always 1.

**Goal:** The data part becomes a table of named string columns with
per-column number conversion.

**Deliverables:**
- `server/nymph_csv.hpp`/`.cpp` — `table`, `parse_csv(text, first_line)`,
  `column_index(name_or_position)`, `numbers(column)`, `strings(column)`.
- `tests/test_nymph_csv.cpp` (pure).

**Tests:**
- `time, value` / `0, 12` / `1, 6` gives two columns, headers trimmed.
- Quoted cells with commas, doubled quotes and embedded newlines parse.
- Blank lines are skipped; a row with the wrong cell count is an error with
  that row's source line.
- `numbers()`: empty cell → NaN; `abc` → error with line and column; `1e3`,
  `-0.5`, ` 7 ` parse.
- `$2`-style positions resolve; `0` and out-of-range positions are errors.
- Empty text gives an empty table, not an error.

### 4. PNG metadata

*Result: done.* Decompression of an embedded source is bounded at 16 MiB.
`tests/test_nymph_png_meta.cpp`.

**Goal:** A source text survives being embedded in and read back from a
PNG.

**Deliverables:**
- `server/nymph_png_meta.hpp`/`.cpp` — `encode_png()`, `read_source()`.
- `tests/test_nymph_png_meta.cpp`.

**Tests:**
- `read_source(encode_png(img, t, d)) == t` for a 1 KiB and a 1 MiB text
  (the second exercises the compressed path), including non-ASCII UTF-8.
- The output decodes with `stb_image` to the same width, height and pixels.
- The `nymph` chunk is the first chunk after `IHDR`; a `Description` chunk
  holds `d`.
- `read_source` returns `nullopt` for a PNG without the chunk, and for
  bytes that are not a PNG; a chunk with a bad CRC is an error.

### 5. Binding

*Result: done, with two additions the plan did not foresee.* (1) wish's
JSON/YAML importer dropped arrays of non-integer numbers, so no descriptor
could carry plot data; fixed in `src/ui/ui_descriptor.cpp` /
`ui_importer.cpp` with tests in `tests/test_ui_importer.cpp`. (2) `bind()`
parses YAML with libyaml directly to get line and column for every node.
Beyond the listed tests it also rejects unknown fields, unknown flag names
and series outside their kind of plot. `tests/test_nymph_bind.cpp`.

**Goal:** A document becomes an image-options struct plus a literal-only
wish descriptor, or one positioned error.

**Deliverables:**
- `server/nymph_bind.hpp`/`.cpp` — `figure`, `bind(document)`, the type
  allow list, reference resolution per DESIGN.md section 3.
- `tests/test_nymph_bind.cpp` (links `wish_server`, calls `register_all()`).

**Tests:**
- The DESIGN.md example binds; `PlotLine.xs` holds the `month` column as
  floats.
- `[$a, $b]` on `PlotHeatmap.values` interleaves by row.
- `labels: $name` on `PlotPieChart` gives a newline-joined string.
- Errors, each with the right line: unknown column; reference on a
  non-array, non-string field; mixed list; `type: Image`; `type: Button`;
  missing `root`; `image.width: 5`; unknown key under `image`.
- `$$5` on a string field yields the literal `$5`.
- Every class registered from `plot_elements/` and `plot3d_elements/`
  passes the allow list (enumerated from the registry, not hand-listed).
- A document with literal arrays and an empty data part binds.
- The bound descriptor of the example renders through `figure_renderer`
  (step 1) without error.

### 6. UI mock

*Result: done after the fact.* `nymph_mock.yaml` was written once the
layout had been settled against the running form (step 10), and loads in
the `editor` tool with an empty error banner. It was not shown to the
maintainer before the form was written.

**Goal:** The edit-mode layout is agreed before form code is written.

**Deliverables:**
- `nymph_mock.yaml` — Source (toolbar + three-tab `TabBar` of
  `TextEditor`s), Preview (a sample `Plot`), Data (`Table`).

**Tests:**
- It loads in `wish client --run=editor -- nymph_mock.yaml` with an empty
  error banner; a screenshot taken through the automation module
  (`docs/automation.md`) shows them. Show it to the maintainer.

### 7. `Nymph` form, silent path

*Result: done.* `load` gained `validate: false` (used by `extract`).
`tests/test_nymph.cpp`.

**Goal:** `load` + `render` on the form produce a nymph PNG in the sandbox.

**Deliverables:**
- `server/nymph.hpp`/`.cpp` — the `Nymph` class with `{silent}`, `load`,
  `render`, `source`; the invisible root; the self-addressed
  `"render_requested"` event; `on_event()` running bind result →
  `figure_renderer` → `encode_png` → `out_<n>.png`; events `"rendered"`
  and `"render_failed"`.
- `tests/test_nymph.cpp` — `memory_transport` server fixture with
  `tests/session_event_recorder.hpp`, session id from `generate_id()`.

**Tests:**
- Upload the example, `load`, `render`: `"rendered"` arrives; the file it
  names is a PNG of the requested size whose `read_source()` equals the
  uploaded text.
- Upload that PNG as the input, `load`, `render`: the second PNG's pixels
  equal the first's.
- `load` of a bad source throws with a `line:column` message in silent
  mode.
- `load` of a plain PNG throws "not a nymph image".
- `render` before `load` throws.
- A path outside the sandbox (`../x`) is rejected.
- Two sessions rendering in turn do not affect each other's output.

### 8. Client runner and silent CLI

*Result: done.* Also added: with `--renderer none` the standalone log goes
to the temp directory instead of `./wish_logs`, so a render leaves only its
PNG behind. `tests/test_nymph_cli.cpp` runs the real `wish` binary.

**Goal:** One shell command turns a source file into a PNG without opening
a window, and fails with a non-zero exit code.

**Deliverables:**
- `client/nymph.hpp`/`.cpp` — `run_nymph()`, argument parsing for `render`
  / `extract` (and `edit`, wired in step 9), atomic local write, app
  registration with `app_param` entries.
- Approved core changes: `--renderer none` in
  `app/wish_cli/standalone/wish_standalone_app.cpp`;
  `wish_app_host::set_exit_code()` and its implementers; `docs/cli.md`
  rows for both.
- `tests/test_nymph_cli.cpp` (or an addition to `test_standalone.cpp`,
  whichever matches how that file drives the binary); temp file names
  include the gtest test name.

**Tests:**
- `wish standalone --renderer none --run=nymph -- render ex.nymph -o
  out.png` exits 0, writes a valid nymph PNG, creates no window (runs with
  no `DISPLAY`).
- Without `-o`, `ex.nymph` produces `ex.png`; a PNG input is rewritten in
  place.
- A source with an unknown column exits 1, prints
  `nymph: ex.nymph:<line>:<column>: ...` on stderr, and leaves no output
  file (and leaves an existing output file unchanged).
- `extract out.png` prints text equal to `ex.nymph`.
- A missing input file exits 1 with a message.
- `wish standalone --list` shows `bdg/dev/nymph`.

### 9. Edit mode

*Result: done except the Help window.* Three windows (Source, Preview,
Data) with a default dock layout. The Help window needs the editor
module's help-panel code moved into a shared component first; that was not
done (DESIGN.md "10. Implementation Status"). Completion in the Format tab
works. A lone `Plot` root gets `height: -1` so it fills the Preview window.

**Goal:** A nymph PNG opens in a UI, shows a live preview and the data,
and saves back with the edited source embedded.

**Deliverables:**
- `server/nymph.cpp` — the windows from the step 6 mock, per-part
  sandbox files, rebind on `"changed"`, preview rebuild with a stable
  window id, Data table rebuild (1000-row cap), banner, Save / Ctrl+S,
  `"on_image_saved"`, `mark_saved`, close confirmation via `MessageBox`,
  Help window (reusing the editor module's help-panel code; if it has to be
  shared, move it to `src/ui/` rather than copy it).
- `client/nymph.cpp` — `edit` command: `"on_image_saved"` → download →
  local write → `mark_saved()`; `"closed"` → `signal_done()`.
- Additions to `tests/test_nymph.cpp`.

**Tests:**
- After `load` in edit mode the session has the top-level windows and
  the preview subtree contains a `Plot` with the example's two series.
- Changing the Format file to invalid YAML sets the banner and leaves the
  preview subtree's ids unchanged; fixing it clears the banner.
- The preview window's `__wish_id` is the same before and after a valid
  edit.
- Changing a CSV value changes the matching cell in the Data table and the
  `ys` of the preview series.
- A 5000-row CSV yields 1000 table rows and a "Showing 1000 of 5000 rows"
  label.
- Save emits `"on_image_saved"`; that file's `read_source()` equals the
  three editors' text joined, and its pixels equal a silent render of that
  same text.
- `load` of a bad source in edit mode does not throw and sets the banner.
- Closing while modified opens the `MessageBox`; "no" emits `"closed"`;
  "yes" emits `"on_image_saved"` and, after `mark_saved`, `"closed"`.

### 10. Live verification

*Result: done* against `wish standalone --renderer web`, driven with
`AutomationClient`: typing a title change updated the preview; a broken
edit showed the error and kept the previous preview; Ctrl+S on the broken
source was refused; after undo, Ctrl+S wrote the PNG and cleared
`[MODIFIED]`; the saved PNG reopened with the edited text; closing with
changes opened the confirm dialog and "Yes" saved and ended the process;
completion offered `PlotScatter` / `PlotStairs` / `PlotStems` for `PlotS`.
Silent renders of a line+bars plot, histogram, heatmap, pie, 3D surface and
a two-plot layout were looked at. A render through a live
`wish server --renderer sdl3` gave the same bytes as `--renderer none`.
`docs/automation.md` was updated.

**Goal:** The tool works end to end when driven like a user and like an
agent.

**Deliverables:**
- Any fixes found. `docs/automation.md` updated with anything learned.

**Tests** (automation module against a running instance, per
`docs/automation.md`):
- Open the example in edit mode; the screenshot shows source, preview and
  data.
- Type a change to the plot title; the preview title changes.
- Introduce and fix a YAML error; the preview never blanks.
- Press Ctrl+S; reopen the saved PNG with `edit`; the edited text is there.
- Render one source per plot family (line, bars, histogram, heatmap, pie,
  3D surface, two plots in a `VerticalLayout`) in silent mode and look at
  each PNG.

### 11. Documentation and build wiring

*Result: done, with a narrower no-SDL3 check than planned.* The full
`-DWISH_ENABLE_SDL3=OFF` build was not run; instead the module's server
sources and `test_nymph_bind` were compiled in a no-SDL3 configuration.

**Goal:** The module is discoverable and documented.

**Deliverables:**
- `README.md` in this directory: commands, the source format reference,
  three worked examples, the metadata-stripping caveat, a short "for AI
  agents" section with the exact silent command.
- A row in `modules/bdg/dev/README.md`; `WISH_MODULE_BDG_DEV_NYMPH` in
  `docs/building.md` and in the `WISH_COLLECTION_BDG_DEV` row; a
  `CHANGELOG.md` entry under `## [Unreleased]` / `### Added` (plus entries
  for `--renderer none`).
- DESIGN.md status line and section 10 updated to what was built.

**Tests:**
- A clean configure with `-DWISH_COLLECTION_BDG_DEV=ON` builds and all
  `test_nymph*` tests pass under `ctest -j2`.
- A configure with `-DWISH_ENABLE_SDL3=OFF -DWISH_ENABLE_WEB=ON` builds.
- Every README example renders in silent mode.

## Completion Criteria

- [x] All `test_nymph*` tests pass, run in parallel (`ctest -j2`) three
      times in a row.
- [x] `render` of every README example exits 0 with no display available.
- [x] For every README example, `extract(render(text)) == text` and
      rendering the PNG again gives identical pixels.
- [x] Edit mode's saved PNG has the same pixels as a silent render of the
      same text.
- [x] A bad source in silent mode: exit code 1, one positioned message on
      stderr, no output file.
- [x] No source text can make the server read or write outside the session
      sandbox (allow list test and path test pass).
- [x] Step 10's live checks done and `docs/automation.md` updated.
- [x] README, collection README, `docs/building.md`, `docs/cli.md` and
      `CHANGELOG.md` updated; DESIGN.md matches the code.
