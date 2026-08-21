# Automation Module — Architecture & Design

**Status: implemented.** This document was the implementation brief this
pass followed; see "Implementation notes" at the end of this document for
the small set of places the actual implementation had to go beyond (or
adjust) what's written below, and why. For agent-facing usage (how to use
this to debug or test a wish app), see `CLAUDE.md`'s "Automation: debugging
and testing a wish UI" section.

This document primarily covers the original, **browser-driven** automation
path (Playwright against the web renderer). A second, independent
implementation of the same feature set was added later for the **SDL3
renderer**, built directly into the wish C ABI instead of a browser — see
"Native (ABI-based) automation for the SDL3 renderer" below.

## Purpose

wish has no way for an external tool — an AI agent, or an automated e2e
test — to observe or drive the UI it renders. There is no widget-tree
query API, no input-injection entry point beyond ImGui's own internal test
hooks (see `tests/test_imgui_renderer.cpp`), no screenshot capability, and
no way to see the application's own log output correlated with what it was
doing at the time — anywhere in the codebase. The automation module closes
that gap: it lets a Python script (or an AI agent driving one) query the
live widget tree (names, classes, screen positions), inject mouse/keyboard
input, pull a screenshot, and receive the session's `logger` output live as
it happens, so it can control a wish UI the way Playwright controls a
browser and "see" the result of each action — including log lines a click
or keystroke caused. The same primitive serves two audiences:

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
- The **genuinely new pieces** are a **tree/hit-test query API** and a
  **live log feed**, both riding the protocol `web_renderer` already
  speaks:
  - The UI is a `<canvas>` (WebGL2), not real DOM, so Playwright cannot
    `page.click("#ok")` a wish widget the way it would a web page.
    Automation adds a way to ask "what widgets exist, and where are they
    on screen" (`QUERY_TREE`/`TREE_SNAPSHOT`).
  - `logger` (`src/context/logger.hpp`) already exists for application log
    output, but had no path out to an external observer. Automation adds
    `LOG_EVENT`: every `logger::log()` call is pushed to the browser as it
    happens, so a script sees log lines land in sequence with its own
    actions (e.g. "click a button, then observe the log entry it caused")
    instead of having to reconcile a separately-parsed log file against
    UI state after the fact.

This scope keeps the new C++ surface small: no new renderer class, no new
socket/port, no new third-party dependency in the server, and — unlike an
earlier draft of this design that proposed a bespoke libuv-based
control-plane socket — **no change to `extern/bison` at all**. (This
reasoning is specific to the web renderer's browser-driven path described
in this section; the SDL3 renderer has no browser to extend, so its own
automation support — added later, see "Native (ABI-based) automation" below
— takes a different approach: new RMI methods on the existing connection,
rather than a browser.)

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
       │      [+ WISH_AUTOMATION_ENABLED: render_node override + hit_test_map_ +
       │       implements automation::automation_backend -- see "Native
       │       (ABI-based) automation" below]
       └─ web_renderer    (existing headless + civetweb HTTP/WebSocket backend)
              [+ WISH_AUTOMATION_ENABLED: render_node override + hit_test_map_]
```

No new renderer class is introduced. `WISH_ENABLE_AUTOMATION` adds a small,
narrowly-scoped `#ifdef WISH_AUTOMATION_ENABLED` block directly to
`web_renderer` (and, for the native path, `sdl3_renderer`) — consistent
with this repo's stated preference for a narrow in-file guard over a
parallel class/file split when the delta is small (`CLAUDE.md`, Platform
Support section, states this principle for platform differences; the same
reasoning applies to a small optional-capability delta).

`WISH_ENABLE_AUTOMATION` **requires** `WISH_ENABLE_WEB=ON` and/or
`WISH_ENABLE_SDL3=ON` — automation has no renderer to extend without at
least one of the two backends that implement it.

### The two-socket-plus-browser model

```
   app-under-test (e.g. bc)          Python (agent or pytest)
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
src/automation/automation_backend.hpp      renderer-side automation interface for the
                                            native (ABI-based) path -- see below
src/automation/automation_service.hpp/.cpp per-session RMI service ("__WishAutomation")
                                            forwarding to automation_backend -- native path
src/automation/DESIGN.md                   this document

# Existing files with small, #ifdef-guarded additions:
src/web/web_renderer.hpp/.cpp              render_node override + hit_test_map_ +
                                            last_broadcast_log_seq_ watermark
src/web/draw_protocol.hpp/.cpp             QUERY_TREE / TREE_SNAPSHOT / LOG_EVENT
                                            message types
src/sdl/sdl3_renderer.hpp/.cpp             render_node override + hit_test_map_ +
                                            implements automation_backend -- native path
src/context/logger.hpp/.cpp                log_entry struct + bounded recent_logs()
                                            ring buffer, appended to by log()
src/context/context.hpp/.cpp               automation_service member; find_singleton_service()
                                            throws for "__WishAutomation" when unset
src/client/client.hpp/.cpp                 automation_proxy_ + get_automation_tree()/
                                            take_screenshot()/inject_*() -- native path
include/wish_client_c.h, src/wish_client_c.cpp  wish_automation_* C ABI functions -- native path
resources/embedded/web/client.js           window.wish JS shim (tree queries +
                                            window.wish.logs)
src/server/renderer.hpp                    service_automation_queries() virtual hook (no-op
                                            default; web_renderer overrides it) +
                                            as_automation_backend() hook (native path)
src/server/registry.cpp                    register_automation_service() -- native path
src/server/server.cpp                      render_loop() calls the hook per session;
                                            on_session_created() attaches automation_service
src/standalone/standalone.cpp              same calls, standalone's own render_loop()

# New, gated tests:
tests/test_automation_query.cpp            mirrors test_web_renderer.cpp's
                                            real-socket, no-browser-required style
tests/test_automation_service.cpp          automation_service's RMI plumbing against a
                                            fake automation_backend -- native path,
                                            renderer-agnostic
tests/test_sdl3_automation.cpp             sdl3_renderer's automation_backend
                                            implementation -- native path
bindings/python/tests/test_native_automation.py  e2e smoke test against a real
                                            `wish server --renderer sdl3` -- native path
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
      last_resolved_rect_min_, last_resolved_rect_max_,  // see "Container/window rects" below
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
context.hpp` — and then, for every element found there, recursively
walking its own `"children"` field for descendants `ui_objects` has no
entry for (rows/tabs a form appends at runtime via direct `children` map
assignment rather than `import_json`'s named-node path — the pattern
`Top`'s process table, `nano`'s file tabs, and `tail`'s
log rows and tag tabs all use; see `collect_unregistered_descendants()`
in `automation_query.cpp` for the full rationale and how such a
descendant's dot-path is synthesized from its sequential child index —
joined against `web_renderer::hit_test_map_` by
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

## Log event protocol

`logger` (`src/context/logger.hpp`) is wish's existing per-session RMI
service: application code calls `client.log_info(...)` etc., and the
server writes each message to stdout/a log file. That's fine for a human
watching the terminal, but useless to an automation script, which has no
way to read the server's stdout and, even if it did, would still have to
reconstruct which UI state was current when each line was logged. The
automation module closes that gap by pushing every log call straight to
the browser, live, instead of leaving it to be reconstructed after the
fact.

**Push, not pull — this is deliberately not symmetric with QUERY_TREE.** A
tree snapshot only makes sense "as of right now, on request" (the tree
constantly changes; there is no meaningful history to ask for). Log
entries are exactly the opposite: they're discrete events whose *order*
relative to the automation script's own actions (click, then observe the
log line that click caused) is the entire point. Modeling that as a
request/response query would force the script to poll and reconstruct
ordering itself; a live push preserves it for free — `window.wish.logs`
grows in exactly the order `logger::log()` was called, interleaved
correctly with whatever the script did in between reads. This also means
no `visible_windows`-style UI-state snapshot needs to travel with each log
entry: the automation script *is* the one driving the UI, so it already
knows what it just did immediately before observing the log line.

One new message type, server → browser only (no browser → server request
exists for this):

| Direction | Type | Payload |
|---|---|---|
| server → browser | `0x22 LOG_EVENT` | JSON `{"logs": [{"seq": N, "timestamp": "...", "level": "...", "message": "..."}, ...]}` |

`logger::log()` (every call, whether it came from an RMI `log` call or a
direct server-side C++ call) appends a `logger::log_entry` — `seq`
(monotonically increasing, assigned by `logger`, never reused),
`timestamp`, `level`, `message` — to a bounded ring buffer,
`logger::recent_logs_` (capped at `kMaxRecentLogs` = 200; oldest dropped
first). This happens inside `logger::log()` itself, under the same mutex
that already serializes writes to stdout/the log file — no new
synchronization primitive.

Delivery is driven from the same place QUERY_TREE is answered:
`web_renderer::service_automation_queries()`, called once per session per
rendered frame. Each call compares `s.logger_service->recent_logs()`
against `web_renderer::last_broadcast_log_seq_` (a render-thread-only
watermark, the highest `seq` already sent) and broadcasts anything newer,
in one `LOG_EVENT` message, to every currently-connected browser via
`civetweb_server::broadcast()` — then advances the watermark. A frame with
nothing new logged sends nothing. Because `on_after_dispatch`
(`src/server/server.cpp`) already marks *every* session dirty after *any*
RMI dispatch — including the `log` RMI method itself — a `log()` call is
guaranteed to trigger a render within one frame interval (the ~16ms cap in
`server::render_loop`), so delivery is near-real-time without any special-
casing beyond the existing dirty-tracking the render loop already does.

On the browser side, `client.js`'s `window.wish.logs` array accumulates
every `LOG_EVENT`'s entries as they arrive (`window.wish.getLogs()` is a
plain synchronous accessor — a shallow copy of that array — not a
`Promise`, since there is no round trip: the data is already local by the
time a script asks for it). A script correlates cause and effect the same
way it would with any event log: read the array length, take an action,
`wait_for` the length to grow, inspect what got appended:

```python
before = len(ui.get_logs())
ui.click("dialog.ok")
ui.wait_for(f"() => window.wish.logs.length > {before}")
assert ui.get_logs()[-1]["message"] == "saved"
```

---

## Render on demand

By default the render loop (`server::render_loop()`/`standalone::render_loop()`)
draws continuously whenever the session is dirty, which routine WS/input
activity keeps triggering — every OS event, every automation query, the
initial post-connect settle window. Fine for wish's own ImGui-only
`web_renderer`, but a project embedding wish for something GPU-heavier
(e.g. genie's `web_deferred_renderer`, whose frames are real WebGL2 draws
executed by a possibly software-rendered browser) pays that draw cost for
every one of those frames, most of which no automation script is actually
looking at yet. `renderer::render_on_demand()` (an opt-in virtual on the
base `renderer` interface, see `src/server/renderer.hpp`) exists to make
that cost pay-per-use instead.

**Tick/draw decoupling is the prerequisite.** Turning off automatic drawing
must not also pause whatever time-based state the app owns (a live game's
simulation should keep running "as in production" whether or not anything
is watching) — so `renderer::tick(sessions)`, a new hook distinct from
`render_server_frame()`, is called every `render_loop()` iteration at a
steady ~60 Hz cadence *unconditionally*, regardless of whether a frame
actually draws that iteration. A renderer with time-based state overrides
`tick()` (not `render_server_frame()`, which is only ever called
immediately before drawing, inside an active ImGui frame, and is meant for
host-chrome UI) for that state advancement — see genie's `DESIGN.md`
("Steady-cadence manager tick, decoupled from drawing") for a concrete
example (`update_session()`).

**What `render_on_demand()` actually gates.** When a renderer returns
`true`: routine `poll_events()` activity (WS traffic, resize, OS input) no
longer sets `pending_render_`, and the end-of-frame `wants_continuous_redraw()`
re-arm is skipped too. `context::dirty` itself is untouched — still checked
every iteration exactly as before — so a genuinely dirty session (RMI
dispatch, the initial post-connect settle window) still draws normally.
The only new, explicit trigger is `renderer::request_render()` /
`consume_render_request()`: a renderer-owned pending flag (not
`context::dirty`, since transport callbacks like `on_message()` typically
run on a worker thread with no session context in hand) that `render_loop()`
checks unconditionally every iteration and folds into `pending_render_`
regardless of `render_on_demand()`.

**Wire trigger: `0x23 REQUEST_RENDER`** (browser → server, empty payload,
gated the same as QUERY_TREE/TREE_SNAPSHOT/LOG_EVENT). Decoded in
`web_renderer::on_message()` (mirrored identically in genie's
`web_deferred_renderer::on_message()`), it calls `request_render()`
directly — no session context needed. `window.wish.requestRender()`
(`client.js`) sends it and returns a `Promise` resolved once the resulting
`FRAME` has actually been rendered to the canvas (the same
`scheduleFrameRender()` rAF callback that flips `window.wish.ready = true`
also pops and resolves the oldest pending `requestRender()` call, FIFO).
`AutomationClient.request_render()` (Python) wraps this. A QUERY_TREE
request also calls `request_render()` internally server-side (see that
handler in `on_message()`) so `get_tree()`/`get_widget()` are always
answered against fresh state without the caller having to think about it;
`screenshot()` does **not** do this automatically (it's a pure
Playwright/CDP call, no server round trip) — a script that wants a
guaranteed-fresh screenshot under `render_on_demand()` must call
`request_render()` itself immediately before `screenshot()`.

**Native SDL3 automation needs none of this wire plumbing.** `sdl3_renderer`'s
own automation ABI (see "Native (ABI-based) automation" below) already runs
in-process with direct access to `standalone::request_render()`/
`context::dirty`, and already calls it before every `query_tree()`/
`capture_screenshot()` — so a renderer that opts into `render_on_demand()`
there (genie's `sdl3_gpu_renderer`/`sdl3_gl_renderer` both take the same
constructor flag) benefits purely from the `poll_events()`-activity
suppression above; no new message type or `consume_render_request()`
override is needed on that path.

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
  logs: [],               // grows as 0x22 LOG_EVENT envelopes arrive

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

  getLogs() {
    return this.logs.slice();  // synchronous -- no round trip, see below
  },
};
// on receiving a 0x21 TREE_SNAPSHOT envelope:
//   const { resolve } = window.wish._pending.get(msg.request_id);
//   window.wish._pending.delete(msg.request_id);
//   resolve(msg);
// on receiving a 0x22 LOG_EVENT envelope (pushed, no request_id to resolve):
//   window.wish.logs.push(...msg.logs);
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
socket, no browser required, asserting on `QUERY_TREE`/`TREE_SNAPSHOT`/
`LOG_EVENT` envelope round-trips, on `automation::build_tree_snapshot()`'s
output against a hand-built `ui_element` tree, on `logger::recent_logs()`'s
ordering/`seq`/cap behavior, and on `service_automation_queries()`
broadcasting each new log entry exactly once (not resending what an
earlier call already sent).

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
    ui.wait_for("async () => (await window.wish.getWidget('status.label'))?.text === 'Saved'")

    before = len(ui.get_logs())
    ui.click("form.save")
    ui.wait_for(f"() => window.wish.logs.length > {before}")
    assert ui.get_logs()[-1]["message"] == "saved"
```

`window.wish.getTree()`/`getWidget()` are `Promise`-returning calls (see
the JS shim below); `wait_for()`'s predicate may itself be `async` --
Playwright's `page.wait_for_function()` already awaits a returned
`Promise` on every poll, so an `async` predicate that calls `getWidget()`
works with no extra plumbing on the Python side. `getLogs()` is different:
synchronous, no `Promise`, since log entries are pushed and buffered
client-side as they happen (see "Log event protocol" above) rather than
fetched on demand.

`AutomationClient.launch(...)`:
1. Starts the given `server_cmd` (typically `wish server --renderer web`) as
   a subprocess, appending `--web_port <port>` for a Python-chosen free port
   if the command doesn't already specify one (see "Implementation notes"
   for why this is a Python-side port pick rather than an ephemeral
   `--web_port 0` whose actual bound port would need to be parsed back out
   of the subprocess), and waits for that port to accept connections — or
   accepts an already-running `url=` for attaching to a manually-started
   server during interactive agent use.
2. Launches a headless Chromium via `playwright.sync_api.sync_playwright()
   .chromium.launch(headless=True)`, navigates to the server's URL.
3. Waits for `window.wish.ready`.
4. Exposes `get_tree()`, `get_widget(path)`, `get_logs()`, `click(path)`,
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
already-installed browser). On Linux, that one-time setup is actually two
separate steps, and only the first is obvious from the package name: the
browser binary (`playwright install chromium`) and the OS-level shared
libraries it links against at launch (`sudo playwright install-deps
chromium` — `libnspr4`, `libnss3`, and similar). Skipping the second on a
fresh machine/container produces a `libnspr4.so: cannot open shared object
file` error from `AutomationClient.launch()` that doesn't obviously point
back to Playwright. That is judged acceptable here because:

- It replaces writing and maintaining a server-side rasterizer or a new
  bison transport primitive with code that already exists and is already
  battle-tested (an entire browser engine).
- Claude Code's own execution environment ships Chromium with Playwright
  pre-configured (`PLAYWRIGHT_BROWSERS_PATH`/`PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD`
  already set up) — the primary stated use case for this module.
- It inherits Playwright's own auto-waiting/retry facilities for e2e
  assertions "for free," rather than wish needing to invent them.

---

## Native (ABI-based) automation for the SDL3 renderer

Everything above this section is the **browser-driven** path: it only ever
reaches the web renderer, since Playwright needs a page to drive. The SDL3
renderer (`src/sdl/sdl3_renderer.hpp/.cpp`) opens a real native window —
there is no browser tab, no WebSocket, no `client.js` to extend — so an
agent debugging or e2e-testing an SDL3 wish app had no automation surface
at all. This section describes the second, independent implementation of
the same feature set (tree/hit-test queries, screenshots, input injection,
log access) built for that renderer, added directly to the wish **C ABI**
(`include/wish_client_c.h` / `wish_client_dll`) instead of a browser.

### Why the ABI instead of another WebSocket protocol

The browser path's wire protocol (`QUERY_TREE`/`TREE_SNAPSHOT`/`LOG_EVENT`
on `web_renderer`'s civetweb HTTP/WebSocket port) is inherently tied to
having a browser-served page and a second socket a browser tab connects
to — neither exists for a windowed SDL3 app. Rather than inventing a
parallel WebSocket-like control channel for a native renderer, native
automation rides the **same RMI connection** a script already uses to
build/control the session's UI: it is implemented as ordinary RMI methods
on a new per-session service object, `automation_service`
(`"__WishAutomation"`), exposed through new `wish_automation_*` functions
in `wish_client_c.h`. This has a real ergonomic upside over the browser
model: a Python script driving an SDL3 app needs only **one**
`wish.Client` connection for both building the UI and automating it —
unlike the browser path's two-client pattern (an RMI client to build the
UI, a separate Playwright-driven browser to observe/drive it), which exists
only because a browser tab has no RMI access of its own.

### Architecture

```
Python script (wish.Client, ctypes)
   │  get_tree() / click() / screenshot() / ...
   ▼
wish_client_c.cpp ("── Automation ──" section)
   │  wish::client::get_automation_tree() / take_screenshot() / inject_*()
   ▼
automation_proxy_ (RMI proxy, resolved like style_proxy_/fs_proxy_)
   │  over the existing RMI connection
   ▼
automation_service ("__WishAutomation", per-session RMI object)
   │
   ├─ inject_mouse_move/button/key(): forwards directly to the backend --
   │  synchronous, no queueing (SDL_PushEvent() is thread-safe)
   │
   └─ get_tree() / get_logs() / screenshot(): forwards to the backend and
      blocks the calling RMI dispatch thread on a std::future until the
      render thread services the request
   ▼
automation::automation_backend (src/automation/automation_backend.hpp)
   implemented by sdl3_renderer
```

`automation::automation_backend` is a small interface
(`query_tree`/`capture_screenshot`/`inject_mouse_move`/`inject_mouse_button`/
`inject_key`/`inject_text`) that a renderer implements to plug into
`automation_service`. `renderer::as_automation_backend()`
(`src/server/renderer.hpp`) is a new virtual hook, analogous to
`service_automation_queries()`, that returns non-null only for a renderer
that implements it; `server::on_session_created`/
`standalone::on_session_created` check it and attach an `automation_service`
only when it does. `sdl3_renderer` is the only implementer today, but the
interface itself has no SDL3 dependency — a future renderer could implement
it too.

### Reused vs. new code

The tree/hit-test snapshot and log-event JSON builders
(`automation::build_tree_snapshot`/`build_log_event` in
`src/automation/automation_query.hpp/.cpp`) are pure logic with no web
dependency, so `sdl3_renderer` reuses them **unchanged** — it just supplies
its own `wish::ui_tree`/`hit_test_map` inputs instead of `web_renderer`'s.
Likewise, hit-test rect resolution (`last_resolved_rect_min_/max_`) already
lives in the shared `imgui_renderer` base class (populated once per
`render_node()` call, before either subclass's own dispatch), so
`sdl3_renderer::render_node()`'s override is a near-verbatim copy of
`web_renderer::render_node()`'s — read the base-resolved rect plus
`ImGui::IsItemHovered/Active/Visible()`, no new resolution logic.

Genuinely new, SDL3-specific code (`src/sdl/sdl3_renderer.hpp/.cpp`, all
`#ifdef WISH_AUTOMATION_ENABLED`):

- **Screenshot capture**: `capture_frame_png()` calls
  `SDL_RenderReadPixels()` on the current frame (right after draw data is
  submitted to `sdl_renderer_` in `end_frame()`, but before
  `SDL_RenderPresent()` — the last point the pixels are guaranteed
  readable), normalizes the surface to RGBA8888 via `SDL_ConvertSurface()`,
  and PNG-encodes it with the vendored `stb_image_write.h` (`extern/stb`,
  already used for the read side by `web_renderer`'s texture loader).
  `capture_screenshot()` (the `automation_backend` method) just queues a
  `std::promise<std::vector<uint8_t>>`, fulfilled by `end_frame()` the next
  time it runs.
- **Tree/hit-test queries**: `render_node()` populates `hit_test_map_`
  exactly like `web_renderer` does; `query_tree()` queues a
  `std::promise<std::string>`, fulfilled by a new
  `service_automation_queries(const context&)` override (the same hook
  `web_renderer` already implements) using the reused
  `build_tree_snapshot()`.
- **Input injection**: `inject_mouse_move/button/key()` build a plain
  `SDL_Event` and call `SDL_PushEvent()` directly from the calling (RMI
  dispatch) thread — SDL's event queue is documented thread-safe, so this
  needs no cross-thread hand-off; the render thread's normal
  `poll_events()`/`SDL_PollEvent()`/`ImGui_ImplSDL3_ProcessEvent()` path
  picks it up next frame, indistinguishable from real hardware input.
  `inject_text()` is the one exception: `SDL_TextInputEvent::text` is a
  `const char*` whose lifetime a synthetically-pushed `SDL_Event` can't
  safely own, so injected text instead queues a plain `std::string`,
  drained at the top of `begin_frame()` (before `ImGui::NewFrame()`) via
  `ImGuiIO::AddInputCharactersUTF8()` directly — bypassing SDL's event
  queue for this one case only.

### Python surface

Unlike the browser path's dedicated `wish.automation.AutomationClient`
(which manages its own Playwright browser/subprocess lifecycle), native
automation is exposed as ordinary methods directly on `wish.Client`
(`bindings/python/wish/client.py`) — `get_tree`/`get_widgets`/`get_widget`/
`click`/`type_text`/`drag`/`screenshot`/`get_logs`/`wait_for`, matching
`AutomationClient`'s method names/semantics so a script can switch renderers
with minimal changes. `click`/`type_text`/`drag` compute widget centers
client-side from a parsed `get_tree()` snapshot and compose the low-level
`mouse_move`/`mouse_button`/`text_input` calls, mirroring
`AutomationClient`'s own `_widget_center()` + Playwright-composition logic.
`wait_for()` takes a plain Python callable (there is no JS engine to
delegate a string predicate to, unlike `page.wait_for_function()`).

### Not available: `MenuBar`'s rect gap

The one known hit-test gap noted in "Hit-test capture mechanism" below
(`MenuBar`'s rect can reflect stale layout state) applies equally here,
since it originates in the shared `imgui_renderer` base, not `web_renderer`.

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

Native (ABI-based) automation carries the identical elevated-sensitivity
caveat, just via a different transport: since it rides the same RMI
connection every other `wish_client_c.h` call uses, anyone who can connect
to a server's RMI transport (`--transport=tcp --port=N`) already gets full
tree introspection and input injection the moment that server's active
renderer implements `automation::automation_backend` — there is no separate
opt-in flag for it. The same "no authentication exists in bison or wish
today" caveat applies.

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
| **Log delivery** | Server pushes `LOG_EVENT`; no browser-initiated query exists | A log line's *position relative to the automation script's own actions* is the point (click, then observe the log it caused) — a pull/query model would force the script to poll and reconstruct that ordering itself, and would need a UI-state snapshot bundled into each entry to substitute for it. Push preserves ordering for free, since the script is already the one driving the UI and therefore already knows what state it's in. |

---

## Implementation notes

A few places the implementation had to add to, or make a concrete choice
beyond, what's specified above:

- **`renderer::service_automation_queries()` hook.** `build_tree_snapshot()`
  needs a session's `context::ui_objects` *and* `web_renderer::hit_test_map_`
  at the same time, under the session's lock — but `web_renderer` itself
  never sees a `context` outside the transient scope of one `render_node()`
  call, and only `server::render_loop`/`standalone::render_loop` hold both
  the per-session `context_wlock` and a reference to the active `renderer`
  at once. Rather than special-case the web renderer in those render loops
  (an `#ifdef WISH_WEB_ENABLED`/`dynamic_cast` in otherwise
  renderer-agnostic code), a new no-op-by-default virtual,
  `renderer::service_automation_queries(const context&)`, was added to
  `src/server/renderer.hpp` alongside the existing `render_session`/
  `render_node` hooks; only `web_renderer` overrides it. `server.cpp` and
  `standalone.cpp` each call it once per session, right after that
  session's `render_session()` calls, while still holding the session's
  context write-lock.
- **Class name resolution.** Every `register_*()` in `src/ui/ui_elements/*.cpp`
  already attaches a `DisplayName` attribute to its own class's `CLASS`
  field, equal to the class name itself (e.g. `button.cpp` attaches
  `DisplayName("Button")` to `"Button"_key`) — this was already there for
  `bison::build_display_dict()`'s existing consumers (trace/property-editor
  output), and `build_tree_snapshot()` reuses it directly for the `class`
  field rather than inventing a second name registry. A class hash with no
  such attribute (defined outside `src/ui/ui_elements/`) falls back to its
  hash formatted as `"0x########"`.
- **Runtime-appended children.** `context::ui_objects` only gets an entry
  for a *named* JSON node (`import_json`'s `build_ui_node()` stamps
  `"__path__"` on named children only) — a form that reconciles a live
  list against a `Table`/`TabBar` by writing directly into an existing
  element's `"children"` field (`(*children_field)[index] = dynamic_ptr{elem}`,
  never a named dot-path) produces children `ui_objects` never sees, even
  though the real renderer draws them fine via
  `ui_element::for_each_child_ordered()`. `build_tree_snapshot()` closes
  this gap with `collect_unregistered_descendants()`: for every element it
  finds via `ui_objects`, it also recurses into that element's `"children"`
  field looking for `ui_element` children with no `"__path__"` field, and
  synthesizes a dot-path for each as `<parent_path>.<child_key.id>` — safe
  because every module using this pattern keys its appended children with a
  plain sequential `size_t` index (`dynamic::operator[](size_t)` stores at
  `fields_[static_cast<hash_t>(pos)]` directly), so `child_key.id` is
  already a small, stable, human-meaningful index rather than a name hash.
  Recursion continues into a discovered child's own `"children"` field too,
  so a runtime-appended `TableRow`'s `TableColumn`/`Label` cells are found
  the same way. A child that *does* carry `"__path__"` is skipped — it is
  one of `ui_objects`' own entries and will be walked when the outer loop
  reaches it directly, so recursing into it here would only duplicate work.
- **Content field probing.** `label`/`text`/`value`/`title`/`checked`/
  `selected`/`hint` (the actual lowercase field-key spellings used across
  `src/ui/ui_elements/*.cpp`) are probed via `dynamic::findField<T>()` for
  `T` in `{string, bool, int32_t, float}` and copied through under their own
  literal name when present — not resolved via `build_display_dict()` (whose
  entries are human-facing display names like `"Label"`, not the
  wire-visible field key `"label"`).
- **`render_node()`'s visibility guard.** The base `imgui_renderer::render_node()`
  early-returns (draws nothing, calls no ImGui widget function) for a node
  with `visible=false`. `web_renderer::render_node()` checks the same
  `visible` field before capturing `GetItemRect*()`, so an invisible node
  doesn't get a stale, unrelated widget's rect attributed to it.
- **Container/window rects are now accurate.** `hit_test_map_` no longer
  reads `ImGui::GetItemRectMin/Max()` directly; it reads
  `imgui_renderer::last_resolved_rect_min_/max_` (`src/imgui/imgui_renderer.hpp`),
  which the base class resolves once per `render_node()` call. For a class
  whose render function recurses into children (`VerticalLayout`/
  `HorizontalLayout`, `TabBar`/`TabItem`, `TreeNode`, `CollapsingHeader`,
  `Table`/`TableRow`, `Plot`/`Plot3D`), the dispatch call is wrapped in
  `ImGui::BeginGroup()`/`EndGroup()`, so the resolved rect is the bounding
  box of everything the container drew, not just its last child. `Window`/
  `DockSpaceViewport` open a genuine new top-level window a group can't see
  into, so they self-report via `ImGui::GetWindowPos()/GetWindowSize()`
  instead (hidden `__wish_win_rect_*__` fields stamped right after their own
  `Begin()`/`BeginPopupModal()`). Leaf widgets are deliberately left
  unwrapped — wrapping every class unconditionally was tried first and
  broke `GetItemID()`/`IsItemActive()` for idle widgets (`EndGroup()`
  unconditionally reassigns `g.LastItemData.ID` to 0 unless the group
  currently contains the active/deactivated id), so only classes that
  actually recurse are wrapped. `MenuBar`/`Menu`/`MenuButton` remain a
  known, narrower gap: `BeginMenuBar()` internally resets its own layout
  bookkeeping in a way no outer group can see through, and `Menu`/
  `MenuButton`'s own visible identity (the trigger label/button in the
  current window) is already a correct leaf rect on its own — their actual
  children render inside a separate floating popup a group around the
  current window can't describe anyway. See `CLAUDE.md`'s "Automation"
  section for the agent-facing version of this note.
- **`AutomationClient` picks its own port.** Rather than passing
  `--web_port 0` (ephemeral) and parsing the server subprocess's stdout to
  discover which port it actually bound — fragile, and `wish server` prints
  no such line today — `AutomationClient.launch()` asks the OS for a free
  port itself (a short-lived probe socket, same technique `web_renderer`
  uses server-side for `--web_port 0`) and passes it explicitly via
  `--web_port <port>`, unless `server_cmd` already specifies one.
