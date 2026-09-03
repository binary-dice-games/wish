# kubectl

A Kubernetes-dashboard-style GUI frontend for the local `kubectl` command
line: dockable windows listing pods, deployments, services, and nodes, each
with per-row lifecycle actions, plus a pod log viewer and a formatted
`kubectl describe` panel. This is a pure frontend — every operation runs the
real `kubectl` binary already on the machine; it never speaks the Kubernetes
API directly.

`wish client --run=kubectl` (no positional args — it talks to whatever
cluster the `kubectl` CLI itself would: the current kubeconfig /
`KUBECONFIG` / current-context). Opens as independently dockable windows
(drag/resize/tab like any other wish window).

Refresh is **manual**: a Refresh button per window plus an automatic refresh
after every mutating action — no background polling (the `git` module's
window-jitter lesson, inherited via `docker`). Destructive actions
(**Delete**, **Drain**) are gated behind a `MessageBox` confirm; everything
else (rollout restart, cordon, uncordon, logs, describe) fires directly.

- **server/**: `KubectlFrontend` form (`register_kubectl()`) — renders
  whatever snapshot it was last given via `update_pods` /
  `update_deployments` / `update_services` / `update_nodes` / `update_logs`
  / `update_describe` / `append_command_log`, and emits `*_requested` events (see
  `server/kubectl.hpp`'s class doc comment for the full contract) for the
  client to react to by running the corresponding `kubectl` command.
- **client/**: `run_kubectl(wish_app_host&)`, self-registered as the
  `"kubectl"` embedded app — owns all `kubectl` invocation.
  `client/kubectl_process.hpp`/`.cpp` is a small, non-interactive,
  libuv-based (`uv_spawn`) "run this argv array, capture stdout/stderr/exit
  code" helper (a near-copy of the `docker` module's `docker_process`).
  `client/kubectl_source.hpp`/`.cpp` runs every actual `kubectl` command
  (tab-delimited `-o jsonpath` templates, split on `\t`), parses the output,
  and pushes snapshots / reacts to `*_requested` events. `run_kubectl()`
  gates on `kubectl version -o json` and prints a clear error if the cluster
  isn't reachable.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_KUBECTL=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).

See [DESIGN.md](DESIGN.md) for the full architecture and [PLAN.md](PLAN.md)
for what's implemented vs. deferred.

## Implementation status

All seven windows are implemented and unit-tested over `memory_transport`
(`tests/test_kubectl.cpp`, `tests/test_kubectl_process.cpp`):

- **Pods / Deployments / Services / Nodes** — each a toolbar + `Table` with
  a `...` per-row action menu (state-aware for nodes: Cordon on a
  schedulable node, Uncordon on a cordoned one), the Pods name / namespace
  text filters + phase Combo, and the same name / namespace filters on
  Deployments and Services.
- **Logs** — `kubectl logs --tail N --timestamps` in a scrolling pane; the
  "Follow" checkbox starts a 2 s client-side re-poll.
- **Describe** — `kubectl describe <kind>` output shown verbatim.
- **Console** — a scrolling, FIFO-capped table tracing every `kubectl`
  command the module ran (`#` / Command / Exit / Output), green on
  success, red on failure; right-click a row for "Copy Entry" or "Clear
  Console". (`git`'s "Log" window, renamed to avoid clashing with Logs.)

Plus a `kubectl version` startup gate and a `MessageBox` confirm for Delete
and Drain.

## Known limitations (v1)

- **No `kubectl apply` / edit / scale-to-N, no `exec` / `port-forward`, no
  events or CRD browsing, no context / namespace switcher.** Pods, etc. are
  always listed with `-A` (all namespaces) and filtered client-side.
- **The Logs "Follow" toggle is a client-side 2 s re-poll**, not true
  `kubectl logs -f` streaming, and shows a single container's logs (the
  default one `kubectl logs` picks).
- **`kubectl` must be on `PATH` with a working current-context.** No in-app
  auth/kubeconfig prompt.
- **Not yet verified end-to-end against a live cluster** — the automation
  workflow in `docs/automation.md` against a `kind` / `minikube` cluster is
  the remaining check (see [PLAN.md](PLAN.md)).

See [PLAN.md](PLAN.md) for the complete list of deferred features.
