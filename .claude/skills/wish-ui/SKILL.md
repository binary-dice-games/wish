---
name: wish-ui
description: Add wish UI capabilities to a third-party application via the wish client C ABI (wish_client_c.h / wish_client_dll) — build a UI tree and wire it to the host app's own data and events. Purely client-side; the host app connects to an existing/separately-run wish server. Example shape — a live resource-usage graph using Plot/PlotLine.
---

You are embedding wish UI into **someone else's application** — this is
purely client-side integration through the wish C ABI
(`include/wish_client_c.h`, backed by `wish_client_dll`, plus the bison ABI
it's layered on: `rmi_c.h`/`bison_c.h`). The host app is not part of the
wish source tree; it links against `wish_client_dll` (or a language binding
built on it — Python `ctypes`, C# P/Invoke) and connects to a wish server
that renders the UI. Never write server-side code for this skill — if the
request needs a new bison class, new fields, or new server logic, that's
`/wish-module` (or a bare feature request against the wish server itself),
not this skill.

Read [../wish-shared/CONCEPTS.md](../wish-shared/CONCEPTS.md) first — it has
the object model, widget catalog, and automation workflow shared with
`/wish-module`. This document only covers what's specific to embedding.

## 1. Load context

1. `README.md` (root) — architecture overview and the two Quick Start
   snippets (template vs. manual tree construction) that this skill's
   output will mirror in whatever language the host app is written in.
2. `include/wish_client_c.h` — the actual ABI surface: client lifecycle
   (`wish_client_tcp_create`/`wish_client_pipe_create`/`wish_client_run`),
   template registration/instantiation, direct `wish_instantiate()`,
   style presets, file transfer, logging. Every function has a doc
   comment with the exact contract (ownership, blocking behavior, error
   codes) — read the ones you're about to use, don't guess signatures.
3. `extern/bison/include/rmi_c.h` and `extern/bison/include/bison_c.h` —
   proxies returned by the wish ABI are plain `rmi_proxy_handle`s:
   `rmi_proxy_set()`/`rmi_proxy_get()`/`rmi_proxy_call()`/
   `rmi_proxy_on_event()`/`rmi_proxy_release()` drive them, and
   `bison_c.h`'s `bison_create()`/`bison_set_*()`/`bison_set_*_at()`
   build/read the `bison_handle` field payloads those calls exchange
   (array-valued fields like `Plot` series' `xs`/`ys` are populated via
   the `_at(handle, index, value)` variants).
4. `docs/bindings.md` — if the host app is Python or C#, its binding
   (`bindings/python/wish/`, `bindings/csharp/Wish/`) wraps this same ABI
   in idiomatic syntax; prefer it over raw `ctypes`/P-Invoke calls when the
   host app is already in one of those languages. If the host app is C++
   directly, `src/client/client.hpp`'s C++ API (as shown in the root
   README) is a thinner layer than the C ABI and preferred when the host
   app can link `wish_client` (C++) rather than `wish_client_dll` (C ABI).

## 2. Understand the two halves of the integration

Every embedding has two independent pieces — design them separately:

1. **The UI tree** — what gets rendered. Either a JSON/YAML template
   (`register_template`/`instantiate_template`, best when the layout is
   static or has a small number of variants) or built node-by-node with
   `instantiate()` calls (best when the tree's shape depends on runtime
   data the host app already has, e.g. one row per currently-running
   process). See CONCEPTS.md section 2-3 for the schema and widget
   catalog.
2. **The logic** — how the host app's own data/behavior drives that tree
   and reacts to it, entirely from the client side:
   - **Pushing data in**: the host app calls `proxy_set()` (or the
     language binding's `.set({...})`) whenever its own state changes —
     e.g. on a timer, push new samples into a `PlotLine`'s `xs`/`ys`
     fields, or update a `Label`'s `text`.
   - **Reacting to input**: `proxy_on_event()` (`.on_event()`/`onEvent()`)
     registers a callback invoked when the user interacts with a widget
     (button click, table row selection) — that callback is host app code,
     called back into from the RMI worker thread, and is where the host
     app's own logic runs (e.g. a "Pause" button's handler stops the
     host app's own sampling timer).
   - **Imperative calls**: `proxy_call()` (`.call()`) for anything
     modeled as a method rather than a field/event on the target class.

   There is no server-side "method" to register for a plain embedding —
   all app-specific behavior lives in the host app's own callback
   functions and its own polling/timer loop, wired to widget fields and
   events purely through the ABI calls above.

## 3. Clarify the integration with the user

Before writing code, confirm:

- **What UI surface is being added** (a whole new window? a panel merged
  into an existing session's tree? one ad-hoc dialog?) and **what data
  drives it** — where does the host app already have the numbers/state
  that need to appear on screen, and at what cadence do they change (event
  driven vs. polled on a timer)?
- **What language/build the host app is in** — determines whether to use
  the raw C ABI, the C++ `client` class, or a Python/C# binding, and
  whether `wish_client_dll` needs to be built/linked first (see
  `docs/bindings.md`'s "Requirements" per language).
- **Which transport** the host app will connect over (`tcp`, named pipe,
  or in-process if the wish server happens to run in the same process) —
  affects which `wish_client_*_create()` constructor to use.
- **Interactive elements**, if any — every button/input the mockup needs
  implies a callback the host app must supply real logic for; get a list
  of these up front rather than discovering them mid-implementation.

Use `AskUserQuestion` when the request leaves any of the above open — e.g.
"a resource usage graph" doesn't say whether it's CPU-only, multi-series
(CPU + memory), or how many seconds of history to keep in the plot.

## 4. Design and mock the UI before wiring real data

Even though this is a third-party integration, build and validate the UI
tree exactly as `/wish-module` does, using a **temporary standalone driver
script** — not the host app itself — so the user can approve the visual
design before any host-app code is touched:

1. Write the template/tree JSON (or the instantiate-call sequence)
   standalone, in whatever throwaway file/script is convenient.
2. Build wish with automation enabled per CONCEPTS.md section 6, run
   `build/app/wish server --renderer web --web_port 8080`.
3. Drive it with a small Python script using **either** `wish.Client`
   (if you want to exercise the exact template/instantiate calls the real
   integration will use) **or** `wish.automation.AutomationClient`
   attached via `url="http://127.0.0.1:8080"` (to screenshot/inspect what
   the first script rendered):
   ```python
   from wish import Client

   def session(client):
       client.set_style_preset("dark")
       client.register_template("ui", MY_TEMPLATE_JSON)
       root = client.instantiate_template("ui", "ui")
       # feed representative fake data, e.g. a synthetic CPU curve
       client.wait()

   Client.tcp("127.0.0.1", 8080).run(session)
   ```
   ```python
   from wish.automation import AutomationClient

   with AutomationClient.launch(url="http://127.0.0.1:8080") as ui:
       open("mockup.png", "wb").write(ui.screenshot())
       tree = ui.get_tree()
   ```
4. Read `mockup.png` back with the `Read` tool and show it to the user;
   iterate before writing the host app's real integration code. For a
   plot-shaped example, feed a synthetic sine wave or ramp into `xs`/`ys`
   so the mockup shows realistic-looking data, not an empty plot.

   **If `ui.screenshot()` comes back as a broken-image placeholder and
   `page.on("console")` (or the server's own log) shows
   `CONTEXT_LOST_WEBGL`**, this environment has no usable GPU for the web
   renderer's canvas — see CONCEPTS.md section 6's "Known limitation".
   Do not retry the screenshot, add Chromium GL flags, or otherwise try to
   fix it; that's an environment property, not something fixable from this
   session. Instead: keep using `ui.get_tree()` to confirm the template
   registered the widgets you expect at the right paths (labels, text,
   structure — everything except pixels), and produce the visual mockup
   for the user as a standalone styled HTML file (`Artifact` tool) that
   mirrors the template 1:1 — same widget tree shape, same labels/text —
   styled to resemble wish's actual look (ImGui dark/light presets: flat
   panels, thin borders, a titlebar, no border-radius-heavy/rounded-card
   styling). Tell the user plainly that this is a styled stand-in, not a
   real render, and why (no WebGL in this environment).
5. For interactive elements, drive them with `ui.click()`/`ui.type_text()`
   and confirm the intended event fires (check `ui.get_logs()` if the
   fake session logs on each callback) before wiring the real host-app
   callback. This still works even when screenshots don't, since it does
   not depend on the canvas actually painting.

## 5. Implement the real integration

Once the design is approved:

1. Add the wish client dependency to the host app's build (link
   `wish_client_dll`/`wish_client`, or add the Python/C# package reference
   per `docs/bindings.md`).
2. Write the connect/session setup (`wish_client_*_create` +
   `wish_client_run`, or the binding equivalent), register/instantiate the
   UI tree designed in step 4.
3. Wire the host app's real data source to the tree's fields — typically
   a periodic timer/thread in the host app calling `proxy_set()` with
   fresh values (e.g. append a new sample to `xs`/`ys` each tick, trimming
   old points to keep the plot's window bounded — decide and implement a
   fixed history length rather than letting the arrays grow unbounded).
4. Wire real event handlers in place of the mockup's fake ones —
   `proxy_on_event()`/`.on_event()` callbacks should call directly into
   the host app's existing logic (its own pause/resume/refresh functions),
   not duplicate it.
5. Release proxies (`rmi_proxy_release()`/`wish_release()`) and disconnect
   cleanly on host app shutdown — match every `instantiate`/
   `instantiate_template` with a release, per the ABI doc comments'
   ownership notes.

Follow the coding-style rules in the root `CLAUDE.md` for any C++ written
for the host app's wish integration layer, and the sibling-file convention
from CONCEPTS.md section 7 for Python/C#.

## 6. Verify the finished integration end-to-end

Don't stop at "it connects and doesn't throw." Rebuild with automation
enabled, run the actual host app (pointed at a `wish server --renderer web`
instance), and use `AutomationClient` to confirm:

- The expected widgets exist in `get_tree()`/a screenshot once the host
  app has connected and pushed its first real data.
- Values actually update over time — e.g. `get_widget("plot.series")`'s
  data (or a `Label`'s `text`) changes across two `get_tree()` calls taken
  a few seconds apart while the host app is running for real, not fed
  synthetic mockup data.
- Every interactive element wired in step 5 produces the real host-app
  side effect, not just an event firing — e.g. clicking "Pause" actually
  stops the host app's sampling (confirm via whatever the host app exposes
  — a log line via `wish_log()`, a field that reflects paused state, etc.),
  not merely that the click was received.

This mirrors the root `CLAUDE.md`'s note that a client-side exception
disappearing can mean the bug is fixed, or just that the failure moved
somewhere silent — the same applies here to "the UI appeared" vs. "the UI
reflects the host app's real, live state."
