# examples/ui — editor example UIs

One standalone UI descriptor per tab of the `demo` app
(`examples/demo/main.cpp`), in both formats:

| Directory | Files |
|-----------|-------|
| `json/` | `basics` · `sliders` · `inputs` · `selection` · `tree` · `misc` · `tables` · `plots` · `plot3d` · `files` · `forms` · `icons` (`.json`) |
| `yaml/` | the same twelve, as `.yaml` |

Each file's root is a `Window` (the demo wraps the same content in a
`TabItem`), so it loads on its own in the **`editor` module**:

```sh
cmake -S . -B build -DWISH_MODULE_BDG_DEV_EDITOR=ON
cmake --build build --target wish-standalone

build/app/wish-standalone --run=editor -- examples/ui/json/basics.json
build/app/wish-standalone --run=editor -- examples/ui/yaml/tables.yaml
```

The editor picks JSON vs. YAML from the file extension; both formats get
the same syntax highlighting, inline autocomplete, and cursor-tracked
field-reference Help panel.

Notes:

- The `json/` and `yaml/` versions of a file are the same tree — the YAML
  is generated from the JSON.
- `plots` / `plot3d` series carry no `xs`/`ys` data and `files` / `icons`
  reference session resources that don't exist outside a real `demo`
  session — the demo client fills those in at runtime. These previews show
  layout and structure, not live data.
- See [docs/ui-elements.md](../../docs/ui-elements.md) for the full widget
  catalog and template schema, and
  [docs/examples.md](../../docs/examples.md) for the `demo` app itself.
