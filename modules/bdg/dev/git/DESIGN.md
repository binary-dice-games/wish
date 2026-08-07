# wish git Module — Architecture & Design

## 1. Purpose / Scope

The `git` module (`wish client --run=git -- /path/to/repo`) is a
SourceTree-style GUI frontend for a local git repository: a commit graph
with branch/merge lines, a sidebar (branches/tags/stashes/remote-tracking
branches), working-directory staging and commit, a diff viewer, and
branch/remote operations. It is a pure frontend — every git operation runs
the real `git` binary already installed on the machine; this module never
reimplements git internals (no libgit2, no manual pack/object parsing).

Scope for this pass, agreed with the user up front given SourceTree's huge
feature surface: the **core SourceTree workflow** (graph, sidebar, staging,
commit, diff, branch checkout/create/delete, fetch/pull/push, fast-forward
merge, basic stash). Explicitly deferred — see [PLAN.md](PLAN.md): interactive
rebase, conflict-resolution UI, cherry-pick/revert/reset, blame, submodules,
LFS, hunk-level staging, external diff/merge tools, search/filter.

## 2. Design Goals

1. **All git invocation and parsing is client-side.** The repository a user
   wants to operate on lives on their own machine, reachable only from the
   client — mirrors `process_explorer`'s own reasoning for why sampling is
   client-side. The server never touches git, the filesystem, or a
   subprocess.
2. **The server owns all UI/render state** (selection, graph lane layout,
   sidebar structure) and renders whatever snapshot it was last given —
   matches `src/ui/forms/DESIGN.md`'s Design Goal 1 ("server-side logic,
   client-side data"), same split `process_explorer` uses for its own
   snapshot/render contract.
3. **No shell involved in any git invocation.** Every `git` call goes
   through a real argv array via `uv_spawn` (see §6), never a shell command
   string — arguments (commit messages, branch names, paths) need no
   escaping and can't be used for injection.
4. **A rebuild is a full rebuild, never an incremental patch.** Every
   `update_*` RMI call clears and fully repopulates whatever UI subtree it
   owns (sidebar section, graph table, files table, diff table) rather than
   diffing against the previous state — simpler to reason about and correct
   by construction, at the cost of some redundant `ui_element`
   creation/`ctx().objects` churn on every refresh. Acceptable for an
   interactive desktop tool refreshing on user action or a slow (~2s)
   background poll, not a hot loop.

## 3. Key Abstractions

### `git_repo` (server, `form`)

Owns **three independently dockable `Window`s** (mirrors the `editor`
module's chrome/Help/Event-log split — `form::init()` only auto-registers
one top-level root, so the extra two are registered by hand in `on_init()`
exactly as `editor.cpp` does for its Help/Event-log windows):

- **Main** (`internal_root_key_`): toolbar (Commit/Push/Pull/Fetch/Branch/
  Merge/Stash/Refresh), a status label, the sidebar (BRANCHES/REMOTES/TAGS/
  STASHES `TreeNode` sections, each populated by `do_update_refs`), and the
  commit graph `Table` (populated by `do_update_log`; first column is a
  `GraphNode` per row).
- **Files** (`files_root_key_`): either the working directory's staged/
  unstaged files (checkboxes, `status_mode_working_ == true`, populated by
  `do_update_status`) or a selected commit's changed files (read-only,
  populated by `do_update_commit_files`) — same `files_table_` `Table`,
  switched by which RMI method last wrote to it. A commit-message
  `InputText` + Commit button live in this window too.
- **Diff** (`diff_root_key_`): the selected file's diff (`do_update_diff`),
  rendered as one `TableRow` per line — a gutter `Label` (`+`/`-`/blank)
  and a content `Label`, both colored via `Label.text_color` (the hex-string
  field the `editor` module added) rather than a new widget.

### `GraphNode` field data (server-computed, see §5)

`rebuild_graph_table()` turns `update_log`'s raw `{hash, parents[], ...}`
array into per-row `GraphNode` field values via `compute_git_graph_layout()`
(`git_graph_layout.hpp`/`.cpp`, pure C++, no UI dependency) — see
`docs/ui-elements.md`'s `GraphNode` entry for the field shape and
`git_graph_layout.hpp`'s doc comment for the lane-assignment algorithm
itself (the standard `git log --graph`/gitk/magit technique).

### `git_repo_source` (client)

Owns the repository path and every actual git invocation. One method per
`*_requested` event (`on_stage`, `on_commit`, `on_checkout`, ...), each
either pushing a fresh snapshot directly (`push_refs`/`push_log`/
`push_status`, no git mutation involved) or running a mutating command via
`git_process::run_git()` and then calling `refresh_all()` (`push_refs` +
`push_log` + `push_status`, in that order) — see Design Goal 4. A background
thread (`client/git.cpp`) calls `refresh_all()` every ~2s so external
changes to the working tree are picked up without an explicit Refresh
click, mirroring `process_explorer`'s sampling loop (far less frequent
here, since a `git status` call is comparatively expensive).

### `git_process::run_git()` (client)

Non-interactive `argv -> {exit_code, stdout, stderr}` helper built directly
on libuv (`uv_spawn`), **not** `bdg::bison::term::terminal`. See
`client/git_process.hpp`'s header comment and §6 below for why.

## 4. Data Flow / Architecture

```
Startup:
  run_git(host) -- requires app_args()[0] = repo path
    validate: `git rev-parse --is-inside-work-tree` (fail fast + signal_done() if not a repo)
    instantiate GitRepo -> proxy
    wire proxy.onEvent(...) for every *_requested event (see git.hpp)
    proxy.onEvent("refresh_requested", source->refresh_all())  -- server emits this once from on_init()
    background thread: refresh_all() every ~2s until "closed"

refresh_all() (client):
  push_refs()   -> `for-each-ref` (branches+remotes), `tag --list`, `stash list`,
                    `rev-parse --abbrev-ref HEAD`, per-branch `rev-list --left-right --count`
                    for ahead/behind -> GitRepo.update_refs(args)
  push_log()    -> `status --porcelain=v1` (dirty check) + `log --topo-order --date-order
                    -n 300 --pretty=format:%H%x1f%P%x1f%an%x1f%ad%x1f%s%x1e`
                    -> GitRepo.update_log(args)
  push_status() -> `status --porcelain=v1`, X/Y-code parsed into staged/unstaged
                    -> GitRepo.update_status(args)

GitRepo.update_log (server):
  rebuild_graph_table(args)
    parse commits[] into git_graph_commit_in[]
    compute_git_graph_layout(...) -> per-row lane/color/top-segments/bottom-segments
    clear existing TableRow children (walk + erase by class, no separate key tracking)
    one add_row() per commit (+ a synthetic is_working row when working_dirty),
      each building a TableRow{GraphNode, Label x4} and inserting at a
      LOCAL 0-based key (see "dynamic::size() gotcha" in §6)
    if the current selection no longer exists in the new row set, select_row(0)

Selecting a graph row (click/dblclick -> Table "row_selected"/"row_activated"):
  select_row(index)
    selected_hash_ = graph_row_hashes_[index]  ("" == the synthetic working-tree row)
    if "" : status_mode_working_ = true; redisplay from last_status_args_ (no round trip --
             do_update_status() already ran during this same refresh_all() cycle)
    else   : status_mode_working_ = false; emit("commit_files_requested", {hash})
             -> client runs `git show --name-status --format= <hash>`
             -> GitRepo.update_commit_files(args) rebuilds the Files table read-only

Selecting a file (its own Selectable's "changed" event, not the Table's
row-level event -- see §6 on why):
  selected_path_/selected_staged_ set; request_diff_for_selected()
    -> emit("diff_requested", {hash, path, staged})
    -> client runs `git diff`/`git diff --cached`/`git show <hash> -- <path>`
       (falls back to `git diff --no-index -- <null-device> <path>` for an
       untracked file, so a brand-new file shows as an all-added diff
       instead of a blank panel)
    -> GitRepo.update_diff(args) rebuilds the Diff table, lines colored by kind

Staging (Checkbox "changed" event in the Files table, working mode only):
  checkbox_handlers_[id](checked) -> emit("stage_requested"/"unstage_requested", {path})
    -> client `git add -- <path>` / `git restore --staged -- <path>`
    -> command_result(...) (status label) -> refresh_all()

Any other mutating action (commit/checkout/create-delete-branch/fetch/pull/
push/merge/stash *) follows the same shape: run the git command, report via
command_result, refresh_all() on completion.
```

## 5. `GraphNode` Field Contract

See `docs/ui-elements.md`'s `GraphNode` entry for the full field table.
Summary of the design choice: one `GraphNode` instance renders *one row's*
local segment of the graph (its own dot + every line segment touching that
row, split into a top half and bottom half meeting at the dot), placed as
the leftmost cell of the commit `Table`'s each `TableRow`. This means:

- **No custom hit-testing.** Row selection is `Table`'s existing
  `row_selected`/`row_activated` (fired on the row's own `Selectable`, not
  per-cell) — `GraphNode` is a pure drawing widget with no events of its
  own.
- **Scrolling is free** from `Table`'s own `ImGuiTableFlags_ScrollY`
  handling — no separate canvas/scroll-sync code, and the graph column can
  never drift out of sync with the text columns next to it.
- Colors are packed `0xRRGGBBAA` `int32` (not `Label.text_color`'s
  `"#RRGGBBAA"` hex-string convention) because the per-segment arrays need
  a field type usable inside `int32[]`, and `bison::field` has no
  `string[]` alternative (see `bison_common.hpp`'s `field_base` variant) —
  the single `color` field matches that for internal consistency within
  this one element, a deliberate, narrowly-scoped deviation from the
  hex-string convention used elsewhere.

## 6. Design Decisions

- **`git_process::run_git()` is built on libuv (`uv_spawn`), not
  `bdg::bison::term::terminal`.** `terminal` (`extern/bison/src/term/`)
  exists purely for the interactive `--transport term` session: it spawns
  the child attached to a real pseudo-terminal (`forkpty()`/ConPTY), takes
  a single shell command *string* (not an argv array), and for its
  lifetime redirects the *calling process's own* stdout/stderr fds through
  a CRLF-translating pump. None of that is safe or appropriate for issuing
  many quick, argv-array `git <args>` invocations from inside a
  long-running `wish_client` process. Bison already vendors/builds libuv
  (`extern/bison/extern/libuv`, CMake target `uv_a`) for its own RMI
  transports but only links it `PRIVATE` into the `bison` target;
  `cmake/WishModules.cmake`'s `wish_finalize_app_modules()` now also links
  `uv_a` into the module-client targets (a small, wish-side-only CMake
  addition — see that function's comment) so `git_process.cpp` can use it
  directly, with **no bison submodule changes**. libuv already abstracts
  POSIX vs. Windows process creation, so (unlike `process_explorer`'s
  `process_info_linux.cpp`/`process_info_win.cpp` split) no `_posix`/`_win`
  file split was needed for this file.

- **`GIT_TERMINAL_PROMPT=0`, set once in the client process's own
  environment** (inherited by every spawned child, since `run_git()`
  leaves `uv_process_options_t::env` null) rather than passed per-call.
  Makes `fetch`/`pull`/`push` fail fast with a clear stderr message when
  credentials aren't already cached by the system's git credential helper/
  SSH agent, instead of hanging forever on a prompt this frontend has no
  UI for (see README.md's "Known limitations").

- **Commit messages/branch names/refs are passed as plain argv entries
  (e.g. `{"commit", "-m", message}`), never via a shell string or stdin.**
  Safe by construction since `run_git()` execs `git` directly through
  `uv_spawn`, with no shell in between — no escaping needed, no injection
  surface.

- **`dynamic::size()` gotcha: every dynamic children map needing generated
  numeric keys gets its own contiguous 0-based counter, never a counter
  shared across independent containers.** `bison::dynamic::size()`
  (`bison_object.hpp`) reports "highest numeric key + 1", not a true
  element count — a single `next_child_key_` shared across the sidebar's
  four sections plus the graph/files/diff tables (an early version of this
  module had exactly that) makes every container populated *after* the
  first in a given call over-report its row count once the shared counter
  has advanced past 0, even though rendering itself is unaffected (the
  renderer iterates actual entries via `for_each_child_ordered`, not a
  count-based loop) — caught by `tests/test_git.cpp`'s row-count
  assertions, not by manual/visual testing. `rebuild_section()` and
  `do_update_diff()` use a local counter (each fully rebuilds its own
  container in one call, so a fresh local variable is trivially correct);
  `rebuild_graph_table()` likewise uses a local counter; `add_file_row()`
  is the one exception — since it's called incrementally across a loop in
  its *callers* (`do_update_status`/`do_update_commit_files`), it needs a
  counter that persists across those calls but resets per rebuild, hence
  the one remaining member, `next_file_row_key_`, explicitly reset in
  `clear_file_rows()`.

- **wish's only built-in dialog form, `MessageBox` (`src/ui/forms/
  message_box.hpp`), is a genuine modal (`Window.modal = true`) but has no
  slot for custom body content** — just a title, message, icon, and a
  Win32-style OK/Cancel/Yes/No/Retry/Abort button preset — so it can't host
  a branch-name field or a branch picker. Branch creation uses a small,
  always-visible `InputText` next to the sidebar's BRANCHES header instead
  of a "New Branch" dialog; the merge target is whichever sidebar branch
  row was last clicked (`selected_branch_`), used by the toolbar's Merge
  button, instead of a target-picker dialog. Per-row secondary actions
  (delete branch, stash pop/apply/drop, ...) use a `MenuButton` (opens a
  popup of `MenuItem`s) at the end of each sidebar row — the existing wish
  equivalent of a right-click context menu, needing no new widget.

- **Destructive actions (delete branch, stash drop) are gated behind an
  inline confirm modal**, not fired directly from the `MenuItem` click.
  Rather than instantiating a standalone `MessageBox` object — which would
  need the *client* to mediate between two separate RMI objects (wait for
  its `on_result`, then tell `GitRepo` what to do) — `show_confirm()`
  builds a second modal `Window` directly inside `GitRepo`'s own tree,
  exactly mirroring `file_explorer::show_overwrite_confirm()`/
  `request_close_confirm()`/`remove_confirm_objects()`
  (`modules/bdg/desktop/file_explorer/server/file_explorer.cpp`): built via
  `import_json()`, ids assigned through `ctx()` (always valid), but session
  state (`ui_objects`/`top_level_objects`/`top_level_handlers`) touched via
  `context_wlock{*sync_ctx_}` rather than `sess()`, since `show_confirm()`
  is called from `on_event()`, which `form.hpp` documents as running
  *outside* dispatch (`sess()` would throw there). A single
  `std::function<void()> pending_confirm_action_` replaces
  `file_explorer`'s enum-typed `pending_transfer` branching, since
  `GitRepo` only needs one reusable "are you sure?" shape rather than
  distinct upload/download variants. "Apply"/"Pop" (reversible-ish) and
  "Merge into current"/"Checkout" stay un-confirmed, matching SourceTree's
  own convention of only gating truly destructive, hard-to-reverse actions.

- **A file's Selectable, not `Table`'s row-level `row_selected`, drives
  diff selection in the Files table.** The Files table's first column is
  an interactive `Checkbox` (stage/unstage) in working-tree mode; relying
  on `Table`'s own row-level hit-test for file selection risks competing
  with the checkbox's own click handling in the same row (untested/
  unspecified interaction in wish's `render_table()`). Giving the path
  cell its own `Selectable` (wired to `selectable_handlers_`, the same
  dispatch map the sidebar's branch rows use) sidesteps the ambiguity
  entirely — two independent leaf widgets in two cells, each with its own,
  unambiguous event, the same composition the sidebar already uses
  (`Selectable` + `MenuButton` in one row).

- **`selected_hash_ == ""` doubles as both "the synthetic working-tree row
  is selected" and "nothing selected yet" (the member's default).** A
  known minor architectural wart, not tightened to `std::optional<std::string>`
  in this pass — the only observable effect is that `select_row()`'s
  initial-selection logic doesn't fire an explicit `select_row(0)` call on
  a form's very first `update_log` (since the default `""` already
  "matches" the working row's `""` hash trivially), but `push_status()`
  populates the Files table directly regardless (via `status_mode_working_`
  defaulting `true`), so the observable behavior — Files populated, Diff
  empty until a file is explicitly clicked — is correct SourceTree-like
  UX anyway. Worth revisiting if a future change needs to distinguish
  "no selection" from "the working row" more explicitly.

## 7. Constraints and Invariants

- The server form never touches the filesystem or spawns a process;
  `git_process`/`git_repo_source` (client-only) own that entirely.
- Every `update_*` RMI handler must fully clear its owned subtree before
  repopulating it (Design Goal 4) — never append without first erasing
  prior rows, both to avoid visual duplication and to keep each affected
  children map's numeric keys a correct 0-based sequence (§6).
- `git_graph_layout.hpp`/`.cpp` must stay free of any UI/bison dependency —
  it is unit-tested (`tests/test_git_graph_layout.cpp`) as pure C++,
  independent of a running server/session.

## 8. Integration Boundaries

Depends on:
- `wish::form`, `ui_root::on_event`'s catch-all dispatch, `import_json()` —
  server-side UI construction, same pattern every other module uses.
- `GraphNode` (`src/ui/ui_elements/graph_node.cpp`, `src/imgui/
  imgui_graph_renderer.cpp`) — new, generic (not git-specific) core widget
  this module's development added; see `docs/ui-elements.md`.
- `Label.text_color` (existing field, added by the `editor` module) — diff
  line and file-status coloring; no new `Label` fields needed.
- `uv_a` (libuv, vendored by bison) — `git_process`'s subprocess helper;
  linked into module-client targets by `wish_finalize_app_modules()`
  (`cmake/WishModules.cmake`).
- The system `git` binary (must be on `PATH`) and the user's own git
  credential/SSH configuration for `fetch`/`pull`/`push`.

Depended on by: nothing else in wish; this is a leaf module.

## 9. Implementation Status

**Implemented** (verified both by `tests/test_git.cpp`/
`tests/test_git_graph_layout.cpp` and live, screenshot-verified end-to-end
runs via the automation module against a real fixture repository with a
branch + merge commit + uncommitted/untracked changes — see PLAN.md's
Verification section): commit graph with correct lane assignment and
branch/merge curve rendering; sidebar (branches with current-branch marker
and ahead/behind, remote-tracking branches, tags, stashes), each row's
`MenuButton` actions; toolbar (Commit/Push/Pull/Fetch/Branch/Merge/Stash/
Refresh); working-directory staging (checkbox -> `git add`/`git restore
--staged`, live-verified) and commit; diff viewer (working tree, staged,
untracked-file fallback, and per-commit diffs, live-verified with colored
+/- lines); branch checkout (including remote-tracking-branch auto-track
fallback)/create/delete; fetch/pull/push; fast-forward-first merge; stash
push/pop/apply/drop; background auto-refresh; an inline confirm modal
(`show_confirm()`, §6) gating delete-branch and stash-drop.

**Not implemented** (see PLAN.md for the full list and rationale):
interactive rebase, conflict-resolution UI, cherry-pick/revert/reset,
blame, submodules, LFS, hunk-level staging, external diff/merge tool
integration, commit search/filter, in-app credential prompts.
