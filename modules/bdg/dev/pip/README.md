# pip

A GUI frontend for the local `pip` command line: a dockable window listing the
installed Python packages with per-row actions and an install bar, plus the
versions the package index offers for a package and a text pane for `pip
show` / `pip freeze` / `pip check` output. This is a pure frontend — every
operation runs the real `pip` already on the machine. Modelled on the sibling
[helm](../helm/README.md) module (same client/server split, same list-window
plumbing, same `MessageBox` confirm).

`wish client --run=pip [-- <python-or-venv>]`. The optional argument picks the
Python environment to manage: an interpreter (`/usr/bin/python3.12`,
`python3.13`) or a virtualenv directory (`.venv`). Without it the `python3`
(else `python`) on `PATH` is used — an activated virtualenv's when there is
one. pip is always run as `<interpreter> -m pip`, so the packages shown are
exactly that interpreter's. The Packages window's first status line names the
interpreter and pip version in use.

`pip` runs on a background worker thread behind the collection's shared
modal progress dialog (see the [bdg/dev README](../README.md)): a command
that takes more than a moment shows a progress bar, pip's output as it
arrives (`Collecting ...`, `Downloading ...`) and **Cancel**; it closes by
itself on success and stays open, showing the error, on failure.

Refresh is **manual**: a Refresh button plus an automatic refresh after every
mutating action — no background polling. **Uninstall** and **Reinstall** are
gated behind a `MessageBox` confirm; installs and upgrades fire directly.

- **server/**: `PipFrontend` form (`register_pip()`) — renders whatever
  snapshot it was last given via `update_packages` / `update_versions` /
  `update_details` / `set_environment` / `append_command_log`, and emits
  `*_requested` events (see `server/pip.hpp`'s class doc comment for the full
  contract) for the client to react to by running the corresponding `pip`
  command.
- **client/**: `run_pip(wish_app_host&)`, self-registered as the `"pip"`
  embedded app — owns all `pip` invocation. Commands run through
  the shared [common/](../common) helpers (`process.hpp`, `tool_source.hpp`)
  with `<python> -m pip` as the launcher. `client/pip_parsers.hpp`/`.cpp` parses `pip list --format=json`
  and `pip index versions` output and rejects flag-shaped values before they
  reach an argv. `client/pip_source.hpp`/`.cpp` runs every actual `pip`
  command on its worker thread and pushes snapshots / progress. `run_pip()`
  gates on `pip --version` and prints a clear error if pip can't be run.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_PIP=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).

## Windows

- **Packages** — `pip list`: Name / Version / Latest / Editable location,
  with a name filter and an All / Outdated / Editable Combo. The Latest column
  is filled by **Check for updates** (`pip list --outdated`, which queries the
  package index, so it only runs on request). **Freeze** and **Check** show
  `pip freeze` / `pip check` in Details. Row menu: Details (`pip show`), Files
  (`pip show -f`), Versions, Upgrade (`pip install --upgrade`), Reinstall
  (`pip install --force-reinstall --no-deps`), Uninstall (`pip uninstall -y`).
  - *Install new package*: type the exact package name (or one or more
    whitespace-separated requirements — `requests==2.31.0`,
    `git+https://...`, a path on the client machine). **Look up** lists the
    versions the index offers for it in the Versions window (and reports a
    name the index doesn't know); **Install** installs it.
  - *Or a requirements file*: the path of a requirements file on the client
    machine and **Install requirements** (`pip install -r`).
  - *Install options*: the `--upgrade` / `--user` / `--pre` checkboxes, applied
    to both.
- **Versions** — `pip index versions <name>`, newest first, marking the
  installed and the latest one. Row menu: Install this version
  (`pip install <name>==<version>`, which also downgrades).
- **Details** — the verbatim output of the last Details / Files / Freeze /
  Check request in a read-only text editor.
- **Console** — a scrolling, FIFO-capped table tracing every `pip` command
  the module ran (`#` / Command / Exit / Output), green on success, red on
  failure; right-click a row for "Copy Entry" or "Clear Console".

## Known limitations

- **Externally managed environments**: on a distribution Python marked
  `EXTERNALLY-MANAGED` (PEP 668; Debian, Ubuntu, ...), pip refuses to install
  or uninstall and the status line shows its
  `externally-managed-environment` error. Point the module at a virtualenv
  instead; there is deliberately no `--break-system-packages` switch.
- **Progress is indeterminate.** pip reports no overall percentage, so the
  bar only animates; the dialog's output log shows what pip is doing. Cancel stops pip the
  way Ctrl-C would: a half-finished install is not rolled back.
- **No package search**: PyPI no longer supports `pip search`. Type the exact
  name and use Versions.
- **`pip index` is needed for Versions** (pip 21.2 or newer; pip itself still
  labels the command experimental).
- No index / proxy / constraint options, no `pip download` / `wheel` /
  `cache` / `config`. pip's own configuration files and `PIP_*` environment
  variables still apply.
- A requirement cannot contain spaces (the box is split on whitespace), so
  write `requests>=2` rather than `requests >= 2`, and a local path with
  spaces cannot be installed from the install bar.
