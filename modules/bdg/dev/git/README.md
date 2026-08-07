# git

A SourceTree-style git GUI for a local repository: commit graph with branch/
merge lines, a sidebar of branches/tags/stashes/remote-tracking branches,
working-directory staging and commit, a diff viewer, and branch/remote
operations (checkout, create/delete branch, fetch/pull/push, fast-forward
merge, stash push/pop/apply/drop). This is a pure frontend — it never
reimplements git internals, only shells out to the `git` binary already on
the machine.

`wish client --run=git -- /path/to/repo` opens the UI as three independently
dockable windows (drag/resize like any other wish window): **Git** (toolbar,
sidebar, commit graph), **Files** (staged/unstaged working-directory files,
or a selected commit's changed files), and **Diff** (the selected file's
diff, colored by line kind).

- **server/**: `GitRepo` form (`register_git()`) — renders whatever snapshot
  it was last given via `update_refs`/`update_log`/`update_status`/
  `update_commit_files`/`update_diff`, and emits `*_requested` events (see
  `server/git.hpp`'s class doc comment for the full contract) for the client
  to react to by running the corresponding git command. `server/
  git_graph_layout.hpp`/`.cpp` is the pure lane-assignment algorithm that
  turns a commit/parent DAG into the [`GraphNode`](../../../../docs/ui-elements.md#graphnode)
  field values that draw the graph's dots and branch/merge lines — the same
  `git log --graph`/gitk/magit technique, unit-tested independently in
  `tests/test_git_graph_layout.cpp`.
- **client/**: `run_git(wish_app_host&)`, self-registered as the `"git"`
  embedded app — owns all git invocation. `client/git_process.hpp`/`.cpp` is
  a small, non-interactive, libuv-based (`uv_spawn`) "run this argv array,
  capture stdout/stderr/exit code" helper (see its header comment for why
  bison's own `terminal` class isn't reusable here). `client/
  git_repo_source.hpp`/`.cpp` runs every actual git plumbing command,
  parses the output, and pushes snapshots / reacts to `*_requested` events.
  Requires a repository path via `app_args()`:
  `wish client --run=git -- /path/to/repo`.
- **resources/**: none.

See [DESIGN.md](DESIGN.md) for the full architecture and [PLAN.md](PLAN.md)
for what's implemented vs. deferred to future work.

## Known limitations (V1)

- **No in-app credential prompt.** `fetch`/`pull`/`push` rely entirely on
  the system's own git credential helper / SSH agent; a command needing
  interactive credentials fails fast (`GIT_TERMINAL_PROMPT=0`) instead of
  hanging, but there is no UI to enter a username/password/passphrase.
- **No conflict-resolution UI.** A non-fast-forward `git merge` that hits
  conflicts surfaces git's own error text in the status label; resolving
  conflicts (or aborting the merge) must be done outside this tool.
- **Branch creation and merge-target selection use inline controls, not a
  dialog.** wish's only built-in dialog form, `MessageBox`
  (`src/ui/forms/message_box.hpp`), is a genuine modal (`Window.modal =
  true`) but carries no slot for custom body content — just a title,
  message, icon, and a Win32-style button preset — so it can't host a
  branch-name field or a branch picker. Branch creation uses a small
  always-visible name field next to the sidebar's BRANCHES header instead;
  the merge target is the last-clicked sidebar branch — see
  `server/git.hpp`'s top-of-file comment. Destructive actions (delete
  branch, stash drop) *do* use a real modal — an inline confirm `Window`
  built into `GitRepo`'s own tree, mirroring `file_explorer`'s
  `show_overwrite_confirm()` pattern.

See [PLAN.md](PLAN.md) for the complete list of deferred features
(interactive rebase, cherry-pick/revert/reset, blame, submodules, LFS,
hunk-level staging, external diff/merge tools, search/filter).
