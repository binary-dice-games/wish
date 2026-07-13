---
name: wish-module
description: Scaffold a new wish CLI module (an optional, compile-time "wish client --run <name>" app) — server-side form logic and/or client-side app code, wired into the modules/ system, validated with a mockup via the automation module before the real logic is written.
---

You are creating a new **wish module**: a self-contained optional tool that
ends up runnable as `wish client --run <name>` (or `wish standalone
--run=<name>`), following the layout and CMake wiring documented in
`modules/README.md` and `src/ui/forms/DESIGN.md`'s "Module System" section.

Read [../wish-shared/CONCEPTS.md](../wish-shared/CONCEPTS.md) first — it has
the object model, widget catalog, and automation workflow shared with
`/wish-ui`. This document only covers what's specific to building a module.

## 1. Load context

In order, before writing anything:

1. `README.md` (project root) — architecture overview, "Declaring bison RMI
   Classes" pattern, "Security Considerations".
2. `modules/README.md` — the exact directory layout
   (`modules/<org>/<collection>/<name>/{server,client,resources/embedded}/`)
   and how `wish_add_module()`/`wish_add_collection()` wire a module in.
3. `src/ui/forms/DESIGN.md`, specifically the "Module System" section and,
   if the module needs a stateful server-side form, "The `form` Base Class"
   and "Form Lifecycle" above it.
4. **One existing module as a concrete template**, chosen by shape:
   - `modules/bdg/desktop/calculator/` — simplest complete example: a
     server-side `form` subclass owning all logic, a trivial client runner
     that just instantiates it and waits for `"closed"`. Start here for any
     module whose UI reacts to its own state (a game, a tool with
     server-computed output).
   - `modules/bdg/desktop/notepad/` — demonstrates the client/server file
     handshake (`on_request_open` → client `upload_file` → `open_file()` →
     `on_file_closed` → client `download_file`). Read this if the module
     needs to read/write files that live on the *client's* machine.
   - `modules/bdg/desktop/process_explorer/` — a form driven by
     periodically-refreshed data rather than direct user input; look at
     this for a monitoring/dashboard-shaped module.
5. `cmake/WishModules.cmake`'s top-of-file comment — the mechanics of
   `wish_add_module()` if you need to explain the CMake wiring to the user,
   or are adding an out-of-tree module.

## 2. Clarify the module's shape with the user

Before scaffolding, make sure you know:

- **Name and placement**: leaf `<name>`, and `<organization>/<collection>`.
  Default to `bdg/desktop` only if the user is extending wish's own bundled
  tool set; otherwise ask, since a distinct org/collection avoids the
  naming collisions described in `modules/README.md`.
- **Does it need server-side logic?** If the UI's behavior is just static
  layout plus events the *client* reacts to (e.g., a settings panel that
  only calls back into the client's own logic), a `client/`-only module
  suffices — no `server/<name>.hpp/.cpp` at all, no new bison class,
  `run_<name>()` builds the tree itself out of existing elements
  (`Window`, `Button`, `Plot`, ...) via `instantiate()`/templates. If the
  UI has its own internal state machine, computed fields, or needs to
  react to its own widget events without a client round trip (like
  `Calculator`'s arithmetic), it needs a `server/` form subclass.
- **Does it need local files on the client's machine, or files embedded in
  the binary?** Determines whether to follow the Notepad handshake pattern
  and/or add a `resources/embedded/` directory.
- **What does `--describe=<name>` need to say**, and does the app take any
  positional `app_args()` (see `calculator.cpp`'s empty `.params = {}` vs.
  notepad's one optional startup-file param)?

Use `AskUserQuestion` if any of the above is genuinely ambiguous from the
request rather than guessing — the org/collection and the
client-only-vs-server-form choice both affect the whole file layout.

## 3. Scaffold the module

Create `modules/<org>/<collection>/<name>/`:

- `server/<name>.hpp` + `server/<name>.cpp` (only if server logic is
  needed): a class inheriting `bdg::wish::form`, overriding `on_init()` to
  build the internal tree (JSON literal + `import_json()`, exactly like
  `kLayout` in `calculator.cpp`) and bind widget handlers, plus
  `on_event()` to route clicks/events. Declare and define
  `register_<name>()`, calling `dynamic::addClass()` with
  `dynamic::make_factory<YourForm>("wish"_key, "YourClassName"_key)` — copy
  the registration block at the bottom of `calculator.cpp` and adapt names.
- `client/<name>.hpp` + `client/<name>.cpp`: a free function
  `run_<name>(wish_app_host&)` that instantiates the form (or, for a
  client-only module, builds the tree directly), wires `onEvent` handlers,
  calls `s.keep_alive(std::move(proxy))`, and returns (the caller blocks on
  `signal_done()`/window close elsewhere). Below it, a self-registering
  anonymous-namespace `<name>_app_registrar` struct calling `register_app()`
  with `.organization = WISH_MODULE_<ORG>_<COLLECTION>_<NAME>_ORGANIZATION`
  and `.collection = WISH_MODULE_<ORG>_<COLLECTION>_<NAME>_COLLECTION` (the
  macro names are mechanical: uppercase the full `org/collection/name`
  path) — copy the registrar block at the bottom of `calculator.cpp`.
- `resources/embedded/` only if the module ships its own assets (icons,
  fonts) — lands in the session sandbox under
  `res/<org>/<collection>/<name>/...` at runtime, no code changes needed
  beyond creating the directory.
- `README.md` — one short paragraph plus a `server:`/`client:`/`resources:`
  bullet list, matching the style of `modules/bdg/desktop/calculator/README.md`.

Follow the coding-style rules in the root `CLAUDE.md` (license header,
`@file`/`@brief`, 2-space indent, trailing-underscore private members,
`bison::synchronized<T>` if the form has any state touched from more than
one call path).

## 4. Wire it into the build

Add one line to the root `CMakeLists.txt`, near the existing
`wish_add_collection(bdg/desktop)` call (or its own line if it's a
different org/collection):

```cmake
wish_add_module(<org>/<collection>/<name>)
```

No edits to `registry.cpp`, `app_registry.cpp`, or any generated file are
ever required — `wish_generate_module_registry()` /
`wish_finalize_app_modules()` already run unconditionally after all
`wish_add_module()` calls in the root `CMakeLists.txt`.

Configure and build with the new option on:

```sh
cmake -S . -B build -DWISH_MODULE_<ORG>_<COLLECTION>_<NAME>=ON
cmake --build build --target wish-cli   # or whichever target the user builds
```

If server-side logic was added, confirm it links: a form `.cpp` with no
external references risks being dropped by the static-archive linker if
`wish_add_module()`'s registry hook isn't wired — the symptom is the module
silently missing from `--list` despite compiling.

## 5. Validate visually before (and after) wiring up the real logic

Use the automation workflow from
[../wish-shared/CONCEPTS.md](../wish-shared/CONCEPTS.md) section 6 to show
the user a real screenshot rather than asking them to approve JSON:

1. Rebuild with `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON` added to
   the existing configure flags.
2. The fastest single-process way to render one module's UI is `wish
   standalone` (fuses server+client, no transport setup needed):
   ```sh
   build/app/wish standalone --run=<name> --renderer=web --web_port=8080
   ```
   (`wish standalone --list` confirms the name resolved correctly first.)
3. Drive it:
   ```python
   from wish.automation import AutomationClient

   with AutomationClient.launch(
       server_cmd=["build/app/wish", "standalone", "--run", "<name>", "--renderer", "web"]
   ) as ui:
       ui.wait_for("() => window.wish.ready === true")
       open("mockup.png", "wb").write(ui.screenshot())
       tree = ui.get_tree()
   ```
4. Read `mockup.png` back with the `Read` tool and show/describe it to the
   user; iterate on the layout JSON/fields before writing the rest of the
   module's logic if anything looks off.
5. Once the module has real interactive behavior, extend the same script
   into repro/regression steps — `ui.click(...)`, `ui.type_text(...)`,
   `ui.get_widget(...)`, `ui.get_logs()` — to confirm the end-to-end flow
   actually works, not just that it compiles. This mirrors the "Workflow:
   investigating a UI bug report" / "writing an e2e regression test"
   sections of the root `CLAUDE.md`.

Once satisfied, confirm the module's real deployment path also works —
`wish server --transport=tcp --port=7070 &` then
`wish client --transport=tcp --port=7070 --run=<name>` (or
`wish_run_app()` through a language binding) — since that split-process path
is what `modules/README.md` and the module's own README promise, and it
exercises the real transport `wish standalone` skips.

## 6. Write tests

Add or extend a GoogleTest suite under wherever this module's sibling tests
live (check for a `tests/` directory alongside similar modules, or
`src/ui/forms/` tests for the `form` pattern) exercising the new class
through the `memory_transport` path — see "Writing a New Form" in
`src/ui/forms/DESIGN.md` and the root `CLAUDE.md`'s "Tests" section. Keep
them deterministic; assert on field values and emitted events, not on
render timing.

## 7. Update docs

- Add/update the module's own `README.md` (step 3).
- If the module is significant enough to be user-facing (not just an
  internal demo), consider whether `README.md` (root) or
  `docs/examples.md` should mention it — only if asked, or if it changes a
  documented command surface (e.g. a new `--describe` output shape).
