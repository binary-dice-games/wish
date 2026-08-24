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
   client — mirrors `top`'s own reasoning for why sampling is
   client-side. The server never touches git, the filesystem, or a
   subprocess.
2. **The server owns all UI/render state** (selection, graph lane layout,
   sidebar structure) and renders whatever snapshot it was last given —
   matches `src/ui/forms/DESIGN.md`'s Design Goal 1 ("server-side logic,
   client-side data"), same split `top` uses for its own
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
   interactive desktop tool refreshing only on an explicit user action (a
   toolbar click or the Refresh button — see §6's "no background polling"
   entry), not a hot loop.

## 3. Key Abstractions

### `git_repo` (server, `form`)

Owns **four independently dockable `Window`s** (mirrors the `editor`
module's chrome/Help/Event-log split — `form::init()` only auto-registers
one top-level root, so the extra three are registered by hand in
`on_init()` exactly as `editor.cpp` does for its Help/Event-log windows):

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
- **Log** (`log_root_key_`): a trace of every `git` subprocess invocation
  the client makes (`do_append_command_log`), one `TableRow` per call —
  sequence #, full command, exit code, and a trimmed output preview,
  green/red-colored by success — for debugging/tracing the tool itself, not
  git repository state. FIFO-capped at `kMaxLogRows` (see §6).

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
`push_log` + `push_status`, in that order) — see Design Goal 4.
`refresh_all()` only ever runs in response to an explicit user action (a
mutating toolbar/menu click, or the Refresh button itself, which the
server maps straight to a `"refresh_requested"` event) — see §6's "no
background polling" entry for why an earlier ~2s background-poll thread
was removed rather than kept.

### `git_process::run_git()` (client)

Non-interactive `argv -> {exit_code, stdout, stderr}` helper built directly
on libuv (`uv_spawn`), **not** `bdg::bison::term::terminal`. See
`client/git_process.hpp`'s header comment and §6 below for why.

### `git_repo_source::run_logged()` (client)

Every `git` invocation `git_repo_source` makes goes through this thin
wrapper over `run_git()` rather than calling it directly: it runs the
command exactly as `run_git()` would, then also pushes a trace row (argv,
exit code, trimmed output preview) to `GitRepo`'s Log window via
`append_command_log`. This makes the Log window a complete trace of every
`git` process actually run — including the read-only ones inside
`refresh_all()` — not just the subset already user-facing via
`command_result` (which only reports the one mutating command behind each
`*_requested` event).

## 4. Data Flow / Architecture

```
Startup:
  run_git(host) -- requires app_args()[0] = repo path
    validate: `git rev-parse --is-inside-work-tree` (fail fast + signal_done() if not a repo)
    resolve_repo_root(app_args()[0]) -> repo_path (see §6's cwd-resolution entry)
    instantiate GitRepo -> proxy
    wire proxy.onEvent(...) for every *_requested event (see git.hpp)
    source->refresh_all()  -- explicit initial call, once every handler above is wired (see §6's
      "initial refresh race" entry for why this isn't done via an on_init()-emitted event instead)
    proxy.onEvent("refresh_requested", source->refresh_all())  -- fires again on every user click of
      the toolbar's Refresh button; no background polling after that (§6)

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
  POSIX vs. Windows process creation, so (unlike `top`'s
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

- **Destructive actions (delete branch, stash drop) are gated behind a
  confirm dialog**, not fired directly from the `MenuItem` click.
  `show_confirm()` privately instantiates the built-in `MessageBox` form
  (`form::instantiate_child_form()`, `src/ui/forms/message_box.hpp`) with a
  `"yes_no"` preset: the child builds and owns its own internal Window/
  buttons exactly as it would for a real client (its own `on_init()`,
  `on_event()`, closing itself on Close/window-X), and its `"on_result"`
  event (`{button: "yes"|"no"}`) is wired straight to an in-process
  `on_result` callback via `set_local_result_sink()` — no real client-side
  RMI round trip needed, and no second RMI object for the client to
  mediate. `confirm_dialog_` (a `std::shared_ptr<message_box>`) is the only
  state `GitRepo` keeps for it; `GitRepo::on_event()` no longer needs any
  confirm-specific branch, since the MessageBox handles its own Yes/No/
  close routing internally. Only one confirm dialog may be open at a time —
  a new `show_confirm()` call just overwrites `confirm_dialog_`, tearing
  down the stale instance. `confirm_label` (the caller's custom button
  caption, e.g. "Delete"/"Drop") no longer has anywhere to go, since
  `MessageBox`'s `"yes_no"` preset has fixed Yes/No labels — an accepted
  trade-off, since every caller's message text already says what's
  happening. "Apply"/"Pop" (reversible-ish) and "Merge into current"/
  "Checkout" stay un-confirmed, matching SourceTree's own convention of
  only gating truly destructive, hard-to-reverse actions.

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

- **`compute_git_graph_layout()`'s top-half segments are collapsed to a
  straight pass-through, not copied verbatim from the previous row's
  bottom half.** Adjacent rows share a border, so a lane-change segment
  crossing it (`{from_lane, to_lane}` with `from_lane != to_lane`) is drawn
  once, as a diagonal, in the row *above* it, where the lane change
  actually happens; by the time it reaches the row below, the line is
  already sitting at `to_lane`. An earlier version copied the bottom-half
  segment into the next row's top half unchanged, which drew the same
  diagonal a second time on top of a straight continuation — visible as a
  duplicated/jagged line at any commit whose parent's row diverges into a
  new lane (caught visually via an automation-driven screenshot, not by
  the existing unit tests, since `has_segment()`'s from/to check doesn't
  distinguish "one correct segment" from "two overlapping ones" — the fix
  only changed which `{from,to}` pair a segment reports, not whether one
  exists).

- **The Log window's row cap and eviction (`kMaxLogRows`, `log_row_entry`,
  `log_rows_`) exactly mirror the `editor` module's own event-log Table**
  (`editor.hpp`/`.cpp`'s `kMaxLogRows`/`log_row_entry`/`log_rows_`/
  `append_log_row()`) — a `std::deque` of "enough ids to fully erase this
  row" (its slot in the table's `children` map, plus every `ctx().objects`
  id `put_object()` assigned it), so a long-running session's trace stays
  bounded (500 rows) instead of leaking `ctx().objects` entries or growing
  the table without limit. Each `refresh_all()` call (an explicit Refresh
  click or the click behind any mutating action — see the "no background
  polling" entry below) makes several `run_logged()` calls at once
  (`push_refs` alone runs `for-each-ref`, `tag --list`, `stash list`,
  `rev-parse`, and one `rev-list` per local branch), so even purely
  user-driven usage can add up over a long session; the cap stays for that
  reason, not because of any longer-running background source of calls.

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

- **`graph_panel` (the `VerticalLayout` wrapping `current_branch_label` +
  `graph_table`, `kMainLayout` in `server/git.cpp`) must set `"height": -1`
  on itself, not just `"width": -1`.** Live-reproduced bug: the graph
  rendered as blank space (the `Table` never getting a real rect) even
  though `rebuild_graph_table()` was populating rows correctly and
  `update_log`/RMI dispatch were succeeding — a rendering-layer bug, not a
  data bug. Root cause: `graph_panel` is itself a child of the `"body"`
  `HorizontalLayout`, and `arrange_horizontal_layout()`'s cross-axis
  (height) sizing (`src/imgui/imgui_layout.cpp`) falls back to a
  no-height-hint child's own *measured/natural* height. `graph_table`
  already carries `"height": -1` (a stretch/fill hint, per the comment
  above `kMainLayout`), and `measure_vertical_layout()` deliberately counts
  such a stretch child as contributing 0 to its parent's natural height sum
  (it "wants to fill whatever's left over, not define it") — so without its
  own `"height": -1`, `graph_panel`'s measured height collapsed to just
  `current_branch_label`'s height, starving `graph_table` down to a few
  pixels. Fixed by adding `"height": -1` to `graph_panel`, matching the
  working pattern already used one module over:
  `modules/bdg/desktop/mc/server/mc.cpp`'s `"left"`/`"right"` panels both
  set `"width": -1, "height": -1` on the `VerticalLayout` wrapping their
  own fill-`Table`. **Invariant**: any `VerticalLayout`/`HorizontalLayout`
  that both (a) is a stretch/fill child on its own axis (`width`/`height`:
  `-1`) *and* (b) contains a fill-sized `Table` (or other stretch child) on
  the perpendicular axis needs an explicit `-1` hint on *that* axis too —
  omitting it silently starves the child to near-zero size rather than
  erroring, since `measure_vertical_layout()`/`measure_horizontal_layout()`
  have no way to signal "this natural size is meaningless, don't trust it
  for cross-axis sizing."

- **`do_update_diff()` must guard against a stale response the same way
  `do_update_commit_files()` already does, and the client must echo back
  enough of the request to check.** Reported bug: after selecting an older
  commit and clicking one of its changed files, the Diff window's title
  correctly updated to the file's path but the table body stayed
  completely empty. Live investigation (automation module, driven against
  this repo's own history — see `docs/automation.md`'s workflow) found the
  request/response round trip itself works correctly for a single,
  unhurried click (confirmed via a screenshot showing real, colored +/-
  lines for `1cab34ab` / `src/client/client.hpp`) — the actual defect is
  architectural, not a rendering or data-shape bug: `do_update_diff()` had
  **no staleness check at all**, unlike its sibling `do_update_commit_
  files()` (which already discards a response whose `hash` no longer
  matches `selected_hash_`). Worse, the client's `on_diff_requested()` only
  ever echoed back `path` in the `update_diff` args — not `hash`/`staged`
  — so even adding a guard needed a matching client-side change to give
  the server enough to compare against. `request_diff_for_selected()`
  re-reads `selected_hash_`/`selected_path_`/`selected_staged_` at the
  moment a file's `Selectable` is clicked, but the client's `git show`/
  `git diff` subprocess and the RMI round trip both take real wall-clock
  time; without a guard, a response for a selection the user has since
  navigated away from (a different file, a different commit, or back to
  the working tree) can arrive **after** a fresher response and silently
  overwrite what's currently displayed with stale or mismatched content —
  demonstrated deterministically in `tests/test_git.cpp`'s
  `StaleDiffResponseIgnoredAfterSelectionChanges` (calls `update_diff`
  out of order via the RMI test harness, bypassing real subprocess/network
  timing entirely) rather than by trying to win a live timing race, which
  proved unreliable to force purposefully through browser automation.
  Fixed by having the client echo `hash`/`staged` back in the
  `update_diff` args and having `do_update_diff()` discard the call
  (return without touching `diff_title_label_`/`diff_table_`) whenever
  `path`/`hash`/`staged` no longer all match the current selection —
  exactly mirroring `do_update_commit_files()`'s existing pattern.
  **Invariant**: any RMI method that repopulates UI state in response to
  an event whose payload was captured *before* an async client round trip
  (not just this one — `do_update_commit_files()`/`do_update_diff()` are
  the two instances so far) must echo back enough of that original request
  in its response args to let the server verify the response still matches
  current selection state before applying it; a response that doesn't
  match must be silently discarded, never applied "because it's the only
  data we have."

- **The repo path argument must be resolved to its actual `git
  rev-parse --show-toplevel` before being used as every git subprocess's
  `cwd` — using it verbatim (`client/git.cpp`'s old behavior) breaks every
  file-specific diff whenever wish itself is launched from anywhere other
  than the repo root.** This is the bug the staleness-guard fix above
  did *not* actually fix, despite initially looking like the same symptom
  (title populates, diff body stays empty) — the guard fix was a real,
  independent correctness issue, confirmed via a deterministic regression
  test, but it wasn't what the user kept hitting. The user's own repro —
  `./wish standalone --run git --theme light -- .`, launched from inside
  `build/app/` rather than the repo root — only reproduced once launched
  the same way here: `git_repo_source::run_logged()` passes whatever
  `repo_path` the client was constructed with straight through to
  `run_git()`'s `cwd`, and prior to this fix `repo_path` was `s.app_args()
  [0]` **unresolved** — literally `"."` in this case, relative to
  wherever the wish *process* itself happened to be launched from, not
  the repo root. `git status`/`log`/`show --name-status` all report paths
  relative to the repo root *regardless* of cwd (verified directly:
  `git status --porcelain` from a subdirectory still prints repo-root-
  relative paths), so the sidebar, graph, and Files panel all looked
  completely correct — but `git diff -- <path>` / `git show <hash> --
  <path>` (`on_diff_requested()`) resolve that same repo-root-relative
  pathspec **relative to cwd**, so unless cwd happens to equal the repo
  root exactly, the pathspec matches nothing: `git diff` silently returns
  empty stdout (no error), `git show` fails outright with `fatal:
  pathspec '<path>' did not match any file(s) known to git` — the exact
  stderr text visible in the tool's own status label at one point during
  this investigation. Fixed by extracting `git_process::resolve_repo_root()`
  (`client/git_process.hpp`/`.cpp`) — runs `git rev-parse --show-toplevel`
  against whatever path was given and returns its absolute output (falling
  back to the input path if that fails) — and calling it once in
  `run_git(wish_app_host&)` right after the existing `--is-inside-work-tree`
  check, before constructing `git_repo_source`. Regression-tested in
  `tests/test_git_process.cpp` against a real, throwaway `git init`'d repo
  (this module never mocks git — Design Goal 1) rather than against the
  RMI layer, since the bug lived entirely in client startup wiring, not in
  `GitRepo`'s server-side logic. **Lesson for live-verifying this module
  going forward**: always launch it from more than one cwd during
  verification (repo root *and* an unrelated subdirectory) — a repro run
  only from the repo root cannot distinguish a cwd-resolution bug like
  this one from a genuinely fixed diff path, since the two are
  indistinguishable exactly when cwd == repo root.

- **No background polling — every refresh is user-initiated.** An earlier
  version had `client/git.cpp` spawn a detached background thread calling
  `refresh_all()` on a ~2s cycle (mirroring `top`'s own sampling loop) so
  external changes to the working tree showed up without an explicit
  Refresh click. Removed at the user's request: on a real repo the ~2s
  `git status`/`git log` cycle was visibly reformatting the Main window
  every tick — enough to shift focus and cause layout "vibration" while
  the user was mid-interaction (typing a commit message, clicking a
  sidebar row) — and flooded the Log window with hundreds of trace rows
  from calls the user never asked for, several times faster than the
  actual per-click Log rationale above assumes. `refresh_all()` now runs
  *only* in direct response to a `"refresh_requested"` event: once from
  `on_init()` (the initial population) and once per explicit Refresh
  button click, plus implicitly at the end of every mutating action
  (stage/commit/checkout/fetch/pull/push/merge/stash — see
  `git_repo_source::run_and_refresh()`), same as before. There is no
  longer any timer, thread, or `std::atomic` stop flag in `client/git.cpp`
  — picking up an out-of-band change to the working tree (an edit made
  outside this tool) now requires clicking Refresh, a deliberate trade-off
  of staleness-between-clicks for a stable, non-jittery UI. If background
  refresh is ever wanted again, it would need to be considerably less
  frequent and/or debounced against active user input, not simply
  reintroduced at the old cadence.

- **Initial-load race, exposed by removing the background poll above:
  `GitRepo::on_init()` emitting `"refresh_requested"` was not actually how
  the tool's first population ever worked.** `on_init()` runs synchronously
  as part of the server handling `instantiate()` — its `emit()` call fires,
  and is gone, before `instantiate(...).get()` even returns to the client,
  let alone before `run_git()`'s `proxy->onEvent("refresh_requested", ...)`
  registration a few lines later runs. Live-verified after removing the
  background-poll thread: with nothing else triggering a refresh, the app
  opened to a *permanently* empty graph, sidebar, Files panel, and Log
  window — confirmed by polling `get_tree()` for 10s with zero user
  interaction and seeing row counts stay at 0 the entire time. This exact
  gap existed before this pass too; it just went unnoticed because the old
  background thread's first tick (~2s after startup) reliably repainted
  real data before anyone looked, making the tool merely *feel*
  instant-on. Fixed by having `run_git()` (client) call `source->
  refresh_all()` directly, once, as the last thing it does after every
  `onEvent()` handler is registered — no server-emitted event, no race
  window at all. `on_init()` no longer emits `"refresh_requested"`; the
  event still exists for the toolbar's Refresh button (`bind_click(...,
  [this] { emit("refresh_requested"_key); })`), which fires it well after
  the client has long since finished wiring its handlers.

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
push/pop/apply/drop; user-initiated refresh only, no background polling
(§6); a MessageBox confirm dialog
(`show_confirm()`, §6) gating delete-branch and stash-drop; a Log window
tracing every `git` subprocess invocation (`run_logged()`/
`append_command_log`/`do_append_command_log`, live-verified showing
sequence #, command, exit code, and colored output preview for a real
refresh cycle).

**Not implemented** (see PLAN.md for the full list and rationale):
interactive rebase, conflict-resolution UI, cherry-pick/revert/reset,
blame, submodules, LFS, hunk-level staging, external diff/merge tool
integration, commit search/filter, in-app credential prompts.
