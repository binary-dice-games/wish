# Automation: debugging and testing a wish UI

wish ships an optional automation module (`src/automation/`, gated by
`WISH_ENABLE_AUTOMATION`, requires `WISH_ENABLE_WEB=ON` and/or
`WISH_ENABLE_SDL3=ON`) that lets an agent *see and drive* a running wish UI
instead of reasoning about it purely from source: query the live widget
tree (paths, classes, field values, screen rects, hover/active/visible
state), take a real screenshot, and inject mouse/keyboard input — the wish
equivalent of Playwright driving a web page. Full protocol/architecture:
[src/automation/DESIGN.md](../src/automation/DESIGN.md).

Two independent implementations exist, one per renderer: the **web
renderer** uses a Playwright-driven headless browser (`AutomationClient`,
documented in the rest of this document below); the **SDL3 renderer** — no
browser tab to drive — exposes the identical capability set directly as
methods on `wish.Client` (`get_tree`/`click`/`type_text`/`drag`/
`screenshot`/`get_logs`/`wait_for`), riding the same connection already used
to build the UI. See [src/automation/DESIGN.md](../src/automation/DESIGN.md)'s
"Native (ABI-based) automation" section and
[docs/building.md](building.md#running-native-automation-sdl3) for the
SDL3 workflow; everything below this point describes the web-renderer path.

**Use this whenever you are debugging a UI-visible bug, verifying a fix, or
writing an e2e regression test for a wish app** — it is almost always faster
and more reliable than re-reading render code and guessing at runtime state.

This includes verifying that a client example/binding (C++, Python, C#, ...)
actually works end-to-end, not just that it compiles and runs without
throwing. "The client connected and didn't crash" is a weaker check than "the
expected widget tree rendered" — e.g. after fixing a client that failed to
`Instantiate` a form, don't stop at confirming the call no longer throws;
launch the server with `--renderer=web` (or reuse one already running) and
use `AutomationClient` to confirm the expected widgets actually appear in
`get_tree()` / a screenshot. A client-side exception disappearing can mean
the bug is fixed, or just that the failure moved somewhere silent.

**Keep this document current.** It exists so the next agent doesn't have to
re-discover the same setup pitfalls and dead ends through trial and error.
Whenever you finish a debugging/verification session that used this
workflow and you hit something not already documented here — a new
gotcha, a setup step that wasn't obvious, an environment-specific failure
mode, a better/worse way to drive some interaction — add it to this
document (or to [src/automation/DESIGN.md](../src/automation/DESIGN.md) if
it's about the underlying architecture rather than the workflow) before
you finish, even if the user didn't ask for a documentation update. A
gotcha you hit and didn't record is one the next agent will hit again.

## Known environment issues

- **SDL3 native automation can hang indefinitely** (`wish server
  --renderer sdl3` + `wish.Client`'s `get_tree()`/`screenshot()`/etc. on the
  same connection) in at least one Claude Code container environment —
  reproduced even with a trivial `instantiate()` immediately followed by
  `get_tree()` and no other traffic, using exactly the documented headless
  recipe (`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`). The
  project's own `bindings/python/tests/test_native_automation.py` smoke
  test hung identically, so this isn't specific to any one app under test.
  Root cause not confirmed live (see below), but traced to a specific,
  plausible deadlock by reading the dispatch/render-loop code:
  `automation_service`'s `get_tree`/`screenshot` RMI method handlers
  (`src/automation/automation_service.cpp`) call `backend_->query_tree(...)
  .get()`/`capture_screenshot().get()` **synchronously, inside the RMI
  method call**. Per this file's own session-threading-model notes,
  bison's `client_worker` holds the session's `synchronized<context>`
  **write** lock for the *entire* RMI dispatch — including this blocking
  `.get()` wait. That future is only fulfilled by the render loop's
  `service_automation_queries(*sess)` call (`src/sdl/sdl3_renderer.cpp`),
  which itself runs *inside* the render loop's own
  `context_wlock{*sync_ctx}` for that same session (`src/server/server.cpp`).
  A `shared_mutex` write lock is exclusive against both future readers and
  writers, so if the dispatch thread reaches its `.get()` wait first, the
  render loop can never acquire the lock it needs to service the query —
  and the query can never be serviced without that lock. Neither side can
  make progress: a real, self-inflicted deadlock, not merely a slow
  render loop. If accurate, this isn't specific to any one environment; it
  would reproduce anywhere the dispatch thread wins that race, which may
  simply be more likely under a headless/dummy driver's different timing.
  **Not fixed here** — a real fix likely means either answering `get_tree`
  synchronously from already-available data instead of waiting on a new
  render (screenshot has no equivalent option: it needs an actual GPU
  readback tied to a real frame), or restructuring the dispatch/render
  locking so an automation call's wait doesn't hold the same lock the
  render loop needs to resolve it. That's a genuine change to core
  server/render-loop concurrency, not safely done blind — verifying it
  needs a working SDL3 automation session to test against, which is
  exactly what's broken. **If you hit this, don't sink time re-debugging
  it from scratch** — switch to the web-renderer + Playwright path
  (`--renderer web`, `AutomationClient`, both below); it worked reliably
  in the same environment/session. If you do fix this, please replace
  this bullet with what actually worked.
- **`bison`'s Python bindings are pre-installed** in Claude Code's own
  environment (`extern/bison/bindings/python` is on `sys.path` via an
  editable-install `.pth` file) — `import bison` works with no path setup.
  **wish's own bindings are not** — you still need
  `sys.path.insert(0, "<repo>/bindings/python")` (or `WISH_LIB=...` if the
  shared library isn't under the default `<repo>/build/`) before `import
  wish`.
- **`pytest` is not pre-installed**, and `pip install pytest` fails under
  this environment's PEP 668 "externally managed environment" protection
  (`pip install --break-system-packages` works but modifies the shared
  system Python — avoid it for a one-off check). `playwright` **is**
  pre-installed (see Prerequisites below), just not `pytest`. To run an
  existing `*.py` test file that uses `unittest.TestCase` under the hood
  (true of every test under `bindings/python/tests/` at the time of
  writing) without installing anything, run it directly with
  `python3 -m unittest <module_name>` from its own directory instead of
  `pytest <path>`.
- **Launch the server with the Bash tool's own `run_in_background: true`**,
  not a shell `command & disown`. In at least one session, `&`/`nohup`/
  `disown` issued through the Bash tool silently failed to leave a
  discoverable, running process (the redirected log file wasn't even
  created), while `run_in_background: true` started the server reliably
  and kept it running across subsequent, separate tool calls.

## Prerequisites

```sh
cmake -S . -B build -DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON
cmake --build build --target wish-server   # or whichever target owns the app you're debugging
pip install playwright && playwright install chromium   # skip the install step if a
                                                          # Chromium is already configured via
                                                          # PLAYWRIGHT_BROWSERS_PATH (true in
                                                          # Claude Code's own environment)
```

`playwright install chromium` downloads the browser binary but not the OS
shared libraries it needs to actually launch (`libnspr4`, `libnss3`, and
similar). On a fresh Linux machine/container this is a **separate,
easy-to-miss step** — without it, `AutomationClient.launch()` fails with
something like `error while loading shared libraries: libnspr4.so: cannot
open shared object file`, not an obviously-Playwright-related error:

```sh
sudo playwright install-deps chromium   # one-time, requires apt + sudo; needs
                                         # an interactive terminal for the sudo
                                         # password if not already cached — run
                                         # it yourself rather than through a
                                         # non-interactive tool
```

Skip both install steps if a working Chromium is already configured (e.g.
via `PLAYWRIGHT_BROWSERS_PATH`, true in Claude Code's own environment) —
but if `AutomationClient.launch()` fails with a `libnspr4.so`/similar
shared-library error even though the browser binary exists, that
environment is missing the OS deps and needs `install-deps` regardless.

## Python client

`bindings/python/wish/automation.py`'s `AutomationClient` is the entry point. It
does **not** require building `wish_client_dll` (unlike `wish.Client`) —
only the C++ binary under test needs to exist.

```python
from wish.automation import AutomationClient

with AutomationClient.launch(server_cmd=["build/app/wish", "server", "--renderer", "web"]) as ui:
    tree = ui.get_tree()                 # {"request_id": N, "widgets": [...]}
    widget = ui.get_widget("dialog.ok")  # one entry by exact dot-path, or None
    ui.click("dialog.ok")
    ui.type_text("form.name_input", "Ada Lovelace")
    png_bytes = ui.screenshot()
    ui.wait_for("async () => (await window.wish.getWidget('status.label'))?.text === 'Saved'")

    # Logs are pushed live and buffered in arrival order, so an action's log
    # output can be told apart from everything logged before it:
    before = len(ui.get_logs())
    ui.click("form.save")
    ui.wait_for(f"() => window.wish.logs.length > {before}")
    assert ui.get_logs()[-1]["message"] == "saved"
```

| Method | Use it to... |
|---|---|
| `get_tree(root="")` | Dump the whole tree, or one subtree, for orientation — "what widgets exist right now, and what are their current field values?" |
| `get_widget(path)` | Read one widget's current state (`class`, `label`/`text`/`value`/`title`/`checked`/`selected`/`hint` — whichever exist, `rect`, `hovered`, `active`, `visible`) by its exact dot-path. |
| `get_logs()` | Read every application log message (`client.log_info(...)` etc., via `logger`) received so far, oldest first — `{seq, timestamp, level, message}` each. Pushed live as the app logs them, so an entry's position relative to actions this script just took (e.g. `click()`) tells you what caused it, with no timestamp cross-referencing needed. |
| `click(path, button=0)` | Click a widget's center — a real DOM/CDP mouse event, indistinguishable from a human click. `button` is `0`=left (default), `1`=right, `2`=middle, matching `wish.Client`'s SDL3-native `click()`. Raises if `path` doesn't exist or was never rendered (`rect` is `None`). |
| `type_text(path, text)` | Focus-click, then type — for `InputText`/`InputInt`/`InputFloat` etc. |
| `drag(from_path, to_path)` | Real press/move/release drag between two widgets' centers — for an element with a `drag_type` field dropped onto one with a matching `drop_type` (see `docs/ui-elements.md`'s "Drag and drop" section). Raises if either path doesn't exist or was never rendered. |
| `screenshot()` | Pixel-perfect PNG bytes of exactly what's on screen right now — attach to a bug report, or eyeball visually with the `Read` tool after writing to a file. |
| `wait_for(js_predicate)` | Block until a JS predicate is true — e.g. wait for an async operation's result to land, a dialog to close, or a new log entry to appear, before asserting. `async` predicates that call `getTree()`/`getWidget()` work directly (Playwright awaits the returned Promise on every poll). |

## Workflow: driving a form end-to-end (not just observing)

`AutomationClient`'s browser page is a pure **observer**: `window.wish` (the
shim in `resources/embedded/web/client.js`) only queries the tree, screenshots,
and injects DOM input events into whatever is already rendered — it is *not*
an RMI client and has no `instantiate()`/`set()`/object-model API. To actually
build UI (e.g. instantiate a form and set its fields) you need a **second,
separate RMI client** connected to the server's RMI transport, running
alongside the automation session:

```python
import threading
from wish import Client                    # real RMI client (needs wish_client_dll)
from wish.automation import AutomationClient  # pure observer/driver

ready = threading.Event()
def session_fn(client):
    proxy = client.instantiate("MessageBox", "wish", params={"title": "Confirm", ...})
    # MUST assign the Proxy to a variable that outlives this closure -- see
    # the premature-destroy gotcha below.
    ready.set()
    client.wait()          # blocks until client.quit() -- keeps the object alive

client = Client.tcp("127.0.0.1", 7071)      # separate RMI port, NOT --web_port
t = threading.Thread(target=lambda: client.run(session_fn), daemon=True)
t.start()
ready.wait(timeout=5)

with AutomationClient.launch(url="http://127.0.0.1:8099") as ui:  # attach, don't launch
    ui.click("__message_box_0.buttons.btn1")
```

Key points this pattern depends on:
- `wish server`/`wish-server` opens **two independent listeners**: the bison
  RMI transport (`--transport=tcp --port=N`, default transport is `term` —
  not scriptable) and the web renderer's HTTP/WebSocket (`--web_bind`/
  `--web_port`, what `AutomationClient` talks to). Launch the server yourself
  with both configured, then `AutomationClient.launch(url="http://host:web_port")`
  to **attach** rather than `server_cmd=` to launch (the latter has no way to
  also pass `--transport=tcp`).
- `wish.Client` (unlike `wish.automation`) requires `wish_client_dll` built
  and discoverable — set `WISH_LIB=/path/to/libwish_client.so` if it isn't
  under the default `<repo>/build/` (e.g. a `build-cli/` or other named build
  directory).
- Run the RMI client's `session_fn` on a background thread with `client.wait()`
  inside it (unblocked by `client.quit()`, typically from an event handler) —
  otherwise `client.run()` disconnects (and destroys every object it created)
  the instant `session_fn` returns.
- Restart the server (and delete its `imgui.ini`, at the server's CWD) between
  debugging attempts that change a `Window`'s content/size — a stale saved
  size for the same stable id can silently pin a freshly-rebuilt window to an
  old, wrong size/position (see `test_imgui_renderer.cpp`'s own "Hermetic"
  fixture comment on the identical class of bug in tests).
- **Always release the proxy and quit the client in a `finally` block**,
  even in a throwaway repro script. A script that raises partway through
  (a bad path, an unexpected rect) skips your cleanup code and leaves the
  instantiated object alive server-side; the *next* run against the same
  long-lived server then creates a second one, and with two (or more)
  overlapping `Window`s now rendering, `get_tree()` rects for the intended
  one become nonsensical (wildly wrong positions/sizes) in a way that
  looks exactly like a real layout bug — costly to misdiagnose. If you
  suspect this happened (rects that were sane on one run and are not on
  the next, with no code change in between), just restart the server
  rather than debugging the readings.
- A method argument that includes an **array of objects** (e.g. a list of
  per-row dicts for something like `update_snapshot`) can now be built
  directly via the `proxy.call(name, {...})`/`client.instantiate(...,
  params={...})` dict shortcut — `payload = {"processes": [{"pid": 1,
  ...}, {"pid": 2, ...}]}` just works. This used to raise `TypeError:
  Unsupported vector element type: <class 'dict'>` (the Python dict→
  `Dynamic` conversion only supported scalars and flat lists of scalars),
  requiring a `bison.from_json(json.dumps(payload))` roundtrip instead —
  fixed upstream in bison (`Dynamic.__setitem__` now builds a proper
  array-of-objects field for a list of dict/`Dynamic` elements, recursing
  for nested list-of-dicts fields too). If you're on an older `extern/bison`
  pin that predates this, the `from_json` roundtrip is still the fallback.

## Workflow: investigating a UI bug report

1. **Reproduce it live** instead of guessing from source: build with automation
   enabled, launch the app under `--renderer web`, connect an `AutomationClient`.
2. **Orient with a screenshot and a tree dump** — `ui.screenshot()` plus
   `ui.get_tree()` tell you what's actually on screen and what every widget's
   *current* field values are, which is often not what the code "should"
   produce if the bug is real.
3. **Drive the exact repro steps** from the bug report with `click()`/`type_text()`,
   checking `get_widget()` after each step — this pinpoints the exact
   action where state diverges from expectation, rather than staring at a
   single end-state screenshot. If the app logs anything (`client.log_info`
   etc.), check `get_logs()` after each step too — an unexpected message
   (or a missing one) right after a specific `click()` often points straight
   at the handler responsible, without needing a debugger.
4. **Correlate a widget back to source** via its `class` and dot-`path`: the
   path's last segment is the field name in whatever JSON/YAML descriptor or
   `register_template()` call defined it (e.g. `"dialog.ok"` → search for
   `"ok"` under a `"dialog"` node); `class` names the `src/ui/ui_elements/*.cpp`
   registration and `src/imgui/imgui_ui_renderer.cpp` render function
   (`render_button`, `render_checkbox`, ...) that owns its behavior.
5. **Fix, rebuild, re-run the same script** — same repro steps, same
   assertions — to confirm the fix without re-deriving the repro by hand
   each time.

## Workflow: writing an e2e regression test

Once a bug is understood, turn the repro script into a permanent test with
`wish.automation_testing` (a pytest fixture wrapping the same launch/teardown
sequence — see `bindings/python/wish/automation_testing.py`):

```python
from wish.automation_testing import make_wish_ui_fixture

wish_ui = make_wish_ui_fixture(lambda: ["build/app/wish", "server", "--renderer", "web"])

def test_saving_shows_confirmation(wish_ui):
    wish_ui.click("toolbar.save")
    assert wish_ui.get_widget("status.label")["text"] == "Saved"
```

## Gotchas

- **A widget with `rect: null` was never rendered this frame** — e.g. it's
  inside a collapsed `TreeNode`, an unopened `TabBar` tab, or a window that
  hasn't been given a chance to draw yet. Navigate to make it visible (or
  `wait_for` the tree to settle) before asserting on its rect or clicking it.
- **Container/window rects are now accurate bounding boxes**, not the
  last-rendered descendant's rect — `imgui_renderer::render_node()` wraps
  each recursing container's dispatch in `ImGui::BeginGroup()`/`EndGroup()`
  (or, for `Window`/`DockSpaceViewport`, self-reports via
  `GetWindowPos()/GetWindowSize()`) before the hit-test capture reads it —
  see "Hit-test capture mechanism" in `src/automation/DESIGN.md`. The one
  remaining gap is `MenuBar`: its rect can still reflect stale layout state
  rather than the menu bar's own strip, since `ImGui::BeginMenuBar()`
  internally resets its own layout bookkeeping in a way no wrap can see
  through — assert on the individual `Menu`/`MenuItem` inside it instead.
- **One dedicated session per launch.** Automation assumes exactly one
  connected app session per server process (see DESIGN.md's "Session model")
  — always launch a fresh server per debugging session / test rather than
  attaching to a shared, already-running multi-client server.
- **Loopback-only by default.** `--web_bind` defaults to `127.0.0.1`; do not
  pass `--web_bind 0.0.0.0` for an automation session on a shared or
  untrusted network — the query API grants full tree introspection and input
  injection over the same WebSocket connection.
- **`get_logs()` only sees logs from after the browser connected.** Logging
  is pushed live, not replayed from history — a message logged before
  `AutomationClient.launch()` finished connecting is never delivered.
- **A `wish.Client` `Proxy` you don't keep a reference to gets destroyed
  almost immediately.** `client.instantiate(...)` as a bare statement (result
  discarded) creates the remote object, then Python garbage-collects the
  returned `Proxy` right away, and its destructor destroys the remote object
  — so it's gone before automation ever queries the tree (symptom: `get_tree()`
  returns an empty/near-empty `widgets` list even though `instantiate` clearly
  succeeded). Always assign it: `proxy = client.instantiate(...)`, and keep
  `proxy` in scope for as long as the object should exist. Server-side
  `--verbose` (prints `connect`/`instantiate`/`destroy`/`disconnect` trace
  lines) is the fastest way to confirm this is what happened — an `instantiate
  ok` immediately followed by `destroy` for the same object is the signature.
- **`bison.Dynamic` (Python) is not a dict — it has no `.get()`.** Read a
  field with `payload["field_name"]`, not `payload.get("field_name")`.
  Calling `.get(...)` resolves through `Dynamic.__getattr__`'s generic
  RMI-method-call fallback instead of raising `AttributeError`, so it fails
  *silently* (returns nothing useful, no exception) — an easy way to
  misdiagnose "the event handler never fired" when it actually fired fine and
  only the payload read was wrong.
- **Right-clicking a widget to open a `ContextMenu` needs a real gap
  between mousedown and mouseup**, not a near-instant down+up. `click(path,
  button=1)` already does this (it passes a `delay` to `page.mouse.click()`
  -- see its doc comment for why: without one, ImGui's own click detection,
  e.g. `BeginPopupContextItem()`'s default `ImGuiPopupFlags_MouseButtonRight`,
  never sees a press-then-release across two distinct frames, so nothing
  opens and every item inside the target `ContextMenu` keeps reading back
  `rect: null`/`visible: false`). If you ever bypass `click()` and drive
  `ui._page.mouse` directly, keep that gap: `mouse.move(x, y)`,
  `mouse.down(button="right")`, a real sleep (tens of ms), `mouse.up(button=
  "right")`.
- **`get_tree()` measurements right after triggering a state change can
  reflect a not-yet-settled frame.** An auto-height row's size (see
  `docs/ui-elements.md`'s `height` field doc) is *the previous frame's*
  measured size, reused for this frame's stretch-row math — right after a
  window first appears, or right after a resize/data change, that
  "previous frame" may not have converged yet, producing wildly wrong
  rects (e.g. a stretch child measuring almost zero height) that look
  exactly like a real layout bug on the very first `get_tree()` call.
  Sleep ~1–1.5s (a handful of frames) after the triggering action before
  trusting rects for anything relying on auto/stretch sizing; a `screenshot()`
  taken too early can look subtly wrong the same way.
- **A `HorizontalLayout` child with an explicit `width` gets wrapped in its
  own `ImGui::BeginChild()`** (the mechanism behind fixed-width/stretch
  columns — see `Layout::width`'s field comment). Content inside that nested
  child window does not get correct hover/click detection when the ancestor
  `Window` is `modal: true` (`BeginPopupModal`) — clicks silently do nothing
  even though the widget's reported `rect` looks correct and matches the
  screenshot. Don't set an explicit `width` on children of a `HorizontalLayout`
  inside a modal `Window`; let them auto-size to content instead. This also
  means `align: "right"` (which computes its offset from the sum of children's
  `width` hints) must not be combined with width-less children either — with
  no explicit widths it sums to 0, pushing the offset almost to the full
  available width and shoving the row off-screen (`visible: false`).
  Connect first, then drive the app, and this is a non-issue in practice.
  A `Spring` child is unaffected by any of this — it never gets a
  `BeginChild()` wrap (it has no content to constrain) — so it's the safer
  choice for weighted/centered spacing inside a modal `Window`'s
  `HorizontalLayout`/`VerticalLayout`, where an explicit negative `width`/
  `height` on a real widget would hit the hover/click issue above.
