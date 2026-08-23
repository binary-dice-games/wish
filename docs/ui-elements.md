# wish UI Elements Reference

A complete reference for every built-in wish UI element: what it's for,
its fields, and the events it emits — plus how windows/layouts work, the
JSON/YAML template schema, and how to use the `editor` tool to preview a
UI from a JSON file without writing any client code.

This document is self-contained: it exists so a human or an AI agent can
write a correct wish UI JSON template from this file alone, without
reading `src/ui/ui_elements/*.cpp`. If a field or event listed here ever
looks wrong, the source under `src/ui/ui_elements/`, `src/ui/plot_elements/`,
and `src/ui/plot3d_elements/` (`addField`/`addMethod` calls) plus the
matching `render_*` function in `src/imgui/imgui_*_renderer.cpp` (for event
payloads) is authoritative — file update requests against those, and this
document if it has drifted.

## 1. The object model in brief

A wish UI is a tree of typed objects (bison RMI classes registered in the
`"wish"` namespace). Every element:

- has a `type` (its registered class name, e.g. `"Button"`),
- has zero or more **fields** (typed properties, set via JSON or
  `proxy.set({...})`),
- may have a `children` map (named or numerically-indexed nested elements),
- may emit named **events** (`clicked`, `changed`, ...) that a client
  subscribes to with `proxy.onEvent(name, handler)`.

Every element class ultimately derives from the base `Element` class, which
contributes these fields to every widget (not repeated per element below):

| Field | Type | Default | Description |
|---|---|---|---|
| `visible` | `bool` | `true` | Whether the element is rendered. |
| `children` | map/list | `{}` | Nested child elements. |
| `order` | `int32` | `0` | Render order within the parent's children; lower renders first. |
| `font_path` | `string` | `""` | Path to a TTF font file (sandboxed; see `CLAUDE.md`'s security section). |
| `font_size` | `float` | `0.0` | Font size in pixels; `0` uses the default font. |
| `drag_type` | `string` | `""` | Opaque type tag; non-empty makes this element a drag source. See "Drag and drop" below. |
| `drag_payload` | `string` | `""` | Opaque payload bytes carried by a drag started from this element. |
| `drop_type` | `string` | `""` | Opaque type tag; non-empty makes this element a drop target that accepts drags whose `drag_type` matches exactly. |

Children are addressed by **dot-path**: a child keyed `"ok"` under a window
named at the root is `"ok"`; nested further, `"row.ok"`. Numerically-indexed
children (an array under `children`) are addressed by integer index instead
and are not given a name.

### Drag and drop

Any element becomes a **drag source** by setting a non-empty `drag_type`
(plus, usually, `drag_payload`); any element becomes a **drop target** by
setting a non-empty `drop_type`. A drop is accepted only when a target's
`drop_type` exactly matches the dragged element's `drag_type` — there is no
wildcard or multi-type matching. On a successful drop (release while
hovering a matching target), the target's own widget id fires a `"dropped"`
event with payload `{"type": <the matched type tag>, "payload": <the
source's drag_payload, verbatim>}`.

Both fields are generic and domain-agnostic (wish itself attaches no
meaning to the tag or payload strings — an application defines its own
type-tag/payload convention, e.g. `"<asset class>|<path>"`) and are
implemented once, generically, via `handle_drag_drop()`
(`src/imgui/imgui_renderer.cpp`) rather than per element class, so they work
on any element without extra wiring.

**Only meaningful on a leaf element** whose render function draws exactly
one top-level ImGui item (`Button`, `Image`, `Label`, `Checkbox`, ...) —
`BeginDragDropSource`/`BeginDragDropTarget` attach to "whatever item was
drawn last," so setting these fields on a container/layout element attaches
to its last-rendered child instead of the container itself. This mirrors
the same "leaf widgets only" caveat `src/automation/DESIGN.md` documents for
hit-test rects. `imgui_renderer::render_node()` calls `handle_drag_drop()`
for every element after its own dispatch; `render_table()` additionally
calls it directly for each `TableRow` right after that row's hit-test
`Selectable()`, since `TableRow` children never go through `render_node()`
themselves (only their cells do) — see `TableRow`'s own field table above.

## 2. Windows and layouts

### `Window`

Every top-level UI is rooted in a `Window` — it's the only element that
gets its own OS/browser-canvas frame; every other element must live inside
one (directly or via nested layouts).

**Description:** "A top-level window container."

| Field | Type | Default | Description |
|---|---|---|---|
| `title` | `string` | `""` | Window title bar text. |
| `width` | `int32` | `0` | Window width in pixels (0–16384). |
| `height` | `int32` | `0` | Window height in pixels (0–16384). |
| `pos_x` | `int32` | `0` | Horizontal position in pixels. |
| `pos_y` | `int32` | `0` | Vertical position in pixels. |
| `closable` | `bool` | `false` | Show a close button (X) on the title bar; clicking it emits `closed`. |
| `flags` | `int32` (flags) | `0` | Bitmask, combine names with `\|`: `NoTitleBar`, `NoResize`, `NoMove`, `NoScrollbar`, `NoScrollWithMouse`, `NoCollapse`, `AlwaysAutoResize`, `NoBackground`, `NoSavedSettings`, `NoMouseInputs`, `MenuBar`, `HorizontalScrollbar`, `NoFocusOnAppearing`, `NoBringToFrontOnFocus`, `AlwaysVerticalScrollbar`, `AlwaysHorizontalScrollbar`, `NoNavInputs`, `NoNavFocus`, `UnsavedDocument`, `NoDocking`, and composites `NoNav`, `NoDecoration`, `NoInputs`. |

**Events:** `closed` — fired when the user clicks the close button (requires
`closable: true`); no payload. The client is expected to tear down the
window's objects (or hide it) in response.

### `Layout`, `VerticalLayout`, `HorizontalLayout`

Layouts are ordinary nodes in the object tree, not a separate concept —
nest them freely to build rows, columns, and grids.

`Layout` is a common base (not instantiated directly) that adds:

| Field | Type | Default | Description |
|---|---|---|---|
| `spacing` | `float` | `0.0` | Space between child elements in pixels (0–256). |
| `width` | `float` | `0.0` | Column width hint used when this element is a direct child of a `HorizontalLayout`: `0` sizes to content; a positive value reserves that many fixed pixels; a negative value makes it a stretch column, sharing whatever width remains after fixed columns and spacing, weighted by its magnitude relative to other stretch columns (mirrors ImGui's `ImGuiTableColumnFlags_WidthStretch` weight convention). Ignored outside a `HorizontalLayout`. |
| `height` | `float` | `0.0` | Row height hint used when this element is a direct child of a `VerticalLayout` — the same convention as `width`, on the height axis. An auto (`0`) row's previous frame's measured height is reserved for this frame's stretch-row sizing, so a fixed header/footer plus one stretch-filling body works regardless of child order. Ignored outside a `VerticalLayout`. |

- **`VerticalLayout`** — stacks children top-to-bottom. No extra fields.
- **`HorizontalLayout`** — places children left-to-right. Adds:

  | Field | Type | Default | Description |
  |---|---|---|---|
  | `align` | `string` | `"left"` | `"left"` (default) or `"right"`. Right alignment flushes children to the content edge; every child needs an explicit `width` for this to work. `Spring` (below) is the more flexible alternative — it also handles centering and space-between. |

Neither layout emits events.

### `Spring`

An expandable, invisible space that shares whatever room is left over in a
`HorizontalLayout`'s or `VerticalLayout`'s stretch pool — the same pool
`Layout.width`/`Layout.height`'s negative-value convention draws from — with
sibling stretch children and other `Spring`s, weighted by its `weight`
field. Placed among a layout's children, it gives CSS-flexbox-style control
over alignment without needing every child to have an explicit width:

| Field | Type | Default | Description |
|---|---|---|---|
| `weight` | `float` | `1.0` | Share of the layout's leftover space this `Spring` claims, relative to sibling `Spring`s/stretch children. `0` claims no share at all -- the `Spring` collapses, flushing its neighbor to that edge (CSS `flex-grow`'s convention). Negative values clamp to `0`. |

Does not emit events. Ignored (renders as zero-size) outside a
`HorizontalLayout`/`VerticalLayout`.

```json
{
  "type": "HorizontalLayout",
  "children": {
    "s1": { "type": "Spring" },
    "btn": { "type": "Button", "label": "Centered", "width": 100 },
    "s2": { "type": "Spring" }
  }
}
```

Two `Spring`s around one child centers it; a `Spring` between two children
pushes them to opposite edges ("space-between"); unequal `weight` values
bias the split (e.g. `weight: 1` and `weight: 2` splits leftover space 1:2).

### `Splitter`

Arranges its children as resizable panes separated by user-draggable bars
([dear imgui issue #319](https://github.com/ocornut/imgui/issues/319)'s
technique, built on the public `InvisibleButton` API — no `imgui_internal.h`
dependency). All panes but the last get an explicit pixel size, stored in
that child's own `width` field (`orientation: "vertical"`) or `height`
field (`orientation: "horizontal"`) — the same field `HorizontalLayout`/
`VerticalLayout` already read as a column/row-size hint on any child, so a
pane's current size is readable/settable the same way as any other layout
child's. A pane left unset (`0`, the default) splits the remaining space
evenly with its unset siblings on first render. The **last** pane is never
stored — it always fills whatever space remains after the others and the
bars' thickness, so leave it unset for the common "fixed sidebar + filling
content" case. For 3+ panes, nest a `Splitter` inside another `Splitter`'s
last pane, exactly like #319's own 3-pane demo calls `Splitter()` twice.

| Field | Type | Default | Description |
|---|---|---|---|
| `orientation` | `string` | `"vertical"` | `"vertical"` draws a vertical drag bar and arranges panes side by side (sizes go in each pane's `width`). `"horizontal"` draws a horizontal drag bar and stacks panes top to bottom (sizes go in each pane's `height`). |
| `thickness` | `float` | `4.0` | Pixel thickness of each draggable bar (1–64). |
| `min_pane_size` | `float` | `20.0` | Minimum pixel size any pane can be dragged down to. |

**Events:** `resized` — fired when a drag bar is released; payload
`{pane_index: int, size1: float, size2: float}` where `size1`/`size2` are
the post-drag sizes of the pane before and after that bar.

```json
{
  "type": "Splitter", "thickness": 6,
  "children": {
    "sidebar": { "type": "TreeNode", "label": "Files", "width": 220 },
    "content": { "type": "Label", "text": "Select a file" }
  }
}
```

### Docking

`DockSpaceViewport` (full-viewport dockspace host — nested `Window` children
become independently dockable) and `DockSpace` (an inline dockspace inside
an existing window) are covered in §4's docking section below; they're
listed here only as a pointer since they're conceptually part of the
window/layout system.

## 3. The JSON/YAML template schema

A template is a tree of `{ "type": ..., ...fields, "children": {...} }`
objects, parsed by `import_json`/`import_yaml` (server-side) or the
client-side descriptor parser used by `register_template_from_json`/
`_yaml`. Example:

```json
{
  "type": "Window",
  "title": "Confirm",
  "width": 300,
  "height": 120,
  "closable": true,
  "children": {
    "msg":    { "type": "Label",  "text": "Delete file?" },
    "row": {
      "type": "HorizontalLayout",
      "spacing": 8,
      "children": {
        "ok":     { "type": "Button", "label": "OK" },
        "cancel": { "type": "Button", "label": "Cancel" }
      }
    }
  }
}
```

Rules:

- `type` — required. The registered class name (`"Window"`, `"Button"`,
  `"Plot"`, `"PlotLine"`, ...) — see the catalog in §4.
- Every other top-level key besides `type`/`children` is a field on that
  class (see the field tables below). An unknown field name is silently
  ignored by the importer, so a typo does not raise an error — double-check
  spelling against the tables here.
- `children` — a JSON object maps a **name** (string key) to a child
  descriptor, giving that child a dot-path name (`"row.ok"` above). A JSON
  array instead gives children **numeric** indices (no name, not addressable
  by dot-path — used rarely, e.g. dynamically generated table rows).
- Nesting is unbounded — layouts, tabs, trees, tables, and plots all accept
  `children` the same way.
- The root node's own `type` is usually `"Window"`, but a template's root
  can be any element — useful when instantiating a fragment into an existing
  window rather than a whole new one.
- Registering: `client.register_template_from_json(name, jsonText)` (or
  `_from_yaml`) stores the template on the server; `instantiate_template(name,
  prefix)` creates a fresh, independent object tree from it, rooted at dot-path
  `prefix`. Building object-by-object in code (`client.instantiate(ns, type)`
  + `.set({...})`) is the equivalent lower-level API and produces the same
  tree shape.

## 4. UI element catalog

Fields below omit the common `Element` fields listed in §1
(`visible`/`children`/`order`/`font_path`/`font_size`) — they apply to every
element in this catalog too.

### Basic controls

#### `Button`
A clickable button.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Button caption text. |

**Events:** `clicked` — button pressed; no payload.

#### `Label`
Read-only static text; no events.

| Field | Type | Default | Description |
|---|---|---|---|
| `text` | `string` | `""` | Displayed text content. |
| `text_color` | `string` | `""` | Optional `"#RRGGBBAA"`/`"#RRGGBB"` text color override; empty uses the current theme's text color. |
| `text_color_light` | `string` | `""` | Overrides `text_color` while the session's active theme is light-based; re-evaluated every frame, so it follows a theme change made after this Label was created. Ignored when empty. |
| `text_color_dark` | `string` | `""` | Same as `text_color_light`, used instead while the active theme is dark-based. Ignored when empty. |
| `wrap` | `bool` | `false` | When true, wraps text to the available width instead of overflowing/clipping on one line. |

#### `Checkbox`
A toggleable checkbox with a label.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Checkbox caption text. |
| `value` | `bool` | `false` | Current checked state. |

**Events:** `changed` — `{ value: bool }`.

#### `SliderFloat` / `SliderInt`
A draggable slider bound to a float or int value.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Slider label text. |
| `value` | `float`/`int32` | `0.0` / `0` | Current value, clamped to `[min, max]`. |
| `min` | `float`/`int32` | `0.0` / `0` | Minimum selectable value. |
| `max` | `float`/`int32` | `1.0` / `100` | Maximum selectable value. |
| `format` | `string` | `"%.2f"` | (`SliderFloat` only) printf format for the displayed value. |
| `width` | `float` | `0.0` | Width in pixels; `0` = ImGui default, `-1` = fill remaining width. |

**Events:** `changed` — `{ value: float|int32 }`.

#### `InputText`
A single-line text field.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Input field label. |
| `value` | `string` | `""` | Current text value. |
| `hint` | `string` | `""` | Placeholder shown when empty. |
| `max_length` | `int32` | `256` | Max characters (1–65536). |
| `width` | `float` | `0.0` | Width in pixels; `0` = ImGui default, `-1` = fill remaining width. |
| `flags` | `int32` (flags) | `0` | ImGuiInputTextFlags bitmask; e.g. `EnterReturnsTrue = 32` makes `changed` fire only on Enter instead of every keystroke. |
| `multiline` | `bool` | `false` | When true, renders as a resizable multi-line box (`ImGui::InputTextMultiline`) instead of a single-line field. `hint` is ignored in this mode. |
| `height` | `float` | `0.0` | Box height in pixels when `multiline` is true; `0` uses ImGui's default (a few lines). |

**Events:** `changed` — `{ value: string }`, fired on edit (or only on
Enter if `flags` includes `EnterReturnsTrue`).

#### `ColorEdit`
A color swatch that opens a picker popup when clicked.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Caption to the left of the swatch. |
| `value` | `float[3\|4]` | `[1,1,1,1]` | Current color as `[r,g,b]` or `[r,g,b,a]` components in `[0,1]`. The component count (3 or 4) selects `ColorEdit3` vs. `ColorEdit4`. |
| `flags` | `int32` (flags) | `0` | ImGuiColorEditFlags bitmask (e.g. `NoAlpha=2`, `NoInputs=32`, `PickerHueWheel=16777216`). |
| `width` | `float` | `0.0` | Width in pixels; `0` = ImGui default, `-1` = fill remaining width. |

**Events:** `changed` — `{ value: float[] }` (same component count as the
current `value`).

#### `InputInt` / `InputFloat`
A numeric field with +/- step buttons.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Caption to the left of the box. |
| `value` | `int32`/`float` | `0` | Current value. |
| `step` | `int32`/`float` | `1` / `0.0` | Amount added/subtracted per +/- click. `InputFloat`: `0` hides the buttons. |
| `step_fast` | `int32`/`float` | `100` / `0.0` | Step amount while Ctrl is held. `InputFloat`: `0` falls back to `step`. |
| `format` | `string` | `"%.3f"` | (`InputFloat` only) printf display format. |
| `width` | `float` | `0.0` | Width in pixels; `0` = ImGui default, `-1` = fill remaining width. |
| `flags` | `int32` (flags) | `0` | (`InputInt` only) ImGuiInputTextFlags bitmask: `EnterReturnsTrue=64` (fire `changed` only on Enter/deactivation, not every keystroke -- useful when `changed` triggers an expensive owner-side reaction), `ReadOnly=512`, `AutoSelectAll=4096`. |

**Events:** `changed` — `{ value: int32|float }`.

#### `DragInt` / `DragFloat`
Click-and-drag to change; double-click to type a value directly.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Caption to the left of the widget. |
| `value` | `int32`/`float` | `0` | Current value. |
| `speed` | `float` | `1.0` | Change per pixel dragged. |
| `min` | `int32`/`float` | `0` | Lower clamp; when `min == max`, unclamped. |
| `max` | `int32`/`float` | `0` | Upper clamp; when `min == max`, unclamped. |
| `format` | `string` | `"%.3f"` | (`DragFloat` only) printf display format. |

**Events:** `changed` — `{ value: int32|float }`.

#### `Combo`
A drop-down selection list.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Caption to the left of the dropdown. |
| `items` | `string` | `""` | Newline-separated option strings, e.g. `"A\nB\nC"`. |
| `value` | `int32` | `0` | Index of the selected item (0-based). |
| `width` | `float` | `0.0` | Width in pixels; `0` = ImGui default, `-1` = fill remaining width. |

**Events:** `changed` — `{ value: int32 (index), text: string }` (`text`
present only when `value` is within bounds of `items`).

#### `RadioButton`
A single radio circle. Build a group by placing several with the same
semantic purpose; the **server** (not the widget itself) is responsible for
maintaining mutual exclusivity — set `active: false` on the others in
response to `clicked`.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Caption beside the circle. |
| `active` | `bool` | `false` | Selected (filled) state. |

**Events:** `clicked` — no payload; the renderer does **not** update
`active` itself.

#### `Selectable`
A highlight-on-hover row, useful for building list boxes or custom menus.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Text shown inside the area. |
| `selected` | `bool` | `false` | Highlighted/selected state. |
| `width` | `float` | `0.0` | Width in pixels; `0` fills available width. |
| `height` | `float` | `0.0` | Height in pixels; `0` uses the default line height. |

**Events:** `changed` — `{ selected: bool }`.

If given `children`, they render as overlay content on top of the
Selectable's own clickable area instead of `label` — e.g. an `Image` plus a
caption `Label`, so clicking anywhere in the tile (not just the caption
text) fires `changed`. Give a childful `Selectable` an explicit nonzero
`width`/`height` covering the children's combined size: `0` falls back to
plain fill-width/single-line sizing, which won't cover taller content.

```json
{
  "type": "Selectable", "width": 84, "height": 108,
  "children": {
    "thumb": { "type": "Image", "src": "cat.png", "width": 84, "height": 84 },
    "caption": { "type": "Label", "text": "cat.png" }
  }
}
```

#### `ProgressBar`
Non-interactive horizontal progress indicator; no events.

| Field | Type | Default | Description |
|---|---|---|---|
| `value` | `float` | `0.0` | Fill fraction, `0.0`–`1.0`. |
| `label` | `string` | `""` | Optional text drawn on top of the bar. |
| `width` | `float` | `-1.0` | Bar width; `-1` fills available width. |
| `height` | `float` | `0.0` | Bar height; `0` uses default height. |

#### `Image`
Displays an image previously uploaded via the file service; no events.

| Field | Type | Default | Description |
|---|---|---|---|
| `src` | `string` | `""` (required) | Resource file name in the session sandbox folder. |
| `width` | `int32` | `0` | Display width; `0` uses the image's natural width (0–16384). |
| `height` | `int32` | `0` | Display height; `0` uses the image's natural height (0–16384). |
| `tint` | `string` | `""` | Optional `"#RRGGBBAA"`/`"#RRGGBB"` hex color multiplied over the whole image; empty draws it unmodified. |
| `tint_light` | `string` | `""` | Overrides `tint` while the session's active theme is light-based; re-evaluated every frame. Ignored when empty. |
| `tint_dark` | `string` | `""` | Same as `tint_light`, used instead while the active theme is dark-based. Ignored when empty. |

#### `Separator` / `SeparatorText`
Visual dividers; no events.

`Separator` has no fields (a plain horizontal rule). `SeparatorText` adds:

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Text shown inline with the rule. |

### Menus

#### `MenuBar`
Container for `Menu` children. Must be a direct child of a `Window`. No
fields, no events. A trailing `Label` child (e.g. a clock) is right-aligned
instead of flowing left-to-right.

#### `Menu`
A drop-down menu; children may be `MenuItem`, nested `Menu` (submenu), or
`Separator`. No events.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Text shown in the parent bar/menu. |
| `enabled` | `bool` | `true` | When `false`, grayed out and cannot open. |

#### `MenuItem`
A selectable leaf inside a `Menu`.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Item text. |
| `shortcut` | `string` | `""` | Decorative shortcut hint shown on the right (not a real keybinding). |
| `checked` | `bool` | `false` | Shows a check mark. Purely display -- the renderer never modifies it; the form sets it explicitly (e.g. a radio-style submenu recomputing it from server state). |
| `enabled` | `bool` | `true` | When `false`, grayed out and cannot be clicked. |
| `copy_text` | `string` | `""` | When non-empty, clicking the item also copies this text to the OS clipboard (`ImGui::SetClipboardText`, on the render thread) -- a "Copy ..." action with no client round trip. |

**Events:** `clicked` — `{ checked: bool }` (the field's current value,
unchanged by the click).

#### `MenuButton`
An ordinary button that opens a popup containing its own children
(`MenuItem`, `Menu`, or `Separator`) when clicked, rendered exactly as they
would be inside a `MenuBar`. Unlike `MenuBar`/`Menu`, it needs no
surrounding menu context of its own — it opens that context itself on
click — so it can appear anywhere an ordinary `Button` can (e.g. a
toolbar), not just inside a `Window`'s menu-bar strip or an already-open
`Menu`. No events of its own; its `MenuItem` children fire `clicked` the
same as they would inside a `Menu`.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Button caption text. |

#### `MenuBarExtension`
Extends the server's own chrome menu bar with app-supplied content —
register as a top-level object (not nested under a `Window`); children are
`Menu`/trailing `Label`, same shape as `MenuBar`. Removed automatically on
session disconnect. No fields, no events.

#### `ContextMenu`
A right-click popup. It draws nothing itself — it opens when the *previous
sibling* in the same parent is right-clicked, via
`ImGui::BeginPopupContextItem()` attaching to that sibling's last-drawn
ImGui item, the same way `MenuButton` attaches its popup to its own trigger
`Button`. Works after any normal widget reached through the generic child
dispatch (`Button`, `Checkbox`, ...). As a child of `TableRow` it is a
special case instead: `Table`'s renderer excludes it from column layout and
opens it on a right-click anywhere on that row (see `TableRow` below), since
a cell's `Label` content has no stable item id of its own to attach to.
Children should be `MenuItem`, `Menu` (submenu), or `Separator` elements,
rendered inside the popup exactly as they would inside a `MenuBar`. No
fields, no events of its own — its `MenuItem` children fire `clicked` the
same as they would inside a `Menu`.

### Tabs

#### `TabBar`
Container for `TabItem` children.

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | `string` | `"##tabbar"` | ImGui identifier, unique within its window. |

No events.

#### `TabItem`
One page inside a `TabBar`.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Tab button text. |
| `closable` | `bool` | `false` | Show a close (×) button on the tab. |

**Events:**
- `selected` — fired once on the transition to being the active tab; no payload.
- `closed` — fired when the × is clicked (requires `closable: true`); no payload.

### Tree / collapsing sections

#### `TreeNode`
A collapsible node with an expand arrow.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Text next to the arrow. |
| `open` | `bool` | `false` | Initial open/closed state (applied once; ImGui manages it afterward). |
| `leaf` | `bool` | `false` | When `true`, renders without an arrow and hides children. |

**Events:** `toggled` — `{ open: bool }`.

#### `CollapsingHeader`
A bold section header toggling visibility of its children.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Header text. |

**Events:** `toggled` — `{ open: bool }`.

### Tables

#### `Table`
A multi-column table. Direct children of type `TableColumn` define columns;
other children (typically `TableRow`) provide data rows.

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | `string` | `"##table"` | ImGui string identifier. |
| `columns` | `int32` | `1` | Number of columns (1–64). |
| `flags` | `int32` (flags) | `0` | ImGuiTableFlags bitmask, e.g. `Resizable=1`, `RowBg=64`, `Borders=1920`, `Sortable=8`. |
| `outer_width` | `float` | `0.0` | Outer container width; `0` fills available width. |
| `outer_height` | `float` | `0.0` | Outer container height; `0` = no clipping/scroll region. |
| `inner_width` | `float` | `0.0` | Width allocated to contents; `0` uses outer width. |
| `headers` | `bool` | `false` | Render a header row from `TableColumn` labels. |
| `auto_scroll` | `bool` | `true` | When `flags` includes `ScrollY` and this is true, automatically scroll to the newest row whenever the row count grows (e.g. a live log table following its latest entry). Set `false` to leave the scroll position alone as rows are appended. |

**Events:**
- `sorted` — fired when `flags` includes `Sortable` (`8`) and the sort spec
  changes (initial default sort, or a header click); `{ column_id: int32, ascending: bool }`
  (`column_id` echoes the clicked `TableColumn.column_id`). The owner is
  responsible for actually reordering rows.
- `row_selected` — single click on a `TableRow`; `{ index: int32, ctrl: bool,
  shift: bool }` (0-based row index, plus whether Ctrl/Shift were held at
  click time — captured the same frame as the click, since the event is only
  delivered later, by which point `ImGuiIO`'s own key state may have moved
  on). The owner is responsible for turning `ctrl`/`shift` into
  multi-selection: `ctrl` toggles just this row, leaving the rest of the
  selection untouched; `shift` selects the contiguous range between this row
  and whatever the owner considers the current anchor (typically the last
  plain click). Holding Shift while dragging across other rows re-emits
  `row_selected` for each newly hovered row (`shift: true`, deduped so a
  multi-frame hover over the same row only fires once) — the "group
  selection" drag gesture, gated on Shift so a plain click-drag stays free
  for a row's own `drag_type`/`drop_type` drag-and-drop.
- `row_activated` — double-click on a `TableRow`; same payload shape as
  `row_selected`. At most one of `row_selected`/`row_activated` fires per
  frame.

#### `TableColumn`
Defines one column; processed by the parent `Table` during setup, not
rendered independently. No events.

| Field | Type | Default | Description |
|---|---|---|---|
| `label` | `string` | `""` | Header text (shown when `Table.headers` is true). |
| `flags` | `int32` (flags) | `0` | ImGuiTableColumnFlags bitmask. |
| `init_width` | `float` | `0.0` | Initial width in pixels (or stretch weight). |
| `column_id` | `int32` | `0` | Stable ID echoed back in the parent `Table`'s `sorted` event payload — use it to map a sort click to a semantic field independent of column position. |

#### `TableRow`
A data row; each child occupies one column cell, left to right — except a
`ContextMenu` child (if present), which is excluded from column layout and
instead opens as a right-click menu for the whole row (see `ContextMenu`
under "Menus" above).

| Field | Type | Default | Description |
|---|---|---|---|
| `flags` | `int32` (flags) | `0` | ImGuiTableRowFlags bitmask, e.g. `Headers=1`. |
| `min_height` | `float` | `0.0` | Minimum row height; `0` uses default. |
| `selected` | `bool` | `false` | Renders the row highlighted, e.g. to show it is part of the current selection; the owner sets this per row, so multiple rows may be highlighted at once (multi-selection). |

No events of its own — `row_selected`/`row_activated` are emitted on the
**parent `Table`**, not on the row.

Unlike most elements, `TableRow` is rendered inline by `render_table()`
rather than through the generic per-element dispatch — but it still honors
its inherited `visible` field (see `Element` above): setting it `false`
skips the row entirely (no cells, no row-index bump for click events),
letting an owner hide/show already-buffered rows on the fly, e.g. to
implement a retroactive filter without re-adding rows.

Like any element, a `TableRow` may also set `drag_type`/`drag_payload`/
`drop_type` (see "Drag and drop" below) to act as a drag source and/or drop
target — `render_table()` checks these on the row's own hit-test item
directly, since `TableRow` children are rendered inline rather than through
the generic per-element dispatch the "Drag and drop" section otherwise
describes.

### Graph

#### `GraphNode`
Draws one row's local segment of a lane-based DAG graph (e.g. a git commit
graph): its own dot, plus every connector line touching that row, split
into a **top half** (row top → row center) and **bottom half** (row
center → row bottom) — the same top/bottom-split technique
`git log --graph`/gitk/magit use to render straight pass-through lines,
branch-out diagonals, and merge-in diagonals uniformly, meeting at each
row's dot in the middle.

Meant to sit as the leftmost cell of a `Table` row (a direct child of a
`TableRow`, alongside ordinary text-column cells): scrolling and row
selection (`row_selected`/`row_activated`) come from the surrounding
`Table` for free, so `GraphNode` is a pure drawing widget with **no
events of its own** and needs no click/hover handling. Not git-specific —
any lane-assigned DAG can drive it from the same field shape.

Colors are packed `0xRRGGBBAA` `int32` values (unlike `Label.text_color`/
`Image.tint`'s `"#RRGGBBAA"` hex-string convention), since the per-segment
arrays below need a field type usable inside `int32[]` — `bison::field`
has no `string[]` alternative (see `bison_common.hpp`'s `field_base`
variant) — and the single `color` field matches that for internal
consistency within this element.

| Field | Type | Default | Description |
|---|---|---|---|
| `lane` | `int32` | `0` | This row's own commit dot column (0-based). |
| `color` | `int32` | `0` | Packed `0xRRGGBBAA` color for this row's own dot. |
| `is_head` | `bool` | `false` | Draws a ring around the dot marking the current `HEAD`. |
| `is_working` | `bool` | `false` | Draws a hollow dot instead of filled — for a synthetic "uncommitted changes" row. |
| `top_from` / `top_to` / `top_color` | `int32[]` | `[]` | Parallel arrays: one entry per line segment in the row's top half, each segment's starting lane, ending lane (at the row's vertical center), and color. |
| `bottom_from` / `bottom_to` / `bottom_color` | `int32[]` | `[]` | Same shape, for the row's bottom half (starting at the vertical center). |
| `lane_width` | `float` | `16.0` | Pixel spacing between adjacent lanes (4–64). |
| `dot_radius` | `float` | `4.5` | Commit dot radius in pixels (1–16). |
| `row_height` | `float` | `0.0` | Total row height in pixels; `0` uses one text line height with spacing, matching an adjacent text cell's natural row height. |

A segment whose `from`/`to` lane match draws as a straight vertical line;
a lane-changing segment draws as a smooth cubic Bézier curve between the
two lanes. The element auto-sizes its reserved width to whichever lane
(its own dot, or any segment endpoint) sits furthest right, so a
branch-out/merge-in diagonal that fans wider than the row's own dot isn't
clipped. No events.

### Docking

#### `DockSpaceViewport`
Full-viewport dockspace host; `Window` children nested under it become
independently dockable panels. No events.

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | `string` | `"demo_dockspace"` | Identifier for the host window and dockspace. |
| `flags` | `int32` (flags) | `0` | ImGuiDockNodeFlags bitmask. |
| `passthru` | `bool` | `false` | Let the central node pass through mouse/keyboard input. |

#### `DockSpace`
An inline dockspace inside an existing window. No events.

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | `string` | `"dockspace"` | String identifier hashed into the ImGui DockSpace ID. |
| `width` | `float` | `0.0` | Width; `0` fills available width. |
| `height` | `float` | `0.0` | Height; `0` fills available height. |
| `flags` | `int32` (flags) | `0` | ImGuiDockNodeFlags bitmask. |

### Text editing

#### `TextEditor`
A full-featured code/text editor (ImGuiColorTextEdit-backed) bound to a
sandboxed file path.

| Field | Type | Default | Description |
|---|---|---|---|
| `file_path` | `string` | `""` (required) | Path to edit, sandboxed to the session resource dir (relative) or requiring `set_allow_absolute_paths(true)` (absolute). |
| `language` | `string` | `"none"` | Syntax highlighting: `"cpp"` (or `"c++"` alias), `"c"`, `"cs"`, `"glsl"`, `"hlsl"`, `"lua"`, `"python"`, `"sql"`, `"json"`, `"markdown"`, `"angelscript"`, `"none"`. |
| `read_only` | `bool` | `false` | When `true`, editing is disabled and `changed` never fires. |
| `width` | `int32` | `0` | Width in pixels (0–8192); `0` fills available width. |
| `height` | `int32` | `400` | Height in pixels (0–8192); `0` fills available height. |
| `wish_ui_schema` | `bool` | `false` | When `true` and `language` is `"json"`, enables cursor tracking (`cursor_moved` events) and autocomplete for wish UI element type names, field names, and enum values, sourced from the live class registry (see `src/ui/ui_schema_help.hpp`). Used by the `editor` module's source panel; off by default so unrelated `TextEditor` uses (e.g. nano) are unaffected. |

**Events:**
- `changed` — fired when text is edited (and `read_only` is `false`); the
  new content is written to disk first. `{ file_path: string }`.
- `saved` — fired on Ctrl+S while focused, signaling the client to
  `download_file`. `{ file_path: string }`.
- `cursor_moved` — fired on every caret move, only when `wish_ui_schema` is
  `true`. `{ line: int32, column: int32 }` (0-based; `column` counts
  characters, not visible columns).

## 5. 2D plotting (`src/ui/plot_elements/`)

`Plot` is the container; every series/annotation class below must be a
**direct child of a `Plot`**. All series/annotation classes carry a shared
`label` field (series name shown in the legend) from their common
`PlotItem` base, in addition to what's listed. None of the plotting
elements emit events.

#### `Plot`
| Field | Type | Default | Description |
|---|---|---|---|
| `title` | `string` | `"##plot"` | Plot title; prefix with `##` to hide the title text but keep a stable ID. |
| `x_label` / `y_label` | `string` | `""` | Axis labels; empty = none. |
| `width` | `float` | `-1.0` | Plot width; `-1` fills available width (-1–8192). |
| `height` | `float` | `300.0` | Plot height (16–8192). |
| `flags` | `int32` (flags) | `0` | ImPlotFlags bitmask. |
| `x_flags` / `y_flags` | `int32` (flags) | `0` | ImPlotAxisFlags per axis; use `31` (`NoDecorations`) for pie charts. |
| `x_min` / `x_max` | `float` | `0.0` | Fixed X-axis limits, locked every frame; ignored (auto-fit) when `x_min == x_max`. |
| `y_min` / `y_max` | `float` | `0.0` | Fixed Y-axis limits, same auto-fit-when-equal rule. |

#### `PlotLine`, `PlotScatter`, `PlotStairs`, `PlotDigital`
Share `xs`, `ys` (`float[]`, one entry per data point) — a connected line,
scatter markers, a staircase line, and a 0/1 digital signal trace,
respectively.

#### `PlotStems`
`xs`, `ys` plus `ref` (`float`, default `0.0`) — vertical stems from the
`ref` baseline to each `(xs[i], ys[i])`.

#### `PlotShaded`
`xs`, `ys` plus `ys2` (`float[]`, default `{}`, second Y array for a band
between `ys`/`ys2`; leave empty to shade between `ys` and `ref` instead) and
`ref` (`float`, default `0.0`).

#### `PlotBars` / `PlotBarsH`
`xs` (positions; empty = placed at `0,1,2,...`), `ys` (heights/lengths),
`bar_size` (`float`, default `0.67`, width/height of each bar, 0–16).
`PlotBarsH` draws horizontal bars (length = `xs[i]`, position = `ys[i]`).

#### `PlotHistogram`
| Field | Type | Default | Description |
|---|---|---|---|
| `values` | `float[]` | `{}` | Sample values to bin. |
| `bins` | `int32` | `-1` | `-1`=Sturges, `-2`=Scott, `-3`=Rice, `-4`=Sqrt, `>0`=explicit count. |
| `cumulative` | `bool` | `false` | Render a cumulative distribution. |
| `density` | `bool` | `false` | Normalize bars to a probability density. |
| `range_min` / `range_max` | `float` | `0.0` | Sample range to include; equal values (default) auto-range. |

#### `PlotHistogram2D`
`xs`, `ys` (paired samples, must match in length) plus `x_bins`, `y_bins`
(`int32`, default `-1`, same sentinel values as `PlotHistogram.bins`) — a 2D
frequency heatmap of the sample pairs.

#### `PlotHeatmap`
| Field | Type | Default | Description |
|---|---|---|---|
| `values` | `float[]` | `{}` | Flattened row-major data, size `rows × cols`. |
| `rows` / `cols` | `int32` | `1` | Grid dimensions. |
| `scale_min` / `scale_max` | `float` | `0.0` / `1.0` | Value range mapped to the colormap. |
| `format` | `string` | `"%.1f"` | printf format for per-cell labels; empty disables labels. |
| `x_min`/`x_max`/`y_min`/`y_max` | `float` | `0.0`/`1.0`/`0.0`/`1.0` | Heatmap bounds in plot coordinates. |

#### `PlotPieChart`
| Field | Type | Default | Description |
|---|---|---|---|
| `labels` | `string` | `""` | Newline-separated slice labels, one per `values` entry. |
| `values` | `float[]` | `{}` | Slice magnitudes. |
| `x` / `y` | `float` | `0.5` | Pie center in plot units [0,1] (0–1). |
| `radius` | `float` | `0.4` | Pie radius in plot units (0–1). |
| `normalize` | `bool` | `false` | Normalize slices to sum to 1. |
| `label_fmt` | `string` | `"%.1f%%"` | printf format for per-slice labels. |
| `angle0` | `float` | `90.0` | Start angle in degrees (0° = 3 o'clock), 0–360. |

Place inside a `Plot` with `x_flags`/`y_flags` set to `31` and axis limits
`[0,1] x [0,1]` for correct scaling.

#### `PlotText`
`text` (`string`), `x`/`y` (plot coordinates), `offset_x`/`offset_y`
(pixel offset after projection) — a text label at a plot coordinate.

#### `PlotInfLines`
`values` (`float[]`, axis positions) plus `horizontal` (`bool`, default
`false`) — infinite reference lines; vertical at each X value by default,
or horizontal at each Y value when `horizontal: true`.

## 6. 3D plotting (`src/ui/plot3d_elements/`)

`Plot3D` is the container; series/mesh/annotation classes must be direct
children. They share a `label` field via `Plot3DItem`. No events.

#### `Plot3D`
| Field | Type | Default | Description |
|---|---|---|---|
| `title` | `string` | `"##plot3d"` | Plot title; `##` prefix hides the title bar. |
| `x_label`/`y_label`/`z_label` | `string` | `""` | Axis labels. |
| `width` | `float` | `-1.0` | Plot width; `-1` fills width (-1–8192). |
| `height` | `float` | `400.0` | Plot height (16–8192). |
| `flags` | `int32` (flags) | `0` | ImPlot3DFlags bitmask. |
| `x_flags`/`y_flags`/`z_flags` | `int32` (flags) | `0` | ImPlot3DAxisFlags per axis. |

#### `Plot3DLine` / `Plot3DScatter`
`xs`, `ys`, `zs` (`float[]`, one entry per point) — a connected line or
unconnected markers through 3D points.

#### `Plot3DSurface`
| Field | Type | Default | Description |
|---|---|---|---|
| `xs`/`ys`/`zs` | `float[]` | `{}` | Flattened `x_count × y_count` grid coordinates/heights (row-major). |
| `x_count`/`y_count` | `int32` | `2` | Grid dimensions (2–4096). |
| `scale_min`/`scale_max` | `float` | `0.0` | Z-to-colormap range; both `0` triggers auto-range. |

#### `Plot3DTriangle` / `Plot3DQuad`
`xs`, `ys`, `zs` — flat vertex arrays whose length must be a multiple of 3
(triangles) or 4 (quads); each consecutive group forms one filled shape.

#### `Plot3DMesh`
`xs`, `ys`, `zs` (per-vertex coordinates) plus `indices` (`int32[]`, flat
list of vertex indices, length a multiple of 3, one triangle per group) —
an indexed triangle mesh.

#### `Plot3DText`
`text`, `x`/`y`/`z` (anchor position), `angle` (rotation degrees, -360–360),
`offset_x`/`offset_y` (pixel offset after projection) — a text label at a
3D coordinate.

## 7. Built-in forms (higher-level, not raw widgets)

These are pre-built composite dialogs/apps with server-side logic, not
plain layout primitives — instantiate them directly rather than
reconstructing their internals. Full detail (internal structure, save
semantics, error handling) is in `src/ui/forms/DESIGN.md`; summarized here:

#### `FileDialog`
A modal file picker (Open or Save As, via `confirm_label`).

- Fields: `title` (default `"Open File"`), `files` (list of `{name, type}`,
  client repopulates on navigation), `filename` (two-way), `filters` (list
  of `{label, regex?}`), `confirm_label` (default `"Open"`, set `"Save"`
  for Save As), `path` (two-way, editable path bar).
- Events: `on_open` `{path}`, `on_navigate` `{name, type: "dir"|"file"|"path"}`,
  `on_cancel` (no payload).

#### `Nano`
A multi-file, syntax-highlighted text editor (module, off by default —
`WISH_MODULE_BDG_DESKTOP_NANO`). One closable `TabItem` per open file; a
tab's label gets a `" *"` suffix while it has unsaved changes. Each tab also
has its own language `Combo` above the editor, seeded from the file's
extension but freely overridable — picking a language only changes
highlighting and does not mark the file dirty.

- Methods: `open_file(path, title?)`, `confirm_close(save)`.
- Events: `on_request_open`, `on_request_new`, `on_file_opened {path,title}`,
  `on_file_closed {path}`, `on_file_saved {path}` (Ctrl+S, or the "Save"
  button for the active tab), `on_confirm_close {paths}` (window closed with
  unsaved files — call `confirm_close(save)` with the user's answer, or
  don't call it at all to cancel the close and leave the window open),
  `on_error {message}`, `closed`.

#### `ObjectInspector`
Unlike the forms above, `ObjectInspector` is a plain nestable `Element` (an
ordinary tree child, not a modal/top-level dialog) — but like them, its
content is server-side-built, not something you construct field-by-field.
Set its `target` field to a `dynamic_ptr` and call its `set_target` method
(a plain `set()` on `target` alone does **not** rebuild it) to reflect over
`target`'s registered class and render a two-column field table plus a
description panel — the Unity/Visual-Studio-style property inspector. Full
detail (the field → widget dispatch table, the `Hidden`/`Order`/
`ColorField`/`Multiline`/`DropTarget` attributes it reads, and why it needs
`set_target()` rather than self-populating) is in
[docs/object-inspector.md](object-inspector.md).

#### `Top`
A read-only top/htop-style monitor (module, off by default —
`WISH_MODULE_BDG_DESKTOP_TOP`).

- Method: `update_snapshot(cpu_percent, per_core_percent, mem_total_bytes,
  mem_used_bytes, processes[])`.
- Events: `closed`.

## 8. The `editor` tool: previewing a UI from a JSON file

wish ships a live JSON UI mock editor as an optional module
(`WISH_MODULE_BDG_DEV_EDITOR`, off by default) — the fastest way to iterate
on a hand-written or agent-generated UI template without writing any client
code or doing a build/re-run cycle for every change.

### Build and run

```sh
cmake -S . -B build -DWISH_MODULE_BDG_DEV_EDITOR=ON
cmake --build build --target wish-standalone   # or wish-client / wish-cli

# Edit (and create, if missing) a local JSON file:
build/app/wish-standalone --run=editor -- path/to/ui.json
```

`wish client --run=editor -- path/to/ui.json` works the same way against a
separately-running `wish server`. `standalone` is the simplest choice for
solo iteration since it needs no separate server process.

### What it shows

A window split into:
- a **filename label** showing the local path and a `[MODIFIED]` marker
  once there are unsaved edits,
- a syntax-highlighted **source panel** (a `TextEditor` bound to the JSON
  file) — edit here directly,
- an **error banner**, shown only when the current source fails to parse
  (invalid JSON or an unknown element `type`); the last successfully-parsed
  preview stays on screen unchanged while the banner is up, so a syntax typo
  never blanks the preview,
- a live **preview window** — the actual instantiated UI the JSON
  describes, re-parsed and swapped in on every edit,
- an **event log** table — every event any preview widget fires is appended
  as `"<dot-path> <event> {payload}"`, e.g. `"row.ok clicked"` — useful for
  confirming a widget's events/payloads match this document without writing
  a client at all.

### Editing workflow

1. Type JSON in the source panel; the preview re-parses and updates on
   every keystroke (a failed parse only updates the error banner, per
   above).
2. Interact with the live preview (click buttons, drag sliders, ...) — each
   interaction appends a row to the event log, letting you verify field
   names, event names, and payload shapes empirically.
3. Press **Ctrl+S** inside the source panel to persist changes back to the
   local file on disk (in-editor edits update the preview immediately but
   are **not** written to disk until saved — matching wish's nano save
   contract).
4. If the local file changes outside the tool (e.g. another process/agent
   edits it), the editor picks up the change and re-parses automatically.
5. Closing the window with unsaved edits shows an inline confirmation
   (Save & Close / Discard & Close / Cancel) instead of closing immediately.

### Why this is useful for an AI agent

Because the preview and event log are driven by the exact same importer and
renderer that any real wish client uses, the editor is a fast way to
validate a hand-authored template's syntax and behavior — confirm which
widgets rendered, which fields took effect, and which events an interaction
produced — before wiring up real application logic behind it. For an
end-to-end automated check (screenshots, programmatic clicks, assertions on
widget state) instead of a human-driven preview, use the automation module
described in `CLAUDE.md`'s "Automation" section.

## See also

- [`CLAUDE.md`](../CLAUDE.md) — automation-driven UI debugging/testing
  workflow, and the security rules for file-accessing widget fields.
- [`docs/ai-assisted-development.md`](ai-assisted-development.md) — the
  `wish-module`/`wish-ui` AI-agent skills that build on this catalog.
- [`src/ui/forms/DESIGN.md`](../src/ui/forms/DESIGN.md) — full detail on
  built-in forms (§7 above) and the form/session architecture.
- [`DESIGN.md`](../DESIGN.md) — overall architecture, object model, and how
  to add a new UI element class.
