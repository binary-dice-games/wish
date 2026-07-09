# ImNodeFlow Node Graph Editor — Design

## Purpose

Add interactive node graph editing to wish using [ImNodeFlow](https://github.com/Fattorino/ImNodeFlow). Clients define nodes and pins as bison RMI objects; the wish server renders the graph each frame, emits events when links are created or removed, and supports save/load of node positions and connections to a sandboxed file.

## New Elements

| Class | Parent | Purpose |
|---|---|---|
| `NodeEditor` | `Element` | Hosts the ImNodeFlow grid; renders the full editor canvas |
| `Node` | `Element` | One node in the graph; has title, position, and children (pins + optional wish UI body) |
| `NodeInputPin` | `Element` | An input pin on a Node |
| `NodeOutputPin` | `Element` | An output pin on a Node |

**Client usage sketch:**
```cpp
auto editor = c.instantiate("wish", "NodeEditor").get();
editor.set({{"width", 800}, {"height", 600}}).get();

auto node = c.instantiate("wish", "Node").get();
node.set({{"title", "Add"}, {"pos_x", 100.f}, {"pos_y", 100.f}}).get();

auto pin_a = c.instantiate("wish", "NodeInputPin").get();
pin_a.set({{"name", "A"}, {"data_type", "float"}}).get();
// … add pins and optional body-content wish elements as children of node,
//     then node as child of editor …

editor.onEvent("link_created", [](bison::dynamic p) {
  // p["from_node_key"], p["from_pin_name"], p["to_node_key"], p["to_pin_name"]
});

// Trigger save: set save_path; renderer writes the file and clears the field.
editor.set({{"save_path", "graph.json"}}).get();
// Trigger load: set load_path; renderer reads positions/links and clears the field.
editor.set({{"load_path", "graph.json"}}).get();
```

---

## Directory Layout

```
src/flow/
├── DESIGN.md                            ← this file
├── node_elements.hpp                    ← registration function declarations
├── node_editor.cpp                      ← NodeEditor prototype
├── node.cpp                             ← Node prototype
└── node_pin.cpp                         ← NodeInputPin + NodeOutputPin prototypes

src/imgui/
└── imgui_node_editor_renderer.cpp       ← WishNode, NodeEditorState, render_node_editor
```

---

## Element Fields

### `NodeEditor`

| Field | Type | Default | Notes |
|---|---|---|---|
| `width` | `int32_t` | `800` | Canvas width in pixels |
| `height` | `int32_t` | `600` | Canvas height in pixels |
| `save_path` | `string` | `""` | Set to trigger save; cleared after write |
| `load_path` | `string` | `""` | Set to trigger load; cleared after read |

**Events emitted:**
- `link_created` / `link_removed` — payload: `from_node_key` (key_t), `from_pin_name` (string), `to_node_key` (key_t), `to_pin_name` (string)

### `Node`

| Field | Type | Default | Notes |
|---|---|---|---|
| `title` | `string` | `"Node"` | Displayed in the node header bar |
| `pos_x` | `float` | `0.0` | Written back by renderer each frame |
| `pos_y` | `float` | `0.0` | Written back by renderer each frame |
| `width` | `float` | `100.0` | ImNodeFlow node width hint |

**Events emitted:**
- `moved` — payload: `x` (float), `y` (float); throttled (only when delta > 0.5 px)

### `NodeInputPin` / `NodeOutputPin`

| Field | Type | Default | Notes |
|---|---|---|---|
| `name` | `string` | `""` | Pin label shown by ImNodeFlow |
| `data_type` | `string` | `"any"` | Informational; actual C++ pin type is always `WishPinValue` |

---

## Key Design Decisions

### Universal pin type (`WishPinValue`)

ImNodeFlow connects pins only if their C++ template type matches. Rather than mapping `data_type` strings to separate C++ types (which would require template instantiations and complicate the generic `WishNode`), all pins use a single empty struct:

```cpp
struct WishPinValue {};
```

This means any pin can connect to any other visually. Type validation (checking `data_type` compatibility) is the client's responsibility, done in the `link_created` event handler. This keeps the server-side generic while allowing rich client-side type systems.

### Trigger fields for save/load

Save and load are triggered by setting `save_path` / `load_path` string fields on `NodeEditor`. The renderer checks these each frame, performs the I/O, then clears the field. This avoids exposing `editor_cache()` (renderer-internal state) to the bison method dispatch layer, keeping all renderer state inside the renderer translation unit — consistent with the TextEditor pattern.

### Custom JSON save/load format

ImNodeFlow's built-in save/load requires registered type factories keyed by class name. Since all nodes share the single `WishNode` type, the factory approach cannot distinguish between different node definitions. We therefore implement our own format:

```json
{
  "nodes": [{ "id": 12345678, "pos_x": 100.0, "pos_y": 200.0 }],
  "links": [{
    "from_node": 12345678, "from_pin": "Result",
    "to_node":   87654321, "to_pin":   "A"
  }]
}
```

`id` is `__wish_id.id` (uint32_t). Node type definitions (title, pins) are not saved — they live in the bison element tree and must already exist when loading. nlohmann/json is available via `extern/bison/extern/json/single_include/` (already on the PRIVATE include path for `wish_server`).

All paths go through `file_service::resolve_path(path, s.resource_dir, s.allow_absolute_paths)`.

### Node lifecycle sync

Each frame the renderer syncs bison `Node` children with ImNodeFlow grid nodes:
- **Version hashing**: compute a hash from `title` + ordered `(name, is_input)` pin pairs.
- **Create**: if no cached `WishNode` exists or version changed, call `grid.addNode<WishNode>(…)` with title, width, pin descriptors, and element's `__wish_id`.
- **Recreate**: on version mismatch (pin or title changed), destroy the old node and create a new one.
- **Remove**: erase grid nodes whose corresponding `Node` child has been deleted.

### Frame-context pointers in `WishNode`

`draw()` is called from within `grid.update()`. To render wish children at that point, `WishNode` stores raw pointers set immediately before `grid.update()`:

```cpp
imgui_renderer*   renderer_ctx{};
const ui_element* element_ctx{};
const session*    session_ctx{};
```

These are valid for exactly one frame (render loop holds the session read lock). They are cleared after `grid.update()` returns as a safety measure.

### Link diffing

After `grid.update()`, the renderer iterates all input pins across all cached nodes, builds a `current_links` set of `LinkKey` structs, diffs against `prev_links` from the previous frame, and emits `link_created` / `link_removed` events via `enqueue_event` for each delta. `prev_links` is then replaced with `current_links`.

---

## Internal Types

```cpp
// All pins share this type so any pin connects to any other.
struct WishPinValue {};

// Pin descriptor — read from children at node creation time.
struct PinDesc { std::string name; bool is_input; };

// Stable identifier for a link (canonical: from output → to input).
struct LinkKey {
  bison::key_t from_node;
  std::string  from_pin;
  bison::key_t to_node;
  std::string  to_pin;
  bool operator==(const LinkKey&) const = default;
};

// Per-editor persistent state stored in a static cache keyed by __wish_id.
struct NodeEditorState {
  ImFlow::ImNodeFlow grid;
  std::unordered_map<uint32_t, std::shared_ptr<WishNode>> node_map;   // wish_id → node
  std::unordered_map<uint32_t, size_t>                    node_version;
  std::unordered_set<LinkKey, LinkKeyHash>                 prev_links;
  explicit NodeEditorState(const std::string& label) : grid(label) {}
};
```

The grid label is `"##ne_<wish_id>"` to give each editor instance a unique ImNodeFlow internal context. Because `ImFlow::ImNodeFlow` is not default-constructible, the cache uses a `find`+`emplace` idiom rather than `operator[]`.

---

## Build Integration

### `CMakeLists.txt` changes (inside `if(WISH_ENABLE_IMGUI)`)

1. Add `FetchContent_Declare(imnodeflow GIT_REPOSITORY https://github.com/Fattorino/ImNodeFlow.git GIT_TAG main GIT_SHALLOW TRUE)`.
2. Add `imnodeflow` to `FetchContent_MakeAvailable(…)`.
3. Add `${imnodeflow_SOURCE_DIR}/src/ImNodeFlow.cpp` to `imgui_core` sources; add `${imnodeflow_SOURCE_DIR}/include` to `imgui_core` public include dirs. (Verify path against fetched layout; fall back to root if the repo uses a flat structure.)
4. Add new unconditional `wish_server` sources: `src/flow/node_editor.cpp`, `src/flow/node.cpp`, `src/flow/node_pin.cpp`.
5. Inside the `if(WISH_ENABLE_IMGUI)` `target_sources` block, add `src/imgui/imgui_node_editor_renderer.cpp`.

### `src/server/registry.cpp`

```cpp
#include "flow/node_elements.hpp"
// …
register_node_editor();
register_node();
register_node_pins();
```

### `src/imgui/imgui_ui_renderer.hpp`

```cpp
void render_node_editor(imgui_renderer& r, const ui_element& node, const session& s);
// Node/pin children are rendered inside WishNode::draw() / init(); stubs needed for dispatch.
void render_node_noop(imgui_renderer&, const ui_element&, const session&);
void render_pin_noop(imgui_renderer&, const ui_element&, const session&);
```

### `src/imgui/imgui_renderer.cpp` dispatch table

```cpp
{"NodeEditor"_key.id,    render_node_editor},
{"Node"_key.id,          render_node_noop  },
{"NodeInputPin"_key.id,  render_pin_noop   },
{"NodeOutputPin"_key.id, render_pin_noop   },
```

---

## Testing

New file: `tests/test_node_editor.cpp`

| Test | Checks |
|---|---|
| `NodeEditorRegistered` | `NodeEditor` instantiates; `width` default = 800 |
| `NodeRegistered` | `Node` instantiates; `title` default = "Node" |
| `NodeInputPinRegistered` | `NodeInputPin` instantiates; `name` and `data_type` defaults |
| `NodeOutputPinRegistered` | Same for `NodeOutputPin` |
| `NodeEditorRendersWithChildren` | Editor + two nodes + pins; `begin_frame` / `render_node` / `end_frame` — no crash |
| `NodeEditorNoLinkEventInitially` | Two frames, no connections → zero `link_created` events |
| `NodeEditorPositionWriteback` | After two frames, at least one node's `pos_x`/`pos_y` is set |
| `NodeEditorSaveTrigger` | Set `save_path`, render one frame → file exists with valid JSON `"nodes"` array |
| `NodeEditorLoadTrigger` | Write known-position JSON, set `load_path`, render one frame → node `pos_x`/`pos_y` updated |

Add to `tests/CMakeLists.txt` inside `if(WISH_ENABLE_IMGUI)`:
```cmake
wish_add_test(test_node_editor test_node_editor.cpp)
```
