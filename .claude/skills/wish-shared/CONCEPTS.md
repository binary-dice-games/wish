# wish shared concepts

Background shared by the `/wish-module` and `/wish-ui` skills, kept in one
place to avoid duplication. Read this once per skill invocation; it does not
replace reading `README.md` / the relevant `DESIGN.md` files, which still
have the authoritative, up-to-date detail.

## 1. Architecture in one paragraph

A `wish server` process owns the UI rendering loop and a `bison` RMI class
registry (buttons, windows, plots, ...). Client code — C++, Python, C#, or a
third party's own process via `wish_client_dll`'s C ABI — connects over a
transport (TCP, pipe, or in-process `memory_transport`) and drives the UI by
instantiating registered classes, setting/getting their fields, calling
methods, and subscribing to events. The client never touches graphics
directly; the server renders whatever tree of objects the client built.
`wish standalone` fuses server+client into one process (SDL3 window or
`--renderer web`) for single-binary demos and for automation-driven testing.

## 2. Object model and dot-path addressing

- `client.instantiate(ns, class, params)` (C++) / `wish_instantiate()` (C
  ABI) creates one remote object and returns a proxy. Proxies support
  `.get()`, `.set({...})`, `.call(method, args)`, `.onEvent(name, handler)`.
- **Templates** are a JSON/YAML description of a whole tree, registered once
  with `register_template(name, descriptor)` and instantiated (possibly
  multiple times, each under its own `prefix`) with
  `instantiate_template(name, prefix)`. The root lands at dot-path `prefix`;
  children at `prefix.<child_key>`; `proxy_get("prefix.child.sub")` resolves
  any node without a server round trip once instantiated.
- Named children in a template use whatever key you choose (`"ok"`,
  `"display"`, ...) — that key becomes the path segment.

### Template JSON schema

```json
{
  "type": "Window",
  "title": "My Window",
  "width": 400,
  "height": 300,
  "flags": "NoResize",
  "closable": true,
  "children": {
    "some_child_key": { "type": "Button", "label": "OK" }
  }
}
```

- `type` — the registered bison class name (`"Window"`, `"Button"`,
  `"Label"`, `"Plot"`, `"PlotLine"`, ...).
- Every other top-level key besides `type`/`children` is a field on that
  class — check the class's registration (`src/ui/ui_elements/*.cpp`,
  `src/ui/plot_elements/*.cpp`) or `src/ui/forms/DESIGN.md` for the field
  list, types, and defaults. Don't guess a field name — grep for
  `addField(` in the owning `.cpp` if unsure.
- `children` is a map from child key to child descriptor, recursively.

## 3. Widget catalog (where to look, not what to memorize)

- **Basic controls**: `src/ui/ui_elements/*.cpp` — `Window`, `Button`,
  `Label`, `Checkbox`, `Combo`, `InputText`/`InputNumber`, `Slider`, `Drag`,
  `RadioButton`, `Selectable`, `Table`, `Tabs`/`TabBar`/`TabItem`, `Tree`,
  `Image`, `TextEditor`, `Separator`, layouts (`HorizontalLayout`,
  `VerticalLayout`), `Docking`, `Menu`.
- **2D plotting**: `src/ui/plot_elements/*.cpp` — container class `Plot`;
  series classes (must be children of a `Plot`): `PlotLine`, `PlotScatter`,
  `PlotStairs`, `PlotStems`, `PlotShaded`, `PlotDigital` (all share `xs`/`ys`
  float-array fields — see `plot_series.cpp`), plus `PlotBars`,
  `PlotHistogram`, `PlotHeatmap`, `PlotPie`, `PlotAnnotations`.
- **3D plotting**: `src/ui/plot3d_elements/*.cpp` — `Plot3D`, `Plot3DSeries`,
  `Plot3DSurface`, `Plot3DMesh`, `Plot3DAnnotations`.
- **Reference forms** (server-side classes with real logic, not just
  layout): `FileDialog`, `Notepad`, `ProcessExplorer` — documented in full
  in `src/ui/forms/DESIGN.md` under "Built-in Forms". `Notepad` in
  particular is the canonical example of a client/server file-transfer
  handshake (`upload_file` → `open_file` → `on_file_closed` →
  `download_file`) worth reading before designing anything that touches
  local files.
- Field/event names always resolve through `DisplayName`/`Description`
  attributes attached at registration — `grep -n "DisplayName\|addField\|addMethod"`
  in the relevant `.cpp` is the fastest way to get the exact contract.

## 4. Styling

`client.set_style_preset("dark" | "light" | "classic")` (or
`wish_set_style_preset()` in the C ABI) applies a built-in per-session theme.
There is no need to hand-roll colors for a first version of a UI — apply a
preset and only reach for per-widget style fields if the request calls for
something a preset can't give.

## 5. File sandboxing (only relevant if the UI touches files)

Every session has an isolated `resource_dir`; `upload_file`/`download_file`
move bytes in/out of it. Any widget field that accepts a path (`Image::src`,
`TextEditor::file_path`, ...) is resolved through
`resolve_widget_path()`/`file_service::resolve_path()` on the server side,
which rejects absolute paths and `..` escapes by default. See README.md's
"Security Considerations for AI Code Assist" section before writing any new
file-accessing field or widget — never construct a filesystem path from
client input without going through that resolution.

## 6. Automation, for validating a UI before/while building it

Both skills lean on wish's automation module (`WISH_ENABLE_AUTOMATION`,
requires `WISH_ENABLE_WEB=ON`) to let an agent see and drive a real rendered
UI instead of reasoning about JSON blindly — see `CLAUDE.md`'s "Automation"
section and `src/automation/DESIGN.md` for full detail. In short:

```sh
cmake -S . -B build -DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON [-D... your target's other options]
cmake --build build --target <the binary that will host your UI>
pip install playwright && playwright install chromium   # skip install if already configured
sudo playwright install-deps chromium   # Linux only, one-time; installs the OS shared
                                         # libraries (libnspr4, libnss3, ...) the browser
                                         # binary above needs to actually launch -- a
                                         # separate step from downloading it. Skip if
                                         # already configured. Needs an interactive
                                         # terminal for the sudo password if not cached,
                                         # so run it yourself, not through a non-interactive
                                         # tool. Symptom if skipped: AutomationClient.launch()
                                         # fails with "error while loading shared libraries:
                                         # libnspr4.so: cannot open shared object file".
```

```python
from wish.automation import AutomationClient

with AutomationClient.launch(server_cmd=[<argv that renders the UI under test>, "--renderer", "web"]) as ui:
    ui.wait_for("() => window.wish.ready === true")
    png = ui.screenshot()
    open("mockup.png", "wb").write(png)
    tree = ui.get_tree()
```

Use `ui.screenshot()` output (read it back with the `Read` tool, which
renders images) as a **mockup for the user to look at and approve** before
writing the "real" logic behind a UI — this works just as well for a
hand-built JSON layout as for a finished app, and catches layout mistakes
(wrong widget, bad sizing, missing field) far earlier than staring at
source. `ui.get_tree()` / `ui.get_widget(path)` confirm the structure and
field values match what was intended; `ui.click()` / `ui.type_text()` /
`ui.get_logs()` let you drive interactive flows end-to-end.

Constraints to remember (see `CLAUDE.md` for the full list): one dedicated
session per launched process, loopback-only bind, a widget with `rect: null`
was never actually rendered (make it visible before asserting on it), and
only leaf-widget rects are reliable (assert on the specific interactive
widget, not its enclosing container).

**Known limitation — `ui.screenshot()` can be unusable in a GPU-less
environment.** The web renderer paints via WebGL; Playwright's headless
Chromium needs a working GL context (hardware or `--use-gl=swiftshader`) to
render it. In a sandboxed/CI environment with no GPU passthrough, the
context comes up and is immediately lost (`CONTEXT_LOST_WEBGL` in
`page.on("console")`), and every `ui.screenshot()` returns a broken-image
placeholder forever — no amount of retrying, extra `wait_for`, or Chromium
launch flags fixes this, because the GL context itself never becomes
usable. If you observe `CONTEXT_LOST_WEBGL` once, treat screenshots as
unavailable for the rest of the session rather than re-attempting; `
ui.get_tree()` / `ui.get_widget()` still work (they read the object model
over the RMI/automation channel, not the canvas) and remain useful for
confirming structure, but do not prove pixels actually painted. See each
skill's mockup step for what to do instead when screenshots are down.

## 7. Coding conventions

Follow this repo's `CLAUDE.md` style guide for anything written in C++
(license header, `@file`/`@brief` doxygen comments, 2-space indent,
`bdg::wish::...` namespaces, trailing-underscore private members,
`bison::synchronized<T>` over raw mutexes for any shared state). Match
Python/C#/YAML/JSON style to sibling files in the same directory rather than
inventing new conventions.
