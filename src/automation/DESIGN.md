# Automation Module — Architecture & Design

**Status: design only — not yet implemented.** This document is the
implementation brief for a future pass; no code in this directory exists
yet. Do not treat any type or file path below as already present until it
has actually been created.

## Purpose

wish has no way for an external tool — an AI agent, or an automated e2e
test — to observe or drive the UI it renders. There is no widget-tree
query API, no input-injection entry point beyond ImGui's own internal test
hooks (see `tests/test_imgui_renderer.cpp`), and no screenshot capability
anywhere in the codebase. The automation module closes that gap: it lets a
Python script (or an AI agent driving one) query the live widget tree
(names, classes, screen positions), inject mouse/keyboard input, and pull a
screenshot, so it can control a wish UI the way Playwright controls a
browser and "see" the result of each action. The same primitive serves two
audiences:

1. **Live AI-agent control** — an agent queries the tree, decides what to
   click or type, observes the result, repeats.
2. **Jest-like e2e testing** — a Python test harness drives an app
   deterministically and asserts on widget state or a screenshot.

This is an **optional, compile-time feature** gated by the CMake option
`WISH_ENABLE_AUTOMATION`, off by default.

Critically, this is **not a new renderer, a new network port, or a new
wire protocol**. `web_renderer` (`src/web/`) is already a fully headless
backend — no window, no GPU — that serves a browser-based ImGui display
over HTTP + WebSocket. The environment this module is designed for (Claude
Code) ships a headless Chromium with Playwright pre-installed specifically
for this kind of UI-driving task. Automation is therefore built as a thin
extension of `web_renderer` plus a small Python client that drives that
existing page with Playwright:

- **Screenshots** = Playwright's `page.screenshot()` against the page
  `web_renderer` already serves. Zero new C++ rasterization code.
- **Input** (mouse/keyboard) = Playwright's native `page.mouse.*` /
  `page.keyboard.*` APIs, dispatching real DOM events at the canvas.
  `client.js` **already** forwards DOM mouse/keyboard events as `INPUT`
  WebSocket messages (`src/web/DESIGN.md`, "Browser Client" section) —
  this is the exact path a real human browser session already exercises.
  **No new input-handling code is needed anywhere.**
- The **one genuinely new piece** is a **tree/hit-test query API**: the UI
  is a `<canvas>` (WebGL2), not real DOM, so Playwright cannot
  `page.click("#ok")` a wish widget the way it would a web page. Automation
  adds exactly this — a way to ask "what widgets exist, and where are they
  on screen" — as two new message types on the protocol `web_renderer`
  already speaks.

This scope keeps the new C++ surface small: no new renderer class, no new
socket/port, no new third-party dependency in the server, and — unlike an
earlier draft of this design that proposed a bespoke libuv-based
control-plane socket — **no change to `extern/bison` at all**.

An earlier draft of this design also considered rendering screenshots via
a new offscreen SDL3 render target (`SDL_RenderReadPixels`) or a
hand-written software rasterizer over `ImDrawData`. Both were rejected in
favor of the Playwright approach: both would have required real new
rendering code for a problem `web_renderer` + a real browser already solve
correctly, and the SDL3 path would have added a GPU/video-driver
dependency to what should be the most headless-friendly of wish's
renderers. See "Design Trade-offs" at the end of this document.

---

## Architecture

### Class Hierarchy

```
renderer            (abstract, src/server/renderer.hpp)
  └─ imgui_renderer (headless ImGui dispatch, src/imgui/imgui_renderer.cpp)
       ├─ sdl3_renderer   (existing windowed backend)
       └─ web_renderer    (existing headless + civetweb HTTP/WebSocket backend)
              [+ WISH_AUTOMATION_ENABLED: render_node override + hit_test_map_]
```

No new renderer class is introduced. `WISH_ENABLE_AUTOMATION` adds a small,
narrowly-scoped `#ifdef WISH_AUTOMATION_ENABLED` block directly to
`web_renderer` — consistent with this repo's stated preference for a
narrow in-file guard over a parallel class/file split when the delta is
small (`CLAUDE.md`, Platform Support section, states this principle for
platform differences; the same reasoning applies to a small optional-
capability delta).

`WISH_ENABLE_AUTOMATION` **requires** `WISH_ENABLE_WEB=ON` — automation has
no meaning without the web renderer's browser/WebSocket surface to extend.

### The two-socket-plus-browser model

```
   app-under-test (e.g. calculator)          Python (agent or pytest)
        │  existing RMI client                     │  playwright
        │  (TCP / pipe / memory_transport,          │
        │   completely unchanged)                   │
        ▼                                            ▼
  ┌─────────────────────────────────────────────────────────┐
  │                     wish server                          │
  │   --renderer web --web_port <ephemeral>                  │
  │                                                            │
  │   RMI transport port  ◄── app's own client                │
  │   civetweb HTTP/WS port ◄── headless Chromium (Playwright)│
  │        │                                                   │
  │        └─ web_renderer: FRAME / TEX_* (unchanged)          │
  │                       + QUERY_TREE / TREE_SNAPSHOT (new)   │
  └─────────────────────────────────────────────────────────┘
```

The app-under-test connects exactly as it does today — automation is
blind to which app is running; it only ever sees "whatever `ui_element`
tree is in the one connected session's `context`." The Playwright-driven
headless Chromium is the *only* thing that changes: it is both the
display client and, via the new query messages, the introspection client.
Because the [session model decision](#session-model) below scopes
automation to one dedicated session per run, there is exactly one browser
talking to exactly one `web_renderer` instance — no multi-browser/shared-
`ImGuiContext` complications to reason about.

### Key Files

```
src/automation/automation_query.hpp/.cpp   tree/hit-test snapshot struct,
                                            builder, JSON serialization
                                            (pure logic, no networking —
                                            mirrors how draw_protocol.hpp
                                            is a pure, network-independent
                                            codec split out of web_renderer)
src/automation/DESIGN.md                   this document

# Existing files with small, #ifdef-guarded additions:
src/web/web_renderer.hpp/.cpp              render_node override + hit_test_map_
src/web/draw_protocol.hpp/.cpp             QUERY_TREE / TREE_SNAPSHOT message types
resources/embedded/web/client.js           window.wish JS shim

# New, gated test:
tests/test_automation_query.cpp            mirrors test_web_renderer.cpp's
                                            real-socket, no-browser-required style
```

---

## Session model

Automation runs as its **own dedicated, single-session headless process**:
a fresh `wish server --renderer web --web_port <ephemeral>` is launched per
test/agent-run, exactly like a fresh browser context is launched per
Playwright test. This was chosen over attaching automation to an
already-running production server with other live sessions, because
`server::render_loop` currently renders every connected session through
one shared, global `ImGuiContext` (`src/server/server.cpp`) — isolating one
session's screenshots and hit-test data from others rendering in the same
frame would be materially harder, and unnecessary for the two audiences
this module targets (an agent driving one instance it just started; a test
harness that wants a clean instance per test). Attaching to a live,
multi-session server is a possible future extension if wish ever moves to
per-session `ImGuiContext`s, but is out of scope here.

---

## Hit-test capture mechanism

To answer "where is this widget on screen," `web_renderer` needs to record
each widget's screen rect as it draws. This is added as a single override,
gated by `WISH_AUTOMATION_ENABLED`:

```cpp
// src/web/web_renderer.hpp (sketch)
#ifdef WISH_AUTOMATION_ENABLED
 protected:
  void render_node(const ui_element& node, const context& s) override;

  struct hit_test_entry {
    ImVec2 rect_min;
    ImVec2 rect_max;
    bool hovered;
    bool active;
    bool item_visible;
  };
  std::unordered_map<bison::key_t, hit_test_entry, bison::key_t, bison::key_t> hit_test_map_;
#endif
```

```cpp
// src/web/web_renderer.cpp (sketch)
#ifdef WISH_AUTOMATION_ENABLED
void web_renderer::render_node(const ui_element& node, const context& s) {
  imgui_renderer::render_node(node, s);  // unchanged dispatch/recursion
  auto id = node.as<bison::key_t>("__wish_id"_key);
  if (!id) return;
  hit_test_map_[*id] = {
      ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
      ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::IsItemVisible(),
  };
}
#endif
```

This is exactly the wrap-and-capture shape already proven safe in
`tests/test_imgui_renderer.cpp`'s `counting_imgui_renderer` (calls the base
`render_node`, observes state around it, without touching dispatch
internals). It requires **no change to any of the ~20 individual
`render_*` widget functions** in `src/imgui/imgui_ui_renderer.cpp` — the
capture happens once, centrally, for every node.

`hit_test_map_` is rebuilt fresh every frame (cleared in `begin_frame()`
when `WISH_AUTOMATION_ENABLED`) so a query always reflects the most
recently *completed* frame, never a stale or partially-rendered one.

**Coordinate space**: `web_renderer` already treats browser canvas pixels
as `ImGuiIO::DisplaySize` 1:1 — `client.js` maps DOM mouse events straight
into `INPUT` messages with no DPI or transform layer in between (see
`src/web/DESIGN.md`, "Threading Model" / INPUT handling). Rects captured
via `GetItemRectMin/Max()` are therefore **directly usable as Playwright
screen coordinates** (`page.mouse.click(x, y)`) with no conversion.

---

## Tree/hit-test query protocol

Two new message types are added to the existing envelope defined in
`src/web/draw_protocol.hpp` (`uint8 msg_type, uint8[3] reserved, uint32
payload_len, payload`, little-endian — unchanged):

| Direction | Type | Payload |
|---|---|---|
| browser → server | `0x20 QUERY_TREE` | JSON `{"request_id": N, "root": "<dot-path or empty>"}` |
| server → browser | `0x21 TREE_SNAPSHOT` | JSON `{"request_id": N, "widgets": [...]}` |

Unlike `FRAME`/`TEX_*`, which are packed binary for per-frame throughput,
`TREE_SNAPSHOT` uses JSON: queries are interactive/occasional (an agent
deciding its next action, a test assertion), not a 60-times-a-second hot
path, so simplicity and debuggability win over compactness here. The
existing envelope already supports an arbitrary payload per `msg_type`, so
mixing a JSON control message into an otherwise-binary protocol costs
nothing structurally. Serialization reuses `nlohmann::json`, already
vendored (`extern/bison/extern/json/single_include`) and already used by
`src/ui/ui_descriptor.cpp` — no new JSON dependency.

Each entry in `widgets` (built by `automation::build_tree_snapshot()` in
`src/automation/automation_query.cpp`, walking `context::ui_objects` —
the existing flat dot-path → `ui_element_ptr` map, `src/context/
context.hpp` — joined against `web_renderer::hit_test_map_` by
`__wish_id`) has the shape:

```json
{
  "path": "dialog.ok",
  "class": "Button",
  "label": "OK",
  "rect": { "x0": 120.0, "y0": 84.0, "x1": 180.0, "y1": 104.0 },
  "hovered": false,
  "active": false,
  "visible": true
}
```

`label`/`text`/`value`/`checked` (whichever fields are meaningful for that
widget's class) are copied from the element's own `bison::dynamic` fields
— no schema duplication, same "no separate schema system" principle the
root `DESIGN.md` already states for the UI tree itself.

The request handler runs on the render thread at a defined point (same
spot `web_renderer::begin_frame()` already drains `input_queue_`/
`pending_resize_`) — it reads `hit_test_map_` and `context::ui_objects`
under the session's existing `context_rlock` (`src/context/context.hpp`),
never mid-render. `request_id` lets `client.js` resolve the right pending
`Promise` if multiple queries are in flight (uncommon in practice, since
Python-driven scripts issue one query at a time and `await` it, but cheap
to make correct).

---

## Screenshots and input via Playwright

No new server-side code is needed for either capability — both ride on
`web_renderer`'s existing display/input surface, driven from the Python
side:

- **Screenshot**: `page.screenshot()`. Real, pixel-perfect PNG bytes of
  exactly what the browser rendered, with zero new C++.
- **Click**: resolve a dot-path to a rect via the query API, compute its
  center, call `page.mouse.click(cx, cy)` — a real DOM/CDP mouse event,
  forwarded by `client.js`'s existing listeners into an `INPUT` WebSocket
  message exactly as a human user's click would be.
- **Type text**: click the target `InputText`-like widget to focus it,
  then `page.keyboard.type(text)` — again, the existing DOM→`INPUT`
  forwarding path, unmodified.
- **Readiness**: `client.js` sets `window.wish.ready = true` once it has
  processed its first `FRAME` message (new, small addition alongside the
  `window.wish` shim below). Python waits for this before interacting:
  `page.wait_for_function("() => window.wish.ready === true")`.
- **Auto-waiting for assertions**: `page.wait_for_function(...)` polling a
  JS predicate that itself calls `window.wish.getWidget(path)` gives
  Playwright-native auto-waiting for e2e assertions (e.g. "wait until
  `status.label`'s text is `Saved`") — no bespoke Python polling loop
  needed.

### `window.wish` JS shim (added to `resources/embedded/web/client.js`)

```js
window.wish = {
  ready: false,
  _pending: new Map(),   // request_id -> {resolve, reject}
  _nextId: 1,

  getTree(root = "") {
    const id = this._nextId++;
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject });
      sendQueryTree(id, root);  // encodes/sends the 0x20 QUERY_TREE envelope
    });
  },

  async getWidget(path) {
    const snap = await this.getTree(path);
    return snap.widgets.find(w => w.path === path) ?? null;
  },
};
// on receiving a 0x21 TREE_SNAPSHOT envelope:
//   const { resolve } = window.wish._pending.get(msg.request_id);
//   window.wish._pending.delete(msg.request_id);
//   resolve(msg);
```

This is the only piece Playwright needs beyond its own APIs — no second
socket, no extra Python package beyond `playwright` itself.

---

## CMake Integration

```cmake
option(WISH_ENABLE_AUTOMATION
    "Build the automation query API on top of the web renderer \
     (headless UI automation for AI agents / e2e tests via Playwright)" OFF)
```

```cmake
if(WISH_ENABLE_WEB AND WISH_ENABLE_AUTOMATION)
  target_sources(wish_server PRIVATE
    src/automation/automation_query.hpp  src/automation/automation_query.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_AUTOMATION_ENABLED)
endif()
```

`WISH_ENABLE_AUTOMATION=ON` with `WISH_ENABLE_WEB=OFF` is a configure-time
error (automation has no renderer to extend without it) — mirrors how
`WISH_ENABLE_WEB` itself is guarded on `WISH_ENABLE_IMGUI`.

No new FetchContent dependency: `nlohmann::json` is already vendored via
bison, and civetweb/WebSocket transport is already brought in by
`WISH_ENABLE_WEB`. This is the direct payoff of building on `web_renderer`
instead of a new pipeline — the superseded SDL3-offscreen draft would have
added an `SDL_TEXTUREACCESS_TARGET`/hidden-window dependency, and the
superseded libuv-control-plane draft would have required a new
`extern/bison` primitive (propose → approve → implement → bump submodule,
per `CLAUDE.md`'s "The bison library" section) before any wish-side code
could be written at all.

`docs/building.md`'s CMake-options table gets a new row for
`WISH_ENABLE_AUTOMATION`, and a short "Running automation" subsection
alongside the existing "Running the web renderer" one.

### Tests

`tests/test_automation_query.cpp`, gated the same way
`tests/test_web_renderer.cpp` is: real WebSocket connections over a real
socket, no browser required, asserting on `QUERY_TREE`/`TREE_SNAPSHOT`
envelope round-trips and on `automation::build_tree_snapshot()`'s output
against a hand-built `ui_element` tree.

---

## Python client library

New, standalone module — does **not** reuse `bindings/python/wish`'s
ctypes/`wish_client_dll` machinery, since the whole point of building on
`web_renderer` is that no native library is required on the Python side.

`bindings/python/wish/automation.py`:

```python
from wish.automation import AutomationClient

with AutomationClient.launch(server_cmd=["wish", "server",
                                          "--renderer", "web"]) as ui:
    tree = ui.get_tree()
    ui.click("dialog.ok")
    ui.type_text("form.name_input", "Ada Lovelace")
    png_bytes = ui.screenshot()
    ui.wait_for("() => window.wish.getWidgetSync('status.label')?.text === 'Saved'")
```

`AutomationClient.launch(...)`:
1. Starts `wish server --renderer web --web_port <ephemeral>` as a
   subprocess (or accepts an already-running `url=` for attaching to a
   manually-started server during interactive agent use).
2. Launches a headless Chromium via `playwright.sync_api.sync_playwright()
   .chromium.launch(headless=True)`, navigates to the server's URL.
3. Waits for `window.wish.ready`.
4. Exposes `get_tree()`, `get_widget(path)`, `click(path)`,
   `type_text(path, text)`, `screenshot()`, `wait_for(js_predicate)` as
   thin wrappers over `page.evaluate()`/`page.mouse`/`page.keyboard`/
   `page.screenshot()`/`page.wait_for_function()`.
5. On context-manager exit: closes the browser, then terminates the
   server subprocess (if it launched one).

### Test harness layer

`bindings/python/wish/automation_testing.py` (or a `conftest.py` fixture)
provides a pytest fixture wrapping the same launch/teardown sequence,
yielding a connected `AutomationClient` per test — the "Jest-like e2e
testing" entry point this module set out to provide. Assertions are plain
`assert` on `get_tree()`/`get_widget()` results; `screenshot()` can be
attached to a failed test's report by any existing pytest reporting plugin
— no custom assertion DSL is introduced.

### New dependency, and why it's an acceptable trade

This design's one real cost, versus a pure-stdlib `socket`+`json` client
considered earlier: the Python side now depends on the `playwright`
package (plus a one-time `playwright install chromium`, or reusing an
already-installed browser). That is judged acceptable here because:

- It replaces writing and maintaining a server-side rasterizer or a new
  bison transport primitive with code that already exists and is already
  battle-tested (an entire browser engine).
- Claude Code's own execution environment ships Chromium with Playwright
  pre-configured (`PLAYWRIGHT_BROWSERS_PATH`/`PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD`
  already set up) — the primary stated use case for this module.
- It inherits Playwright's own auto-waiting/retry facilities for e2e
  assertions "for free," rather than wish needing to invent them.

---

## Security note

`--web_bind` already defaults to `127.0.0.1` (loopback-only) for the web
renderer. The automation query API inherits that default unchanged, and
this document explicitly flags it as **more sensitive** than plain
display: it grants full tree introspection and — via the same WebSocket
connection any browser tab already has — input injection into the
session. It must never be exposed on `--web_bind 0.0.0.0` in a shared or
untrusted network without additional authentication, which does not exist
in bison or wish today (see `src/auth/DESIGN.md` for the closest related,
still-unimplemented, design).

---

## Design Trade-offs

| Decision | Choice | Rationale |
| --- | --- | --- |
| **Rendering backend** | Extend existing `web_renderer` | Already fully headless (no GPU/window). Avoids a new renderer class, a new SDL3-offscreen dependency, and a new software rasterizer — all considered and rejected in earlier drafts of this design. |
| **Screenshots** | Playwright `page.screenshot()` against a real headless Chromium | Real, pixel-perfect output with zero new C++ rasterization code; Claude Code's environment ships Chromium/Playwright pre-installed for exactly this purpose. Trade-off: adds a non-stdlib Python dependency and browser-driven timing to reason about, versus a hypothetical pure in-process pipeline. |
| **Input injection** | Playwright's native mouse/keyboard APIs over the existing canvas | `client.js` already forwards DOM input events as `INPUT` WebSocket messages — reuses the exact code path a real human user's browser session already exercises. Zero new input-handling code anywhere in wish. |
| **Tree/hit-test query transport** | Two new message types on the existing WebSocket protocol (`draw_protocol.hpp`) | No new port, no new bison primitive, no new Python-side networking dependency (`websockets` package) — Playwright's `page.evaluate()` calls straight into a JS shim that speaks the existing socket. |
| **`TREE_SNAPSHOT` payload format** | JSON, not packed binary | Queries are interactive/occasional, not the 60fps hot path `FRAME`/`TEX_*` serve — simplicity and debuggability win; reuses `nlohmann::json`, already vendored via bison. |
| **Hit-test capture placement** | Central `render_node` override in `web_renderer` | Zero changes to any of the ~20 individual widget `render_*` functions in `imgui_ui_renderer.cpp`; mirrors the exact wrap-and-capture shape `tests/test_imgui_renderer.cpp`'s `counting_imgui_renderer` already proves safe. |
| **Widget targeting** | Existing dot-path names (`context::ui_objects`) | Already the stable, human-readable identifier scheme wish maintains; no new "testid" field needed on any widget class. |
| **Session model** | Dedicated single-session process per run | Matches Playwright's own "fresh context per test" model; sidesteps the current shared-global-`ImGuiContext` limitation across multiple sessions in `server::render_loop`. Attaching to a live multi-session server is a possible future extension, out of scope here. |
