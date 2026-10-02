# helm

A GUI frontend for the local `helm` command line: dockable windows listing
releases, chart repositories, and chart search results, each with per-row
actions, plus a release revision history and a text pane for `helm status` /
`get values` / `get manifest` / `get notes` / `show values` / `show readme`
output. This is a pure frontend — every operation runs the real `helm` binary
already on the machine; it never speaks the Kubernetes API directly. Modelled
on the sibling [kubectl](../kubectl/README.md) module (same client/server
split, same list-window plumbing, same `MessageBox` confirm).

`wish client --run=helm` (no positional args — it manages whatever the `helm`
CLI itself would: the current kubeconfig / `KUBECONFIG` / current-context and
the user's own repository configuration). Opens as independently dockable
windows (drag/resize/tab like any other wish window).

Refresh is **manual**: a Refresh button plus an automatic refresh after every
mutating action — no background polling. Destructive actions (**Uninstall**,
**Rollback**, repository **Remove**) are gated behind a `MessageBox` confirm;
**Install** and **Upgrade** go through their own dialog; everything else
fires directly.

The module is meant to cover day-to-day helm use without a terminal: add a
repository, browse its charts, install one (optionally with custom values),
inspect / upgrade / roll back / uninstall the release.

- **server/**: `HelmFrontend` form (`register_helm()`) — renders whatever
  snapshot it was last given via `update_releases` / `update_repos` /
  `update_charts` / `update_history` / `update_details` /
  `set_install_values` / `append_command_log`, and emits `*_requested` events (see
  `server/helm.hpp`'s class doc comment for the full contract) for the client
  to react to by running the corresponding `helm` command.
- **client/**: `run_helm(wish_app_host&)`, self-registered as the `"helm"`
  embedded app — owns all `helm` invocation.
  `client/helm_process.hpp`/`.cpp` is a small, non-interactive, libuv-based
  (`uv_spawn`) "run this argv array, capture stdout/stderr/exit code" helper
  (a near-copy of the `kubectl` module's). `client/helm_table_parser.hpp`/
  `.cpp` parses helm's table output (columns are TAB-separated and
  space-padded) and rejects flag-shaped values before they reach an argv.
  `client/helm_source.hpp`/`.cpp` runs every actual `helm` command and pushes
  snapshots / reacts to `*_requested` events. `run_helm()` gates on
  `helm version --short` and prints a clear error if `helm` isn't installed.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_HELM=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).

## Windows

- **Releases** — `helm list -A` (every status): Namespace / Name / Revision
  / Status (green deployed, red failed, amber pending or uninstalling) /
  Chart / App Version / Updated, with name and namespace text filters and a
  status Combo. Row menu: Status, Values, Manifest, Notes, History, Upgrade,
  Rollback (to the previous revision), Uninstall.
- **Repositories** — `helm repo list`: Name / URL, with a name filter, an
  "Update all" button and an inline name + URL row for `helm repo add`. Row
  menu: Update, Remove. Any change re-runs the Charts search.
- **Charts** — `helm search repo <query>`: Name / Chart Version / App Version
  / Description. Every chart of the configured repositories is listed on
  startup (an empty query lists all; at most 500 rows are shown). Row menu:
  Values, Readme, Install. The "Install chart..." button opens the same
  dialog empty, for a chart that is not in a repository.
- **Install / Upgrade dialog** — a floating window opened by a chart's
  Install or a release's Upgrade: chart (`repo/name`, `oci://...`, a URL, or
  a path on the client machine), version, release name, namespace, "Create
  namespace", "Wait until ready", and a YAML values box. "Load chart
  defaults" fills the box with `helm show values`; an upgrade opens with the
  release's current user-supplied values. Submitting runs `helm install` /
  `helm upgrade <release> <chart> -n <namespace> [--version] [-f <values>]
  [--create-namespace] [--wait]`; a failure is shown in the dialog, which
  stays open for a retry.
- **History** — `helm history` for the release picked from the Releases row
  menu. Row menu: Rollback to this revision.
- **Details** — the verbatim output of the last Status / Values / Manifest /
  Notes / chart Values / chart Readme request in a read-only, syntax-
  highlighted text editor.
- **Console** — a scrolling, FIFO-capped table tracing every `helm` command
  the module ran (`#` / Command / Exit / Output), green on success, red on
  failure; right-click a row for "Copy Entry" or "Clear Console".

## Known limitations

- **An upgrade needs the chart typed in**: `helm list` reports a release's
  chart as `<name>-<version>`, not as an installable reference. An upgrade
  applies exactly the values in the box (helm's default, no
  `--reuse-values`).
- **No `--set`** (use the values box), no `helm template` / `lint` /
  `package` / `dependency`, no OCI registry login, no `helm search hub`, no
  context / namespace switcher. Releases are always listed across all
  namespaces and filtered client-side; `helm list` returns at most 256
  releases (its own default `--max`).
- **Progress is indeterminate.** helm reports no percentage, so the progress
  dialog's bar only animates; its log shows helm's output. Cancel stops helm
  the way Ctrl-C would -- a half-finished install is not rolled back.
- **`helm` must be on `PATH`.** The startup gate only checks that the binary
  runs; if the cluster is unreachable, the Releases window's status line
  shows helm's error while Repositories and Charts keep working.
- **Install, upgrade, rollback and uninstall have not been run against a
  live cluster.** Listing, repository add, chart search and `show values`
  were exercised against helm 4.3 through the automation module; the
  mutating release commands only against a script emulating `helm`, plus the
  unit tests (`tests/test_helm.cpp`, `tests/test_helm_process.cpp`).
