# Documentation & Project Files Maintenance

## README.md — keep it concise

`README.md` is the first file agents read each conversation. Its purpose is to give a quick project overview, core build commands, a quick-start snippet, and a summary of concepts — then link out to `docs/` for details. **Do not inline long how-to content into README.md.**

Rules:
- Keep `README.md` under ~200 lines. Prose should be tight; no padded explanations.
- Detailed setup instructions belong in `docs/building.md`.
- Example run instructions belong in `docs/examples.md`.
- When you add or change a feature, update the relevant section in `README.md` (or the relevant `docs/` file) to reflect the change. Do not leave stale content.
- If you add a new `docs/` file, add a row for it in the "Further Documentation" table in `README.md`.

## Code documentation

Maintain clear, concise doc comments in code, especially for public APIs, classes, and module-level contracts. Prefer brief Doxygen comments that explain intent, parameters, return values, and failure modes.

## DESIGN.md — directory architecture docs

Some directories contain a `DESIGN.md` file that describes the architecture, key abstractions, and design decisions for the code in that directory.

**Contents:** Overview of the subsystem's purpose, a diagram or summary of key abstractions and their relationships, the public API contract, and the *why* behind non-obvious design decisions (trade-offs, constraints, alternatives rejected). Do not restate what the code already says — explain intent and reasoning.

**When to read:** Before making any change in a directory that has a `DESIGN.md`. Understanding the intended design prevents changes that technically compile but violate the subsystem's contracts or invariants.

**When to update:** After any change that affects the architecture, the public API surface, or a key design decision documented there. Keep it accurate; a stale `DESIGN.md` is worse than none.

**When to create:** Only when explicitly asked. Do not create a `DESIGN.md` speculatively.

## PLAN.md — feature implementation plans

Some features or directories contain a `PLAN.md` file that describes the ordered sequence of steps for implementing the feature. Each step is a self-contained, testable deliverable small enough for a human to review comfortably before the next step begins.

**Contents:** An introductory note pointing to the relevant `DESIGN.md`, then a numbered list of steps. Each step has a **Goal** (one sentence stating what is true when this step is done), **Deliverables** (the exact files created or changed and what each must contain), and **Tests** (the specific assertions that must pass before moving on). A **Completion Criteria** section at the end lists the overall pass/fail conditions for the entire feature.

**When to read:** Before implementing any step of a planned feature. If a `PLAN.md` exists, follow it — do not skip steps or reorder them without a clear reason. If a step is already done, verify its tests pass before continuing.

**When to update:** If implementation reveals that a step's deliverables or tests need adjustment, update the plan *before* diverging from it, so the document stays authoritative. Check off or remove completed steps when the feature ships.

**When to create:** Only when explicitly asked. Do not create a `PLAN.md` speculatively.

## Startup reading

At the start of every new conversation, always read `README.md` to understand the project's structure and goals. When task-specific details are needed, read the relevant `docs/` file and the in-code documentation.

## Tests

Always write automated tests validating any new or modified behavior. Maintain and update tests alongside code changes; tests should be concise, deterministic, and focused on public behavior.

# Whish Coding Style Guide for Claude Code Assist

Use this guide when generating or editing code in this repository.

## General

- Match the style already used in nearby files first.
- Keep changes minimal and focused; do not reformat unrelated code.
- Preserve existing public API names and behavior unless explicitly requested.
- Prefer readable, explicit code over clever shortcuts.
- Use ASCII by default.

## C++ Style (Primary)

### Formatting

- Follow `.clang-format` exactly.
- Use 2-space indentation, never tabs.
- Keep line length around 80 columns.
- Use attached braces (`if (...) {`, `class X {`, `namespace N {`).
- Use one space before control statement parentheses (`if (...)`, `for (...)`).
- Use pointer alignment on the left (`Type* ptr`).
- Let includes be sorted by formatter rules.

### File Structure

- Start files with the MIT license header used in this repo.
- Add `@file` and `@brief` Doxygen comments for public headers and key sources.
- Use section dividers for readability in larger files (for example: `// ── Section ──`).

### Includes

- In headers/sources, keep project includes grouped before standard library includes.
- Prefer explicit includes over relying on transitive includes.

### Naming

- Keep namespace style as `bdg::wish::...` and close with a namespace comment.
- Follow existing naming in each subsystem
- Use trailing underscores for private member fields (`running_`, `mtx_`).
- Use descriptive local names (`payload_bytes`, `request_id`, `workers_mutex_`).
- **Always write `bdg::bison::key_t` fully qualified, never bare `key_t`.** glibc's
  `<sys/types.h>` (transitively included by `<gtest/gtest.h>` and other system headers)
  also defines a `key_t` typedef (`__key_t`, used by SysV IPC). Even with
  `using namespace bdg::bison;` in scope, an unqualified `key_t` is ambiguous between
  `bdg::bison::key_t` and the global `::key_t` and fails to compile. This is a very
  common compile error in Claude-generated test code in this repo — always qualify as
  `bdg::bison::key_t` (or `bison::key_t` if already inside the `bdg` namespace).

### API and Error Handling

- Prefer clear contract comments on public methods (`@param`, `@return`, failure behavior).
- Use `std::runtime_error` (and derived errors) for C++ error reporting where appropriate.
- At C ABI boundaries, catch all exceptions and convert to error codes/null handles.
- For optional results, prefer `std::optional` over sentinel values.

### Concurrency

**Prefer `bison::synchronized<T>` over raw `std::mutex` for all shared state.**
`synchronized<T>` wraps a value and makes it inaccessible without explicitly
calling `.rlock()` (shared read) or `.wlock()` (exclusive write), so the type
system enforces synchronization rather than relying on comments or convention.
Avoid raw `std::mutex`, `std::lock_guard`, and `std::unique_lock` except at the
lowest implementation level (e.g. inside custom data structures).

#### Session threading model

The wish server uses a two-level synchronization hierarchy:

- **`server::sessions_`** (`synchronized<map<id, sync_session_ptr>>`): protects
  the sessions map at server level.  Acquire its `rlock` briefly to look up a
  session; acquire its `wlock` only when adding or removing sessions.

- **Per-session `sync_session`** (`synchronized<session>`): protects all data
  owned by one session — the UI element tree, `top_level_objects`, `emit_event`,
  etc.  The rule is:
  - The **render loop** acquires each session's `rlock` for the duration of
    `render_session`.  Multiple sessions are rendered sequentially (render loop
    snapshots the session list first), each under its own rlock.
  - The **RMI dispatch hook** (`on_before_dispatch`) acquires the session's
    `wlock` for the entire message dispatch and stores a raw `session*` in
    `detail::current_session` (thread_local).  `on_after_dispatch` releases
    the wlock and clears the pointer.
  - **Form and template-handler methods** read/write session data via
    `detail::current_session` — they do NOT acquire additional locks, because
    the dispatch wlock is already held and `std::shared_mutex` is non-recursive.
  - Code that runs **outside dispatch** (e.g. form destructors during cleanup)
    must acquire the session wlock directly: `sync_sess_->wlock()`.

Do NOT store a raw `session*` or `session&` as a long-lived member.  Any class
that needs to access session data across calls must hold a `sync_session_ptr`
(`std::shared_ptr<bison::synchronized<session>>`).

- Keep lock scope tight.
- Use condition variables with explicit shutdown/stop flags.

## Platform Support

wish targets **Linux, MSYS2, and native Windows (MSVC)**. MSYS2 provides the
same POSIX layer as Linux and still defines `_WIN32`/`WIN32` (it uses the
mingw-w64 toolchain), so MSYS2-only code such as the
`__declspec(dllexport/dllimport)` `WISH_API` macro in
`include/wish_client_c.h` must stay — it is not native-Windows-only. Do not
add a separate platform-suffixed file (e.g. `logger_win.cpp`) for a small
platform difference; avoid conditional compilation (`#ifdef`,
`#if defined(...)`) to branch on OS behavior unless truly necessary. If a
genuine platform difference arises, prefer a small, narrowly-scoped
`#if defined(_WIN32)` guard within the shared file over a full file split;
split into `_posix`/`_win` suffixed files only when the divergence is large
enough that a guard would make the shared file hard to read. Native MSVC is
stricter than GCC/Clang about template instantiation order (e.g. it requires
complete types where GCC/Clang tolerate an incomplete forward declaration in
some contexts) — do not assume something that compiles cleanly on
Linux/MSYS2 will compile on MSVC without verification.

## Testing Style (GoogleTest)

- Use `TEST` / `TEST_F` with descriptive suite and test names.
- Prefer `ASSERT_*` for preconditions and `EXPECT_*` for subsequent checks.
- Group tests with clear section banners in larger test files.
- Keep tests deterministic and avoid timing-sensitive flakiness where possible.

## Python/C#/Java Binding Style

- Maintain consistent formatting and naming (snake_case for Python, PascalCase for C#/Java).
- Ensure public APIs have clear documentation (docstrings or XML comments).

## The bison library

The `extern/bison` submodule (also checked out separately at `c:\github\bison`) is a
first-party dependency and **may be modified**. When a wish feature requires a missing
bison capability (a new hook, a template helper, a new transport primitive, etc.), or a
platform-compatibility fix (e.g. an MSVC build error) that originates in bison:

1. Identify the minimal change needed in bison.
2. Propose the change to the user with a clear rationale before implementing.
3. Once approved, apply the change to `c:\github\bison` on the `main` branch (fast-forward
   to `origin/main` first if the local `main` is stale), commit it there, then update
   the wish submodule to reference the new bison commit.

Follow the same coding style and documentation standards as the rest of bison (see the bison
source for conventions). Do not modify bison unilaterally — always get explicit approval first.

### Updating the submodule after a bison commit

**Never commit directly inside `c:\github\wish\extern\bison`.** That checkout is the
submodule working tree; committing there creates an orphan commit on a detached HEAD
that diverges from `c:\github\bison`'s `main` branch. When wish is later pushed,
git cannot reconcile the two histories and rejects the push as non-fast-forward.

The correct sequence after committing a bison change at `c:\github\bison`:

```
# 1. Push bison main so the new commit exists on the remote.
git -C c:/github/bison push origin main

# 2. In the submodule checkout, fetch and detach HEAD at the new commit.
git -C c:/github/wish/extern/bison fetch origin
git -C c:/github/wish/extern/bison checkout <new-sha>

# 3. Stage the updated submodule pointer in wish and commit.
git -C c:/github/wish add extern/bison
git -C c:/github/wish commit -m "..."
```

Step 1 must come before step 2 so the new SHA is available on the remote when wish is
eventually pushed.

## Automation: debugging and testing a wish UI

wish ships an optional automation module (`src/automation/`, gated by
`WISH_ENABLE_AUTOMATION`, requires `WISH_ENABLE_WEB=ON`) that lets an agent
*see and drive* a running wish UI instead of reasoning about it purely from
source: query the live widget tree (paths, classes, field values, screen
rects, hover/active/visible state), take a real screenshot, and inject
mouse/keyboard input — the wish equivalent of Playwright driving a web page.
Full protocol/architecture: [src/automation/DESIGN.md](src/automation/DESIGN.md).

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

### Prerequisites

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

### Python client

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
| `click(path)` | Click a widget's center — a real DOM/CDP mouse event, indistinguishable from a human click. Raises if `path` doesn't exist or was never rendered (`rect` is `None`). |
| `type_text(path, text)` | Focus-click, then type — for `InputText`/`InputInt`/`InputFloat` etc. |
| `screenshot()` | Pixel-perfect PNG bytes of exactly what's on screen right now — attach to a bug report, or eyeball visually with the `Read` tool after writing to a file. |
| `wait_for(js_predicate)` | Block until a JS predicate is true — e.g. wait for an async operation's result to land, a dialog to close, or a new log entry to appear, before asserting. `async` predicates that call `getTree()`/`getWidget()` work directly (Playwright awaits the returned Promise on every poll). |

### Workflow: driving a form end-to-end (not just observing)

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

### Workflow: investigating a UI bug report

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

### Workflow: writing an e2e regression test

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

### Gotchas

- **A widget with `rect: null` was never rendered this frame** — e.g. it's
  inside a collapsed `TreeNode`, an unopened `TabBar` tab, or a window that
  hasn't been given a chance to draw yet. Navigate to make it visible (or
  `wait_for` the tree to settle) before asserting on its rect or clicking it.
- **Only leaf-widget rects are reliable.** A container/window element's
  captured rect actually reflects whatever widget happened to render last
  *inside* it (an inherent consequence of the hit-test capture happening
  right after each ImGui call — see "Hit-test capture mechanism" in
  `src/automation/DESIGN.md`), not a meaningful bounding box for the
  container itself. Assert on the specific interactive widget you care
  about, not its enclosing `Window`/`Layout`/`TabBar`.
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

## Security Considerations for AI Code Assist

When generating or modifying wish server-side code, AI agents must follow these rules:

### File access is sandboxed per session

Every client session has an isolated temporary directory (`session::resource_dir`).
Widget fields that accept file paths (e.g. `Image::src`, `TextEditor::file_path`)
**must** be validated against this sandbox before any file I/O:

- **Relative paths** are resolved with `resolve_widget_path()` (defined in
  `src/imgui/imgui_ui_renderer.hpp`), which uses purely lexical normalization to
  verify the resolved path stays inside `resource_dir`. Never skip this check.
- **Absolute paths** are rejected by default. They are permitted only when the
  server has been explicitly configured with
  `wish::server::set_allow_absolute_paths(true)` — which is intended exclusively
  for same-process (`memory_transport`) deployments.
- **Never** construct file paths from untrusted client input without going through
  `resolve_widget_path()` or `file_service::resolve_path()`.  A path like
  `../../etc/passwd` must be rejected, not silently truncated or accessed.

### Adding new file-accessing widgets

Any new widget whose render function reads or writes a file must:

1. Accept only a **relative** path field by default.
2. Call `resolve_widget_path(path, s.resource_dir, s.allow_absolute_paths)` and
   return early if the result is empty.
3. Document the field's security contract in the registration attributes
   (see `src/ui/ui_elements/text_editor.cpp` as a reference).

### Optional auth module and persistent sandbox directories

By default every session's `resource_dir` is a throwaway temp directory,
deleted on disconnect. A server can opt in to a persistent, identity-keyed
directory per client via `wish::server::start(auth_module)` (an optional
`bison::rmi::auth_module_ptr`, evaluated once per connection) together with
`wish::server::set_allow_absolute_paths`'s sibling setter,
`set_persistent_sandbox_root(path)` — both are required before any session
gets a directory outside the default temp location. `wish::local_auth_module`
is a ready-to-use, trust-the-client module for local/single-user deployments;
untrusted or remote deployments must supply their own `auth_module_iface`
that verifies the client's claimed identity. See `src/auth/DESIGN.md` for
the full design and the sandbox-escape guard applied to the identity string.

### Uploads and downloads

`file_service::upload` / `download` already enforce sandboxing via
`file_service::resolve_path()`.  Do not bypass or replicate this logic; call the
method directly.

### Session isolation

Each `wish::session` is independent: object tree, resource folder, file service,
and style service are all isolated.  Do not share mutable session state across
sessions or cache per-session resources in process-global structures keyed by
anything other than the session ID.

## Claude Code Assist Behavioral Rules for This Repo

- Do not introduce broad stylistic rewrites.
- Do not change naming conventions in existing APIs.
- When adding new C++ files, mirror the RMI/core style in nearby files.
- When adding bindings/tests, mirror style from sibling binding/test files.
- If style is ambiguous, prefer consistency with adjacent code over generic defaults.