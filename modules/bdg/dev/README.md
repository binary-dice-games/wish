# bdg/dev

wish's own bundled developer-tool modules — GUI frontends for developer
CLIs and workflows, in the same shape as the [bdg/desktop](../desktop/README.md)
collection and doubling as reference implementations for the
[module system](../../README.md). All off by default; enable the whole
collection with `-DWISH_COLLECTION_BDG_DEV=ON`, or individual modules with
their own `WISH_MODULE_BDG_DEV_<NAME>` option (see
[docs/building.md](../../../docs/building.md)).

Every tool here that shells out to a CLI runs it on a background worker
thread, one command at a time, so the UI never freezes. A command that takes
more than a moment (about 0.4 s) opens a modal progress dialog -- the command,
a progress bar, the tool's output as it arrives and a **Cancel** button. It
closes by itself on success and stays open on failure, showing the error,
until **Close** is pressed. The shared implementation is
[common/command_worker.hpp](common/command_worker.hpp) plus the built-in
`ProgressBox` form.

| Module | Description |
|--------|-------------|
| [editor](editor/README.md) | Live JSON/YAML UI mock editor: a syntax-highlighted source panel next to a continuously re-parsed preview, plus an event log and schema-aware autocomplete. The tool the `wish-module` / `wish-ui` skills use to preview a UI. |
| [docker](docker/README.md) | Docker Desktop-style GUI for the local `docker` CLI: dockable windows for containers/images/volumes/networks with per-row lifecycle actions, plus logs and inspect. Server owns the UI, client shells out to the `docker` binary. |
| [kubectl](kubectl/README.md) | Kubernetes-dashboard-style GUI for the local `kubectl` CLI: dockable windows for pods/deployments/services/nodes with per-row lifecycle actions, plus logs and describe. Server owns the UI, client shells out to the `kubectl` binary against the current kubeconfig context. |
| [helm](helm/README.md) | GUI for the local `helm` CLI: dockable windows for releases, chart repositories and chart search with per-row actions (status/values/manifest/history, rollback, uninstall, install, repo add/update/remove). Server owns the UI, client shells out to the `helm` binary against the current kubeconfig context. |
| [pip](pip/README.md) | GUI for the local `pip` CLI: dockable windows for the installed Python packages (filter, outdated check, per-row details/upgrade/reinstall/uninstall), an install bar (requirement or requirements file), and the versions the package index offers. Server owns the UI, client shells out to `python -m pip` for the chosen interpreter or virtualenv. |
| [pkg](pkg/README.md) | One GUI over the system package managers `apt`, `dnf`, `pacman` and `brew`, chosen by a command-line argument: installed packages (filter, update check, per-row details/upgrade/reinstall/remove), index search and install, upgrade all. Server owns a manager-agnostic UI, client runs the chosen manager's commands (through `sudo`/`pkexec` where root is needed). |
| [curl](curl/README.md) | Postman-style GUI for the local `curl` CLI: a request builder (params/headers/body/auth), a JSON-aware response viewer, History, Collections, and Environments. Server owns the UI, client shells out to the `curl` binary. |
| [sq](sq/README.md) | DBeaver-style, query-only GUI for the local [`sq`](https://github.com/neilotoole/sq) CLI: switch between database connections, browse tables/columns in a navigator tree, run SQL into a results grid, and export a result to CSV. Server owns the UI, client shells out to the `sq` binary. Non-SELECT SQL is rejected. |
| [git](git/README.md) | SourceTree-style git GUI: commit graph, staging/commit, diff viewer, branch/remote operations; server owns the UI, client shells out to the local `git` binary. |
