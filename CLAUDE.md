# Documentation & Project Files Maintenance

## README.md — keep it concise

`README.md` is the first file agents read each conversation. Its purpose is to give a quick project overview, core build commands, a quick-start snippet, and a summary of concepts — then link out to `docs/` for details. **Do not inline long how-to content into README.md.**

Rules:
- Keep `README.md` under ~200 lines. Prose should be tight; no padded explanations.
- Detailed setup instructions belong in `docs/building.md`.
- Example run instructions belong in `docs/examples.md`.
- When you add or change a feature, update the relevant section in `README.md` (or the relevant `docs/` file) to reflect the change. Do not leave stale content.
- If you add a new `docs/` file, add a row for it in the "Further Documentation" table in `README.md`.

## CHANGELOG.md — user-facing change log

After successfully completing any task that changes code in the project, add an
entry to `CHANGELOG.md` under the `## [Unreleased]` section (create that section
at the top, above the most recent release, if it does not exist).

Rules:
- Keep entries very brief: state *what* was done, not *how* or *why*.
- Record only changes that affect users of wish (public API, widget/UI behavior,
  CLI, build outputs, bindings, transports, server behavior). Do not log internal
  refactors, test-only changes, or documentation-only changes.
- Group entries under `### Added`, `### Changed`, `### Fixed`, `### Deprecated`,
  or `### Removed` as appropriate.
- Follow the [Keep a Changelog](https://keepachangelog.com/) format already used
  in the file.

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

### Performance: hot-path `bison::dynamic` field access

`ui_element` walks its own tree 1-3x per frame (measure, arrange, render), so
a `findField()`/`get_as()`/`as()` call on a field read on (nearly) every node
in one of those passes runs at real volume — even though `findField()` is
already O(1) via `field_lookup_`, a warm `unordered_map` lookup still costs
more than a plain member read at that call volume. **Prefer caching the
`field*` pointer**, not the field's value, as a `mutable bison::field*`
member on `ui_element` (see `class_key()`/`visible()`/`path()`/`wish_id()`/
`font_path()`/`font_size()`/`highlight()`/`profiler_marker()` in
`src/ui/ui_element.hpp`):

- `bison::dynamic` never erases a *named*-key field once created — the only
  erase paths (`erase(size_t)`, `clear()` in `bison_object.hpp`) operate
  strictly on the numeric/array-index range, never on named keys like
  `"visible"_key` or `dynamic::CLASS`.
- `fields_` is a node-based `std::map<key_t, field>` — inserting/erasing
  *other* keys never invalidates pointers/references to existing elements.
- So once a named field exists, its `field*` is stable for the object's
  lifetime. Reading through the cached pointer (`->is<T>()`/`->as<T>()`)
  always observes the live value — **no invalidation logic needed**, no
  matter how many of `dynamic`'s independent, non-virtual write paths
  (`operator[](key_t)`, `as<T>()`, `addField()`, direct `field*` mutation)
  touch the field. Caching the field's *value* instead would go stale, since
  none of those write paths are virtual or funnel through one choke point.
- The only edge case: a field that doesn't exist yet at first read. The
  cache stays null and falls back to a default; re-attempt `findField()` on
  each access while still null, then freeze permanently once found.

Use `ui_element::cached_field()`/`cached_field_or<T>()` to write a new
accessor without hand-rolling the pattern — add one `mutable bison::field*`
member and a one-line accessor:

```cpp
bool highlight() const {
  return cached_field_or<bool>(highlight_field_, bison::key_t{"__wish_highlight__"}, false);
}
```

Only add a pointer-cache accessor for a field read on (nearly) every node of
a hot traversal (measure/arrange/render) — not for fields read once per call,
from a cold/debug path, or only on one specific widget class.

**Default to the flat approach**: add the accessor directly on base
`ui_element`, even when only a subset of widget classes actually populate
that field — most widgets (Button, Slider*, Drag*, Table, Splitter, Layout,
Menu*, etc.) are instantiated as plain `ui_element` via
`dynamic::make_factory<ui_element>(...)`, with no widget-specific C++
subclass, so a subclass-per-field would mean introducing new factory
subclasses just to host one accessor. Reserve a typed `ui_element` subclass
(`dynamic::make_factory<your_class>(...)` in place of the default) for a
field that's genuinely hot *and* the widget already has (or clearly
warrants) its own subclass for other reasons — not as the default answer to
"this field is widget-specific."

For a field whose *stored type* varies by widget (e.g. `"width"`/`"height"`
are `float` on most widgets but `int32_t` literals on window/dockspace),
share one `mutable field*` cache member across multiple typed accessors
instead of introducing per-widget-type subclasses — the cache holds the
field's *location*, not its type, so each accessor can read it as whatever
type that particular node actually stores (see `width()`/`width_i()` in
`ui_element.hpp`).

**`cached_field_or<T>()` must coerce, not just type-match.** A JSON literal
like `"width": -1` is stored as an `int32_t` field even though most
`width()`-style accessors return `float`. `cached_field_or<T>()` calls
`field::get_as<T>()` (which cross-type-converts, e.g. `int32_t -> float`)
rather than the stricter `field::is<T>() ? field::as<T>() : default`
pair — using the strict pair silently returns the *default* for any field
whose stored type doesn't exactly match the accessor's `T`, which is a easy
mistake to reintroduce when adding a new accessor by hand instead of going
through `cached_field_or<T>()`. This exact bug previously reintroduced an
unbounded per-frame table-growth regression (`file_dialog.cpp`'s
stretch-fill file list has `"width": -1`/`"height": -1` int literals; a
strict-typed accessor made `arrange_vertical_layout()` misclassify the
Table as "auto" instead of "stretch", which fed the Table's own previous
`last_rendered_size()` back into its measured size every frame). Always add
new hot-field accessors via `cached_field_or<T>()` rather than hand-rolling
the `is<T>()`/`as<T>()` check.

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

wish ships an optional automation module (`src/automation/`) that lets an
agent *see and drive* a running wish UI instead of reasoning about it purely
from source: query the live widget tree, take a screenshot, and inject
mouse/keyboard input — the wish equivalent of Playwright driving a web page.

**Use this whenever you are debugging a UI-visible bug, verifying a fix, or
writing an e2e regression test for a wish app** — it is almost always faster
and more reliable than re-reading render code and guessing at runtime state.
This includes verifying that a client example/binding actually works
end-to-end, not just that it compiles and runs without throwing — "didn't
crash" is a weaker check than "the expected widget tree rendered."

Full setup, workflows, and known gotchas:
**[docs/automation.md](docs/automation.md) — read it before using this
workflow.** After a debugging/verification session that used it, update
that document with anything you learned that isn't already there (a new
gotcha, an environment quirk, a better/worse way to drive some interaction)
before you finish, even if the user didn't ask for a documentation update —
a gotcha you hit and didn't record is one the next agent will hit again.

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