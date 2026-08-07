# wish git Module — Implementation Plan

See [DESIGN.md](DESIGN.md) for the architecture this plan implements,
including the `GraphNode` core widget it introduced and the design
decisions behind the client/server split, the libuv-based subprocess
helper, and the no-modal-dialog interaction model.

## Steps

1. **`GraphNode` core widget** — `src/ui/ui_elements/graph_node.cpp`
   registration, `src/imgui/imgui_graph_renderer.{hpp,cpp}` render function,
   `docs/ui-elements.md` entry. ✅ Done.
   - Deliverables: the widget class + renderer, registered in
     `src/server/registry.cpp` and `imgui_renderer.cpp`'s dispatch table.
   - Tests: none dedicated yet (see "Not implemented" below) — exercised
     indirectly by `tests/test_git.cpp`'s graph-table row-count assertions
     and live automation screenshot verification.

2. **Module scaffold + subprocess plumbing** — `modules/bdg/dev/git/{server,client}/`,
   `register_git()`/`run_git()` boilerplate, `git_process` (libuv), the
   `uv_a` CMake link change in `cmake/WishModules.cmake`. ✅ Done.
   - Deliverables: `wish client --run=git -- /path/to/repo` opens a window
     and shows a flat commit list end-to-end.
   - Tests: `tests/test_git.cpp`'s `GitRepoLocalTest`/instantiation tests.

3. **Commit graph** — `git_graph_layout.hpp`/`.cpp` lane-assignment
   algorithm, `GraphNode` wiring into the graph table, sidebar skeleton,
   toolbar. ✅ Done.
   - Tests: `tests/test_git_graph_layout.cpp` (linear history, branch+merge
     diamond, octopus merge, empty input, lane-color determinism) — all
     passing. Live-verified: a real fixture repo's merge-commit diamond
     renders with correct lane/color/curve shape (screenshot-confirmed via
     the automation module).

4. **Working directory / staging** — `git status --porcelain=v1` parsing,
   staged/unstaged tables with stage/unstage checkboxes, commit message +
   commit. ✅ Done.
   - Tests: `tests/test_git.cpp`'s `UpdateStatusPopulatesFilesTable`.
     Live-verified: clicking a.txt's checkbox ran `git add`, the status
     label showed "stage: OK", and the checkbox visually updated after the
     following `refresh_all()`.

5. **Diff viewer** — `git diff`/`git diff --cached`/`git show` parsing,
   colored-line diff table (reusing `Label.text_color`, no new fields
   needed). ✅ Done.
   - Live-verified: clicking a modified file populated the Diff window
     with the real `git diff` output, correctly colored (header lines
     blue, the added line green).

6. **Branch/remote operations** — checkout/create/delete branch, fetch/
   pull/push, fast-forward-first merge, stash push/pop/apply/drop, per-row
   `MenuButton` actions. ✅ Done.

7. **Docs + tests** — `README.md`, `DESIGN.md`, this file, `tests/
   test_git.cpp`, `tests/test_git_graph_layout.cpp`, `tests/CMakeLists.txt`
   wiring. ✅ Done.

8. **Confirmation modal for destructive actions** — an initial pass wrongly
   assumed wish had no modal-dialog mechanism at all (it does:
   `Window.modal = true`, used by the built-in `MessageBox` form and by
   `file_explorer`'s own inline confirm-dialog precedent); delete-branch
   and stash-drop originally fired immediately with no confirmation as a
   result. Fixed: `show_confirm()` (see DESIGN.md §6) gates both behind an
   inline "are you sure?" modal built into `GitRepo`'s own tree. ✅ Done.

## Verification

- **Unit tests**: `cmake --build build --target test_git test_git_graph_layout`
  with `-DWISH_MODULE_BDG_DEV_GIT=ON`, then run both binaries directly.
  14/14 passing as of this pass.
- **End-to-end** (performed during this implementation, not just described):
  built with `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON
  -DWISH_MODULE_BDG_DEV_GIT=ON`; created a throwaway fixture repo (`git
  init`, a few commits, a `feature` branch, a `--no-ff` merge back into
  `master`, plus an uncommitted modification and an untracked file);
  launched `wish server --renderer web` and `wish client --run=git -- <repo>`;
  drove it with `wish.automation.AutomationClient` (screenshot +
  raw-pixel clicks, since dynamically-added rows have no dot-path to
  target by `click(path)` — see DESIGN.md §6 and the `editor` module's own
  same limitation for its event-log rows). Confirmed: the merge commit's
  diamond graph shape rendered correctly (dot colors, straight lines, and
  Bézier curves all matching `git log --oneline --graph --all`'s own
  topology); the sidebar showed both branches with the current one
  starred; clicking a file populated the Diff window with the real,
  correctly-colored `git diff` output; clicking a file's stage checkbox
  ran `git add` and the checkbox + status label updated correctly after
  the resulting refresh.

## Not implemented (deferred future work)

Explicitly out of scope for this pass (confirmed with the user before
implementation began, given SourceTree's very large total feature
surface):

- **Interactive rebase.** No UI for reordering/squashing/editing commits
  mid-rebase.
- **Conflict-resolution UI.** A non-fast-forward `git merge` that
  conflicts surfaces git's raw error text in the status label; resolving
  (or aborting) must happen outside this tool. No 3-way merge view.
- **Cherry-pick / revert / reset.** No UI entry points for any of the
  three, though the underlying `git_process::run_git()` helper could
  support them directly once event contracts are designed.
- **Blame.** No per-line authorship view.
- **Submodules / LFS.** No special handling; a repo using either will
  show their tracked files/pointers as ordinary files, with no
  submodule-aware status or LFS smudge/clean awareness in the UI.
- **Hunk-level staging.** Staging is whole-file only (`git add -- path`),
  not `git add -p`-style partial-hunk staging.
- **External diff/merge tool integration.** No `difftool`/`mergetool`
  launching; the built-in diff viewer is the only view.
- **Commit/branch search and filter.** The graph shows up to 300 most
  recent commits (`git log ... -n 300`) with no search box, branch
  filter, or "load more" pagination yet for repos with deeper history.
- **In-app credential prompts.** See DESIGN.md's design-decisions section
  on `GIT_TERMINAL_PROMPT=0` — a `fetch`/`pull`/`push` needing
  credentials neither a credential helper nor an SSH agent can supply
  fails with a clear error instead of hanging, but there is no UI to
  enter one.
- **Dedicated `GraphNode` renderer unit test.** `imgui_graph_renderer.cpp`
  is exercised indirectly (row construction correctness via
  `tests/test_git.cpp`, visual correctness via live automation
  screenshots) but has no `tests/test_imgui_renderer.cpp`-style direct
  draw-list-primitive-count assertion yet.
