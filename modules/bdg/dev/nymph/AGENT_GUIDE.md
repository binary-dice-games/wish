# nymph: making charts from text

Instructions for an AI agent. Everything needed to use the tool is in this
file.

## What nymph does

`nymph` turns a small text file into a PNG chart. You write the text, run
one command, and get an image you can reference from markdown or show to a
user. The text is stored inside the PNG, so the image can later be given
back to the tool to change it.

Use it whenever a chart would explain something better than a table or a
sentence: trends over time, comparisons, distributions, proportions.

## The command

nymph is part of the `wish` program. If `wish` is not on `PATH`, use the
full path to the binary (in a wish source checkout: `build/app/wish`).

```sh
wish standalone --renderer none --run=nymph -- render chart.nymph -o chart.png
```

Type the part before `render` exactly as shown. It runs without a window
and without a display, and takes well under a second.

| Command | Result |
|---|---|
| `render IN -o OUT.png` | Make a PNG from a source file. `IN` may also be a PNG made by nymph. |
| `render IN` | Same, writing `IN` with its extension replaced by `.png`. |
| `render - -o OUT.png` | Read the source from standard input. |
| `extract IN.png` | Print the source stored in a nymph PNG. Add `-o FILE` to write it to a file. |

There are also two commands that open a window for a person, `view IN`
(look at the chart and its data, read-only) and `edit IN` (change it with a
live preview). They need `--renderer sdl3` or `--renderer web` in place of
`--renderer none`, and they block until the window is closed, so do not run
them unless the user asks to see or edit a chart interactively.

**Exit code 0** means the PNG was written. **Exit code 1** means it was
not: one line on standard error says why, and no file is created or
changed. Always check the exit code.

```
nymph: chart.nymph:15:11: unknown column 'revenu' (columns: month, revenue, cost)
```

The numbers are the line and column in your source file. Fix that line and
run the command again.

## Workflow

1. Write the source file (format below). Use the extension `.nymph`.
2. Run `render`.
3. If the exit code is 1, read the error line, fix the source, run again.
4. Look at the PNG if you can view images, and adjust the source if the
   chart is not clear.
5. Reference the PNG as a normal image, for example `![Revenue by month](chart.png)`.

To change an existing nymph image when you no longer have its source, run
`extract` on the PNG, edit the text, and `render` again.

## Source format

A source file has three parts, separated by a line that contains only `--`.

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

| Part | Content |
|---|---|
| 1. Description | Plain text saying what the chart shows. It becomes the image's description. May be empty, but the `--` line after it is still required. |
| 2. Format | YAML describing the chart. Two top-level keys: `image` (optional) and `root` (required). |
| 3. Data | CSV with a header row. May be empty if the format part contains all its numbers itself. |

Rules that cause most errors:

- There must be exactly two separator lines before the data. A separator is
  `--` alone on its line, with no indentation.
- YAML indentation is spaces, never tabs.
- Every `type`, field name and flag name must be spelled exactly as listed
  in this file. Unknown names are errors, not ignored.
- Series (`PlotLine`, `PlotBars`, ...) must be inside the `children` of a
  `Plot`. 3D series must be inside the `children` of a `Plot3D`.

### `image`

| Key | Default | Meaning |
|---|---|---|
| `width` | `800` | Width in pixels, 16 to 4096. |
| `height` | `500` | Height in pixels, 16 to 4096. |
| `scale` | `1` | 1 to 4. The PNG has `width*scale` by `height*scale` pixels with the same layout, only sharper. Use `2` for images that will be shown large. |
| `theme` | `light` | `light`, `dark`, `classic` or `wish`. |
| `padding` | `8` | Empty pixels around the chart. |

### `root`

`root` is one element. An element is a YAML mapping with a `type`, that
type's fields, and for containers a `children` list of more elements.

For one chart, `root` is a `Plot` (2D) or a `Plot3D`. It fills the image.

For several charts in one image, `root` is a `VerticalLayout` or
`HorizontalLayout` whose children are the plots. See "Several charts".

### Using the data: `$column`

A value that starts with `$` means "this column of the CSV".

| Written as | Meaning |
|---|---|
| `ys: $revenue` | The column whose header is `revenue`, one number per row. |
| `ys: "$unit price"` | A header with spaces: put the value in quotes. |
| `ys: $2` | The second column, counting from 1. |
| `values: [$a, $b, $c]` | Several columns interleaved row by row: row 1 of a, b, c, then row 2, and so on. This is how a grid is given to `PlotHeatmap` and `Plot3DSurface`. |
| `labels: $name` | On a text field: the column's cells, one per line. Used for pie slice names. |
| `ys: [1, 2.5, 4]` | Numbers written directly, no CSV needed. |

A list is either all numbers or all `$` references, never a mix. To start
a text with a real dollar sign, write `$$` (`title: $$5 per unit`).

### CSV rules

- First row is the header. Header names must be unique and not empty.
- Comma-separated. Spaces around values are ignored.
- A value that contains a comma, a quote or a line break goes in double
  quotes; a quote inside it is written twice (`"said ""hi"""`).
- Every row has the same number of values as the header.
- An empty value in a numeric column leaves a gap in the chart.
- Columns you do not reference are not checked, so text columns are fine.

## Element reference

Every series has a `label` field: its name in the legend. Every series
also takes the style fields in "Colours and style" below.

### `Plot` (2D chart container)

| Field | Default | Meaning |
|---|---|---|
| `title` | none | Title above the plot. |
| `x_label`, `y_label` | none | Axis labels. |
| `x_min`, `x_max` | automatic | Fixed X range. Both must be set and different to take effect. |
| `y_min`, `y_max` | automatic | Fixed Y range, same rule. |
| `width` | `-1` | Width in pixels; `-1` fills the available width. |
| `height` | fills the image when the plot is `root`; otherwise `300` | Height in pixels. |
| `flags` | none | Any of `NoTitle`, `NoLegend`, `NoFrame`, `Equal` (same scale on both axes), `Crosshairs`, `CanvasOnly`. |
| `x_flags`, `y_flags` | none | Per axis: `NoLabel`, `NoGridLines`, `NoTickMarks`, `NoTickLabels`, `NoDecorations` (all four), `Invert`, `Opposite` (axis on the other side), `AutoFit`. |
| `legend_location` | top left | `Center`, `North`, `South`, `West`, `East`, `NorthWest`, `NorthEast`, `SouthWest`, `SouthEast`. |
| `legend_flags` | none | `Outside` (legend outside the plot area), `Horizontal`, `Sort`, `Reverse`. |
| `colormap` | `Deep` | The set of colours series take in turn, and the scale of heatmaps. See "Colours and style". |

Several flags are joined with `|`: `flags: NoLegend|NoTitle`.

### 2D series (children of a `Plot`)

| Type | Fields besides `label` | Draws |
|---|---|---|
| `PlotLine` | `xs`, `ys` | A connected line. |
| `PlotScatter` | `xs`, `ys` | Markers, not connected. |
| `PlotStairs` | `xs`, `ys` | A step line. |
| `PlotStems` | `xs`, `ys`, `ref` (baseline, default 0) | A vertical stem from the baseline to each point. |
| `PlotShaded` | `xs`, `ys`, and either `ref` (default 0) or `ys2` | A filled area between `ys` and the baseline, or between `ys` and `ys2`. |
| `PlotBars` | `xs` (positions; omit for 0, 1, 2, ...), `ys` (heights), `bar_size` (width, default 0.67) | Vertical bars. |
| `PlotBarsH` | `xs` (lengths), `ys` (positions), `bar_size` | Horizontal bars. |
| `PlotHistogram` | `values`, `bins` (a count, or `-1` for automatic), `cumulative`, `density` (true/false), `range_min`, `range_max` | A histogram of the raw samples in `values`. |
| `PlotHistogram2D` | `xs`, `ys`, `x_bins`, `y_bins` | A 2D frequency map of sample pairs. |
| `PlotHeatmap` | `values` (grid, row by row), `rows`, `cols`, `scale_min`, `scale_max` (default 0 and 1), `format` (cell label format, default `"%.1f"`; `""` for none) | A coloured grid. |
| `PlotPieChart` | `labels`, `values`, `x`, `y` (centre, default 0.5), `radius` (default 0.4), `normalize` (true/false), `label_fmt` (default `"%.1f%%"`), `angle0` (start angle in degrees, default 90) | A pie chart. |
| `PlotDigital` | `xs`, `ys` (0 or 1) | A digital signal trace. |
| `PlotInfLines` | `values`, `horizontal` (true/false, default false) | Reference lines across the whole plot: vertical at each X value, or horizontal at each Y value. |
| `PlotText` | `text`, `x`, `y`, `offset_x`, `offset_y` (pixels) | A text label at a plot coordinate. |

`xs` and `ys` should have the same length. If they differ, the extra
values are ignored.

### `Plot3D` and 3D series

`Plot3D` has `title`, `x_label`, `y_label`, `z_label`, `width`, `height`,
`colormap`, `flags` (`NoTitle`, `NoLegend`, `NoClip`, `Equal`, `CanvasOnly`) and
`x_flags`, `y_flags`, `z_flags` (`NoLabel`, `NoGridLines`, `NoTickMarks`,
`NoTickLabels`, `NoDecorations`, `Invert`, `AutoFit`).

| Type | Fields besides `label` | Draws |
|---|---|---|
| `Plot3DLine` | `xs`, `ys`, `zs` | A connected line through 3D points. |
| `Plot3DScatter` | `xs`, `ys`, `zs` | Markers at 3D points. |
| `Plot3DSurface` | `xs`, `ys`, `zs`, `x_count`, `y_count`, `scale_min`, `scale_max` | A surface over a grid of `x_count` by `y_count` points, listed row by row. All three lists have `x_count * y_count` values. |
| `Plot3DTriangle` | `xs`, `ys`, `zs` | Filled triangles; every 3 points make one. |
| `Plot3DQuad` | `xs`, `ys`, `zs` | Filled quads; every 4 points make one. |
| `Plot3DMesh` | `xs`, `ys`, `zs`, `indices` | A triangle mesh; `indices` lists 3 point numbers (from 0) per triangle. |
| `Plot3DText` | `text`, `x`, `y`, `z`, `angle`, `offset_x`, `offset_y` | A text label at a 3D coordinate. |

### Colours and style

Without any of these, series are coloured automatically in this order:
blue, orange, green, red, purple, brown, pink, grey, yellow, cyan.

Fields on every series, 2D and 3D:

| Field | Default | Meaning |
|---|---|---|
| `color` | automatic | The series colour, for its lines, fills and markers. |
| `fill_color` | same as `color` | A different colour for filled areas (bar faces, shaded regions). |
| `line_weight` | `1` | Line thickness in pixels. |
| `fill_alpha` | automatic | Opacity of fills, from 0 (transparent) to 1 (solid). |
| `marker` | none on lines, circles on scatters | A point marker: `None`, `Circle`, `Square`, `Diamond`, `Up`, `Down`, `Left`, `Right`, `Cross`, `Plus`, `Asterisk`. |
| `marker_size` | automatic | Marker radius in pixels. |

**Colours are written `"#RRGGBB"` or `"#RRGGBBAA"`, always in quotes.**
Without quotes, YAML reads `#` as the start of a comment and nymph reports
an error. Colour names such as `red` are not accepted.

```yaml
- type: PlotLine
  label: target
  xs: [0.5, 6.5]
  ys: [8, 8]
  color: "#C0392B"
  line_weight: 2
```

`PlotHeatmap`, `PlotHistogram2D`, `PlotPieChart`, `PlotText` and
`Plot3DText` ignore these fields. Heatmap cells, pie slices and surfaces
are coloured by the plot's `colormap` instead:

| `colormap` | Kind |
|---|---|
| `Deep` (default), `Dark`, `Pastel`, `Paired` | Distinct colours, for separate series and pie slices. |
| `Viridis`, `Plasma`, `Hot`, `Cool`, `Pink`, `Jet`, `Greys` | A smooth scale from low to high, for heatmaps and surfaces. |
| `RdBu`, `BrBG`, `PiYG`, `Spectral`, `Twilight` | A scale with a distinct middle, for values around a centre such as zero. |

`colormap` is set on the `Plot` or `Plot3D`, not on the series.

### Layout elements

| Type | Fields | Use |
|---|---|---|
| `VerticalLayout` | `spacing` (pixels between children), `children` | Stack plots top to bottom. Give each plot a `height`. |
| `HorizontalLayout` | `spacing`, `children` | Place plots side by side. Give each plot `width: -1` to share the width equally, and a `height`. |
| `Label` | `text` | A line of text, for example a heading above the plots. |
| `Separator` | none | A horizontal rule. |

No other element types are accepted.

## Recipes

Each of these is a complete source file.

### Line chart, several series

```
CPU and memory use during the load test.
--
image:
  width: 800
  height: 420
root:
  type: Plot
  title: Load test
  x_label: minute
  y_label: "%"
  y_min: 0
  y_max: 100
  children:
    - type: PlotLine
      label: cpu
      xs: $minute
      ys: $cpu
      color: "#C0392B"
      line_weight: 2
      marker: Circle
    - type: PlotLine
      label: memory
      xs: $minute
      ys: $mem
      color: "#2471A3"
      line_weight: 2
--
minute, cpu, mem
0, 12, 40
1, 35, 42
2, 71, 47
3, 88, 55
4, 64, 58
5, 30, 57
```

### Bar chart

Set `x_min` and `x_max` half a step beyond the first and last bar, or the
outer bars are cut in half.

```
Tickets closed per team last sprint.
--
image:
  width: 640
  height: 400
root:
  type: Plot
  title: Tickets closed
  x_label: team number
  y_label: tickets
  x_min: 0.5
  x_max: 4.5
  y_min: 0
  y_max: 40
  children:
    - type: PlotBars
      label: closed
      xs: $team
      ys: $closed
      bar_size: 0.6
--
team, closed
1, 31
2, 24
3, 37
4, 18
```

### Histogram

`values` takes the raw samples; nymph does the binning.

```
Distribution of response times.
--
root:
  type: Plot
  title: Response time
  x_label: ms
  y_label: requests
  children:
    - type: PlotHistogram
      label: latency
      values: $ms
      bins: 8
--
ms
118.2
131.0
96.4
142.7
109.9
125.3
88.1
137.6
121.4
114.0
```

### Pie chart

The fixed ranges, `Equal` and `NoDecorations` are needed for a round pie
without axes. Keep the image square. The number printed on each slice is
the value as given in the data, not a computed share: give percentages (as
here) with the default label format, or give counts and set
`label_fmt: "%.0f"`.

```
Share of traffic by browser.
--
image:
  width: 440
  height: 440
root:
  type: Plot
  title: Browser share
  flags: Equal
  x_flags: NoDecorations
  y_flags: NoDecorations
  x_min: 0
  x_max: 1
  y_min: 0
  y_max: 1
  children:
    - type: PlotPieChart
      labels: $browser
      values: $share
      normalize: true
--
browser, share
Chrome, 63
Safari, 20
Firefox, 6
Edge, 5
Other, 6
```

### Heatmap

One CSV column per grid column, one CSV row per grid row. `rows` and `cols`
must match the data, and `scale_min` / `scale_max` should cover its range.
Set a smooth `colormap` such as `Viridis`; the default one is meant for
separate series and makes neighbouring values look unrelated. `NoLegend`
keeps the legend box from covering the first cell.

```
Activity by weekday and time of day.
--
image:
  width: 520
  height: 360
root:
  type: Plot
  title: Activity
  colormap: Viridis
  flags: NoLegend
  x_flags: NoDecorations
  y_flags: NoDecorations
  children:
    - type: PlotHeatmap
      label: activity
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

### Several charts

Stacked:

```
Temperature and humidity over a day.
--
image:
  width: 640
  height: 540
root:
  type: VerticalLayout
  spacing: 6
  children:
    - type: Label
      text: Greenhouse sensors
    - type: Plot
      title: Temperature
      height: 240
      y_label: C
      children:
        - type: PlotLine
          label: temp
          xs: $hour
          ys: $temp
    - type: Plot
      title: Humidity
      height: 240
      x_label: hour
      y_label: "%"
      children:
        - type: PlotShaded
          label: humidity
          xs: $hour
          ys: $hum
--
hour, temp, hum
0, 12.8, 73
6, 14.9, 66
12, 23.2, 47
18, 21.0, 52
```

Side by side:

```
Before and after the cache change.
--
image:
  width: 900
  height: 380
root:
  type: HorizontalLayout
  spacing: 8
  children:
    - type: Plot
      title: Before
      width: -1
      height: 360
      y_min: 0
      y_max: 400
      children:
        - type: PlotLine
          label: p95 ms
          xs: $hour
          ys: $before
    - type: Plot
      title: After
      width: -1
      height: 360
      y_min: 0
      y_max: 400
      children:
        - type: PlotLine
          label: p95 ms
          xs: $hour
          ys: $after
--
hour, before, after
1, 310, 120
2, 345, 118
3, 298, 131
4, 372, 125
```

### 3D surface

The CSV lists the grid row by row: all X values for the first Y, then the
next Y. Here the grid is 3 by 3.

```
Height of the test surface.
--
image:
  width: 640
  height: 480
root:
  type: Plot3D
  title: Surface
  children:
    - type: Plot3DSurface
      label: height
      xs: $x
      ys: $y
      zs: $z
      x_count: 3
      y_count: 3
--
x, y, z
0, 0, 0.0
1, 0, 0.5
2, 0, 0.0
0, 1, 0.5
1, 1, 1.0
2, 1, 0.5
0, 2, 0.0
1, 2, 0.5
2, 2, 0.0
```

## Making a good chart

- Give every chart a `title` and axis labels with units.
- Give every series a `label` when there is more than one.
- Write a real description in part 1. It is the image's alt text.
- For values that are not numbers on the X axis (names, dates), number the
  rows in a column and plot against that column; say what the numbers mean
  in `x_label` or the description. Axis ticks are always numeric.
- Set `y_min: 0` (with a `y_max`) for bar charts, so bar lengths are honest.
- Keep to about five series per plot. Split into several plots beyond that.
- Use `line_weight: 2` for the lines that matter; one-pixel lines are faint
  in a document.
- Set colours when they carry meaning (red for a limit, the same colour for
  the same thing across charts). Otherwise leave them automatic.
- Use `scale: 2` when the image will be viewed large.

## Limits

- A one-pixel line that is exactly horizontal or vertical (a flat reference
  line drawn with `PlotLine` or `PlotInfLines`) comes out pale grey. Give
  such a line `line_weight: 2`.
- Pie slices, heatmap cells and surfaces cannot be given individual
  colours; choose a `colormap` for the plot instead.
- Axis ticks are numbers only: no date or category axes.
- The source stays in the PNG only while the file is copied unchanged.
  Services that re-encode images remove it. Keep the `.nymph` file when the
  chart may need changing later.
- Maximum source size is 16 MiB and 1,000,000 data rows.

## Common errors

| Message contains | Cause and fix |
|---|---|
| `expected three parts ... separated by two '--' lines` | A separator is missing, indented, or has other text on its line. |
| `unknown column 'x'` | The `$x` name does not match a CSV header. The message lists the headers. |
| `type 'X' is not allowed here` | `X` is misspelled or not one of the types in this file. |
| `a Plot can only contain 2D series` / `must be a child of a Plot` | A series is at the wrong level. Put it under a `Plot`'s `children`. |
| `Plot has no field 'x'` | The field name is misspelled or belongs to another type. |
| `unknown value 'X' (one of: ...)` | A flag name is wrong. The message lists the valid ones. |
| `is not a colour` / `is empty; an unquoted '#' starts a YAML comment` | Write the colour as `"#RRGGBB"`, in quotes. |
| `must be a list of numbers or a $column reference` | A list field was given a single number or plain text. |
| `mixes numbers and $column references` | Use only numbers or only references in one list. |
| `'abc' is not a number` | A referenced column has a non-numeric value on the reported line. |
| `CSV row has N cells, the header has M` | A row is too short or too long, or a value with a comma is not quoted. |
| `YAML: ...` | A YAML syntax error at the reported line: check indentation, colons and quotes. |
| `not a nymph image` | The PNG given as input was not made by nymph, or lost its embedded source. |
