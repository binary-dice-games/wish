# nymph

Charts from text. `nymph` turns a short text description of a chart into a
PNG drawn with wish's own `Plot` / `Plot3D` widgets, the way mermaid turns
text into diagrams. The text is stored inside the PNG, so the image is its
own source file: give it back to `nymph` to change it.

```sh
# no UI, no window, no display needed
wish standalone --renderer none --run=nymph -- render chart.nymph -o chart.png

# open it again later, edit with a live preview, save
wish standalone --run=nymph -- edit chart.png
```

- **server/**: the `Nymph` form (`register_nymph()`) and its parts — source
  splitter, CSV parser, format validation and data binding, the offscreen
  rasterizer, PNG metadata. See [DESIGN.md](DESIGN.md).
- **client/**: `run_nymph(wish_app_host&)`, self-registered as the `"nymph"`
  app. It only moves files between the local disk and the session sandbox.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_NYMPH=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).
Rendering needs `WISH_ENABLE_SDL3=ON` (the default); it uses SDL's software
renderer in memory and never opens a window.

## Commands

The arguments after `--`:

| Command | What it does |
|---|---|
| `render <in> [-o <out.png>]` | Source text (or a nymph PNG) to PNG. No UI. `<in>` may be `-` to read the source from standard input (then `-o` is required). |
| `edit <in> [-o <out.png>]` | Open the editing UI. A missing `<in>` starts from a small example. |
| `extract <in.png> [-o <out>]` | Write the source a nymph PNG carries to `<out>`, or to standard output. |

Without `-o`, the output is the input with its extension replaced by
`.png`; a PNG input is rewritten in place.

Exit code is 0 on success and 1 on failure. A failure prints one line to
standard error and writes nothing (an existing output file is left as it
was):

```
nymph: chart.nymph:15:11: unknown column 'revenu' (columns: month, revenue, cost)
```

`<line>:<column>` are positions in the source file.

`render` and `extract` also work through `wish client --run=nymph -- ...`
against a running wish server; `wish standalone --renderer none` is the
form that needs nothing else running.

## For AI agents

[AGENT_GUIDE.md](AGENT_GUIDE.md) is a self-contained instruction file to
load into an agent: the command, the whole format, every element and field,
recipes and common errors, with no other wish knowledge assumed. In short,
to put a chart in a markdown file or a reply:

1. Write the source to a file (format below).
2. Run `wish standalone --renderer none --run=nymph -- render FILE -o OUT.png`.
3. If the exit code is not 0, read the one-line error, fix that line, run again.
4. Reference `OUT.png` as an ordinary image.

To change an existing nymph chart, either `extract` its source, edit and
`render` again, or `render` the edited source over the same file name.

Use `scale: 2` under `image:` when the image will be shown large or on a
high-density screen; thin lines are noticeably cleaner.

## Source format

Three parts, separated by a line containing only `--`:

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
3, 11, 10
```

1. **Description** — plain text; stored in the PNG as its description. May
   be empty.
2. **Format** — YAML with `image` (optional) and `root` (required).
3. **Data** — CSV with a header row. May be empty when the format only uses
   literal arrays.

Only the first two `--` lines are separators.

### `image`

| Key | Default | Meaning |
|---|---|---|
| `width`, `height` | `800`, `500` | Image size in pixels (16–4096). |
| `scale` | `1` | 1–4. The PNG is `width*scale` x `height*scale`, same layout, sharper. |
| `theme` | `light` | `light`, `dark`, `classic` or `wish`. |
| `padding` | `8` | Pixels between the image edge and `root`. |

### `root`

One wish UI element, written exactly as in a wish YAML template: a `type`,
the type's fields, and `children`. Allowed types:

- `Plot` with any 2D series as children: `PlotLine`, `PlotScatter`,
  `PlotStairs`, `PlotStems`, `PlotShaded`, `PlotBars`, `PlotBarsH`,
  `PlotHistogram`, `PlotHistogram2D`, `PlotHeatmap`, `PlotPieChart`,
  `PlotDigital`, `PlotText`, `PlotInfLines`.
- `Plot3D` with any 3D series as children: `Plot3DLine`, `Plot3DScatter`,
  `Plot3DSurface`, `Plot3DTriangle`, `Plot3DQuad`, `Plot3DMesh`,
  `Plot3DText`.
- `VerticalLayout`, `HorizontalLayout`, `Spring`, `Label`, `Separator`, to
  combine several plots in one image.

Every field of those types works; they are listed in
[docs/ui-elements.md](../../../../docs/ui-elements.md) sections 5 and 6.
Flag fields take numbers or names (`flags: NoLegend|NoTitle`). A field the
type does not have, a type that is not in the list, or a series outside its
kind of plot is an error. A `Plot` / `Plot3D` used as `root` fills the image
unless it sets `height`.

### Column references

A value starting with `$` refers to a CSV column by header (`$revenue`,
`"$unit price"`) or by 1-based position (`$2`):

| Where | Value | Result |
|---|---|---|
| a list of numbers (`xs`, `ys`, `values`, ...) | `$col` | the column, one number per row |
| a list of numbers | `[$a, $b, $c]` | the columns interleaved by row — the row-major grid `PlotHeatmap.values` and `Plot3DSurface.zs` expect |
| a text field (`PlotPieChart.labels`) | `$col` | the cells joined by newlines |

Write `$$` for a literal `$` at the start of a text. Literal lists
(`ys: [1, 2.5, 4]`) also work.

### CSV rules

Comma-separated, header first, RFC 4180 quoting, blank lines ignored,
whitespace around unquoted cells trimmed. Every row needs as many cells as
the header. In a column used as numbers, an empty cell is a gap in the plot
and anything else that is not a number is an error. Columns the format does
not use are not checked, so text columns are fine.

## More examples

A histogram:

```
Distribution of response times.
--
root:
  type: Plot
  title: Response time
  x_label: ms
  children:
    - type: PlotHistogram
      label: latency
      values: $ms
      bins: 20
--
ms
118.2
131.0
96.4
```

A heatmap, one CSV column per grid column:

```
Activity by weekday and time of day.
--
image: {width: 520, height: 360}
root:
  type: Plot
  title: Activity
  x_flags: NoDecorations
  y_flags: NoDecorations
  children:
    - type: PlotHeatmap
      rows: 4
      cols: 5
      values: [$mon, $tue, $wed, $thu, $fri]
      scale_min: 0
      scale_max: 10
      format: "%.0f"
--
slot, mon, tue, wed, thu, fri
morning, 2, 4, 6, 3, 1
noon, 5, 7, 9, 6, 4
evening, 8, 9, 10, 7, 6
night, 1, 2, 3, 2, 5
```

Two plots in one image:

```
Temperature and humidity over a day.
--
image: {width: 640, height: 520}
root:
  type: VerticalLayout
  spacing: 6
  children:
    - type: Plot
      title: Temperature
      height: 240
      children:
        - {type: PlotLine, label: temp, xs: $hour, ys: $temp}
    - type: Plot
      title: Humidity
      height: 240
      x_label: hour
      children:
        - {type: PlotShaded, label: humidity, xs: $hour, ys: $hum}
--
hour, temp, hum
0, 12.8, 73
6, 14.9, 66
12, 23.2, 47
18, 21.0, 52
```

## Edit mode

`edit` opens three dockable windows:

- **Source** — Save, the file name (with `[MODIFIED]`), an error line, and
  one tab per part of the source. The Format tab has YAML highlighting and
  completion for type names, field names and flag values.
- **Preview** — the chart as live plot widgets, rebuilt as you type. Zoom,
  pan, legend toggles and the plot context menu all work.
- **Data** — the CSV as a table (the first 1000 rows).

While the source has an error, the error is shown and the last valid
preview stays. **Save** or Ctrl+S writes the PNG; a source with an error is
not saved. Closing with unsaved changes asks first.

## Things to know

- **The saved image comes from the text, not from the preview.** Zooming or
  hiding a series in the preview does not change what is saved. To change
  the image, change the text (for example `x_min` / `x_max`).
- **Metadata survives only exact copies.** Tools and services that
  re-encode images (many chat apps and image hosts) drop the embedded
  source. Keep the source text too when the image will go through one.
- **Colors and colormaps follow the theme.** The plot elements have no
  per-series color field yet.
- The saved image is drawn by SDL's software renderer, so its anti-aliasing
  differs from the on-screen preview. Layout and content are the same. One
  visible effect: a thin line that is exactly horizontal or vertical (a
  flat reference line) comes out pale, nearly grey, instead of in its
  series colour.
- There is no cursor-tracked field help window yet (the `editor` module has
  one); completion in the Format tab covers the same schema.
