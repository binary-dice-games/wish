# wish kubectl Module — Architecture & Design

**Status: implemented, unit-tested, not yet live-verified against a
cluster.** All seven windows (Pods / Deployments / Services / Nodes / Logs /
Describe / Console) are built and covered by `tests/test_kubectl.cpp` over
`memory_transport`. See [PLAN.md](PLAN.md) and "10. Implementation Status".

The **Console** window is a FIFO-capped `Table` (# / Command / Exit /
Output) tracing every one-shot `kubectl` invocation the client ran, colour-
coded green/red — `git`'s "Log" window, renamed to avoid clashing with the
pod **Logs** window. Fed by the `append_command_log` RMI method (client-side
choke point: `kubectl_source::run_logged()`). The Logs "Follow" 2 s re-poll
thread is deliberately *not* traced (it would flood the window).

This module is a close sibling of the `docker` module
([modules/bdg/dev/docker/DESIGN.md](../docker/DESIGN.md)) — same client /
server split, same generic list-window plumbing, same `MessageBox` confirm,
same tab-delimited-CLI-output parsing. Read that document first; this one
only records where `kubectl` differs from `docker`.

## 1. Purpose / Scope

The `kubectl` module (`wish client --run=kubectl`) is a Kubernetes-dashboard
-style GUI frontend for the local `kubectl` command line: dockable windows
listing pods, deployments, services, and nodes, each with per-row lifecycle
actions, plus a pod log viewer and a formatted `kubectl describe` panel. It
is a **pure frontend** — every operation runs the real `kubectl` binary
already installed on the machine; this module never speaks the Kubernetes
API directly, never links `client-go`, and never reads a kubeconfig itself.

Scope for this pass:

- **Resource types**: pods, deployments, services, nodes (four cheap
  `kubectl get` surfaces). ReplicaSets / StatefulSets / DaemonSets /
  Jobs / ConfigMaps / Secrets / Ingresses / PVCs / CRDs are out.
- **Refresh model**: an explicit Refresh button per window plus an automatic
  refresh after every mutating action — **no background polling** (inherited
  from `docker`, which inherited it from `git`'s window-jitter regression).
- **Detail views**: a Logs window (`kubectl logs`, with a re-poll "Follow"
  toggle) and a Describe window (`kubectl describe`, verbatim). **No
  `exec` / `port-forward`** (both need PTY / socket streaming over RMI).
- **Confirmation**: destructive actions (delete / drain) are gated behind
  the built-in `MessageBox` form.
- **Namespaces**: every list is fetched with `-A` (all namespaces);
  namespace is a column and a client-side filter, never a re-query. There is
  no context / namespace switcher.

Explicitly deferred — see §10 and [PLAN.md](PLAN.md): `kubectl apply` /
`edit`, scale-to-N, `exec` / `port-forward`, `kubectl top` (metrics),
`kubectl get events`, rollout history / undo, CRD browsing, context and
namespace switching.

This directory owns:

- `server/kubectl.hpp`/`.cpp` — the `KubectlFrontend` form (seven windows,
  the `update_*` render methods, the `append_command_log` Console trace
  method, the `*_requested` events).
- `client/kubectl.hpp`/`.cpp` — the client runner (`run_kubectl`, event
  wiring, app registration).
- `client/kubectl_process.hpp`/`.cpp` — a non-interactive libuv-based "run
  `kubectl <args>`, capture output" helper (near-verbatim copy of
  `docker_process`).
- `client/kubectl_source.hpp`/`.cpp` — every actual `kubectl` invocation,
  the tab-delimited `-o jsonpath` output parsing plus the small amount of
  client-side humanization `kubectl` doesn't do in that mode (§6), and the
  snapshot push / event reactions.
- `README.md` — user-facing usage.
- `kubectl_mock.json` / `kubectl_mock.html` — the UI mockup (all six windows
  as one tabbed window) for review in the `editor` tool.

## 2. Design Goals

Identical to `docker`'s five goals (client-side CLI invocation, server owns
UI/render state, no shell in any invocation, full rebuild per `update_*`,
destructive-only confirmation) — see
[../docker/DESIGN.md §2](../docker/DESIGN.md). The one addition:

6. **All list queries are `-A` + client-side filter, never per-namespace
   re-queries.** A namespace text box in each list window's toolbar filters
   the already-fetched rows (the same mechanism as the name filter and the
   Pods phase Combo), so switching what you're looking at never costs a
   round trip and the "refresh is a full rebuild" invariant is unaffected.

## 3. Key Abstractions

### `KubectlFrontend` (server, `form`)

Bison class `"KubectlFrontend"` in the `"wish"` namespace. Owns **seven
independently dockable `Window`s**, built exactly as `docker.cpp` builds
its: `build_list_window()` for the four `list_window`s (Pods is
`internal_root_key_`, the rest are `internal_root_key_ + "_deployments" /
"_services" / "_nodes"`), `build_text_window()` for Logs / Describe /
Console (`_logs` / `_describe` / `_console`). The `...` menu dispatch keys on a `{scope, name,
ns, action}` `row_action`, so one `on_event()` clause routes every window's
actions.

Per-window specifics:

- **Pods** (`internal_root_key_`, the main window): toolbar (Refresh, a name
  filter `InputText`, a namespace filter `InputText`, a phase `Combo`
  All/Running/Pending/Succeeded/Failed), a status `Label`, and the pods
  `Table` (Namespace, Name, Ready, Status, Restarts, Age). The Status cell
  is colour-coded — green Running, amber Pending / *Creating* / *Init*, red
  Failed / *CrashLoopBackOff* / *Err\** / *ImagePull\**, gray
  Succeeded/Completed/Unknown — and shows the container waiting reason
  (`CrashLoopBackOff`, …) instead of the bare phase when there is one. Row
  menu: Logs / Describe / Delete (confirm).
- **Deployments**: toolbar (Refresh, name + namespace filters), `Table`
  (Namespace, Name, Ready, Up-to-date, Available, Age). Ready is green when
  `have == want != 0`, amber otherwise. Row menu: Restart (`rollout
  restart`, not gated — it self-recovers, matching `docker`'s Restart) /
  Describe / Delete (confirm).
- **Services**: toolbar (Refresh, name + namespace filters), `Table`
  (Namespace, Name, Type, Cluster-IP, Ports, Age). Row menu: Describe /
  Delete (confirm).
- **Nodes**: toolbar (Refresh, name filter — nodes are cluster-scoped, no
  namespace), `Table` (Name, Status, Version, Age). Status is green Ready,
  red NotReady, amber when `,SchedulingDisabled`. Row menu is state-aware:
  Cordon on a schedulable node, Uncordon on a cordoned one; then Drain
  (confirm) and Describe.
- **Logs** (`_logs`): toolbar (a target `Label`, a Follow `Checkbox`, a
  Lines `InputInt`, Refresh), a read-only single-column scrolling `Table` of
  `Label` lines showing `kubectl logs` output.
- **Describe** (`_describe`): toolbar (a target `Label`, Refresh), the same
  read-only line table showing `kubectl describe` output verbatim.
- **Console** (`_console`): a FIFO-capped `Table` (# / Command / Exit /
  Output, `kMaxConsoleRows = 500`) tracing every one-shot `kubectl`
  invocation, green/red by exit status. Each row's right-click
  `ContextMenu` offers "Copy Entry" and "Clear Console". `git`'s "Log"
  window, renamed to avoid clashing with **Logs**. Fed only by
  `append_command_log`.

Closing any window emits `"closed"` and tears down all seven subtrees
(`remove_objects_at` for the six extra roots + `remove_internal_objects` —
`docker.cpp`'s `on_event` close branch).

### `kubectl_source` (client)

Owns the proxy and every actual `kubectl` invocation. `refresh_all()` runs
the four `kubectl get … -A -o jsonpath` snapshots (`push_pods` /
`push_deployments` / `push_services` / `push_nodes`) and calls the matching
`update_*` RMI method. One method per `*_requested` event, each running a
mutating `kubectl` command via `run_and_refresh()` (which reports the
outcome via `command_result` and then calls `refresh_all()`). `on_logs_requested`
/ `on_describe_requested` run their read-only command and call `update_logs`
/ `update_describe` directly. Structurally identical to
`docker_source` / `git_repo_source`.

Every one-shot `kubectl` invocation goes through `run_logged()` (over
`run_kubectl_cli()`), which — after running the command — pushes one
`append_command_log` trace row (command, exit code, `ok`, 200-char output
preview) to the **Console** window; `push_list()` calls the same
`push_command_log()` helper inline. The one exception is the Logs "Follow"
2 s re-poll thread, which calls `run_kubectl_cli()` directly so it does not
flood the Console (`git`'s Log-window lesson). Mirrors
`docker_source::run_logged()`.

### `kubectl_process::run_kubectl_cli()` (client)

Byte-for-byte the `docker_process::run_docker_cli()` design: `uv_loop_init`
→ prepend `binary` (default `"kubectl"`) as argv[0] → stdout/stderr pipes →
`uv_spawn` → `uv_run` (blocking) → collect exit code. The `binary` parameter
exists only so `test_kubectl_process` can drive it with `printf` / `false`
on a machine with no cluster. `uv_a` is already linked into every
module-client target — no CMake change.

## 4. Data Flow / Architecture

Same shape as [../docker/DESIGN.md §4](../docker/DESIGN.md). The `kubectl`
list templates (one TAB-delimited `-o jsonpath` range per `get`, split on
`\t` line by line):

```
push_pods()        -> kubectl get pods -A -o jsonpath=
                        '{range .items[*]}{.metadata.namespace}\t{.metadata.name}\t{.status.phase}\t
                          {.status.containerStatuses[*].ready}\t{.status.containerStatuses[*].restartCount}\t
                          {.status.containerStatuses[*].state.waiting.reason}\t{.metadata.creationTimestamp}\n{end}'
                     -> KubectlFrontend.update_pods({pods:[...]})
push_deployments() -> kubectl get deployments -A -o jsonpath=
                        '...namespace\tname\t{.status.readyReplicas}\t{.spec.replicas}\t
                          {.status.updatedReplicas}\t{.status.availableReplicas}\t{creationTimestamp}...'
                     -> update_deployments({deployments:[...]})
push_services()    -> kubectl get services -A -o jsonpath=
                        '...namespace\tname\t{.spec.type}\t{.spec.clusterIP}\t{.spec.ports[*].port}\t{creationTimestamp}...'
                     -> update_services({services:[...]})
push_nodes()       -> kubectl get nodes -o jsonpath=
                        '...{.metadata.name}\t{.status.conditions[?(@.type=="Ready")].status}\t{.spec.unschedulable}\t
                          {.status.nodeInfo.kubeletVersion}\t{creationTimestamp}...'
                     -> update_nodes({nodes:[...]})
```

`kubectl_source` fills each row's dynamic from the `\t`-split columns,
applying the client-side humanization `kubectl` skips in jsonpath mode
(§6): `age` from `creationTimestamp`, `ready` "1/1" from the
space-separated `.ready` booleans, `restarts` total from the
space-separated `.restartCount`s, and the first container waiting `reason`.

Mutating reactions:

| event | `kubectl` argv |
|---|---|
| `pod_action_requested {delete}` | `delete pod <name> -n <ns>` |
| `deployment_action_requested {restart}` | `rollout restart deployment <name> -n <ns>` |
| `deployment_action_requested {delete}` | `delete deployment <name> -n <ns>` |
| `service_action_requested {delete}` | `delete service <name> -n <ns>` |
| `node_action_requested {cordon|uncordon}` | `cordon <name>` / `uncordon <name>` |
| `node_action_requested {drain}` | `drain <name> --ignore-daemonsets --delete-emptydir-data --force` |
| `logs_requested` | `logs <name> -n <ns> --tail <lines> --timestamps` |
| `describe_requested` | `describe <kind> <name> [-n <ns>]` |

`update_logs` / `update_describe` echo `name` + `namespace` (+ `kind` for
describe) and the server drops the call if they no longer match the window's
open target (`docker`'s `do_update_logs` staleness guard).

## 5. Public API Contract

| Symbol | Contract |
|---|---|
| `KubectlFrontend.update_pods(args)` | RMI. `args.pods` — dynamic array, each `{ namespace, name, ready ("1/1"), phase, reason (container waiting reason or ""), restarts (string), age (string) }`. Fully rebuilds the pods table. |
| `KubectlFrontend.update_deployments(args)` | `args.deployments` — each `{ namespace, name, ready ("2/3"), uptodate, available, age }`. |
| `KubectlFrontend.update_services(args)` | `args.services` — each `{ namespace, name, type, cluster_ip, ports, age }`. |
| `KubectlFrontend.update_nodes(args)` | `args.nodes` — each `{ name, status ("Ready"/"NotReady"[,SchedulingDisabled]), schedulable ("true"/"false"), version, age }`. |
| `KubectlFrontend.update_logs(args)` | `{ name, namespace, title, text }`. `name`/`namespace` echoed from `logs_requested`; discarded if they no longer match the Logs window's open target. |
| `KubectlFrontend.update_describe(args)` | `{ kind, name, namespace, title, text }`. Same staleness guard on all three of `kind`/`name`/`namespace`. |
| `KubectlFrontend.command_result(args)` | `{ command (string), ok (bool), output (string, shown on failure), scope ("pods"/"deployments"/"services"/"nodes", default "pods") }`. Writes that window's status `Label` green/red. |
| `KubectlFrontend.append_command_log(args)` | `{ command (string), exit_code (int32), ok (bool), output (string, single-line preview) }`. Appends one row to the **Console** window's trace table, green/red by `ok`; FIFO-capped at 500 rows. |
| `"refresh_requested"` event | No payload. Client re-runs all four `kubectl get` snapshots. |
| `"pod_action_requested"` event | `{ name, namespace, action }` — `action` ∈ `delete`. |
| `"deployment_action_requested"` event | `{ name, namespace, action }` — `action` ∈ `restart`, `delete`. |
| `"service_action_requested"` event | `{ name, namespace, action }` — `action` ∈ `delete`. |
| `"node_action_requested"` event | `{ name, action }` — `action` ∈ `cordon`, `uncordon`, `drain`. No `namespace` (cluster-scoped). |
| `"logs_requested"` event | `{ name, namespace, follow (bool), lines (int32) }`. |
| `"describe_requested"` event | `{ kind, name, namespace }` — `kind` ∈ `pod`, `deployment`, `service`, `node`. |
| `"closed"` event | Any window's X was clicked; all seven subtrees torn down. The client should `signal_done()`. |
| `wish client --run=kubectl` | No positional args — the module talks to whatever cluster the `kubectl` CLI itself would (current kubeconfig / context). |

The internal `ui_element` tree of every window is private.

## 6. Design Decisions

Everything in [../docker/DESIGN.md §6](../docker/DESIGN.md) applies verbatim
(libuv over `terminal`, the `binary` test parameter, no background polling,
full clear-and-rebuild, per-rebuild-local row-key counter, `MessageBox` for
destructive actions, inline toolbar fields, client-side "Follow" re-poll,
the logs/describe staleness guard, theme-paired colours, separate dockable
windows, the plain line-table text viewer, the `version` startup gate, and
the **Console** subprocess-trace window fed by `run_logged()` /
`append_command_log`). The `kubectl`-specific decisions:

- **A tab-delimited `-o jsonpath` template per list, split on `\t` — not
  `-o json` and no JSON library.** `docker` chose a Go `--format` template
  for the same reason: the JSON path (`nlohmann::json`) is `PRIVATE` to a
  handful of wish targets and is *not* propagated to module-client sources.
  `kubectl get … -o jsonpath='{range .items[*]}…{"\t"}…{"\n"}{end}'` is the
  direct analog — one tab-separated line per object, fields chosen so none
  can contain a tab or newline. `kubectl describe`'s output is shown
  verbatim as text, never parsed.

- **Unlike `docker`'s `--format`, `kubectl` jsonpath does *not* pre-humanize
  the derived columns, so `kubectl_source` does a small amount itself.**
  `docker` gets `.Status` = "Up 3 hours" and `.Size` = "431MB" straight from
  the CLI. `kubectl` jsonpath gives only raw API fields, so the client
  computes: `age` from `.metadata.creationTimestamp` (RFC3339 → "3h"/"2d"
  short form, kubectl's own `ShortHumanDuration` style), `ready` "1/2" by
  counting the space-separated `.status.containerStatuses[*].ready`
  booleans, `restarts` by summing the space-separated `.restartCount`s, and
  the effective pod status by taking the first
  `.status.containerStatuses[*].state.waiting.reason` when present. This is
  ~40 lines of pure string/`std::tm` helpers in an anonymous namespace in
  `kubectl_source.cpp` — deliberately kept on the client side of the split,
  never in the form.

- **`timegm` / `_mkgmtime` is the one platform guard.** Converting the
  parsed RFC3339 `std::tm` to a UTC epoch needs `timegm` on POSIX and
  `_mkgmtime` on Windows; there is no portable standard function. This is a
  genuine platform difference, guarded by a two-line
  `#if defined(_WIN32)` inside the shared file per the repo's platform
  policy — not a file split.

- **Lists are always `-A`; namespace is a filter, not a query parameter.**
  Deriving "which namespace am I looking at" from a toolbar `InputText`
  filtered client-side (§2 goal 6) keeps the module stateless with respect
  to the cluster and keeps every `update_*` a pure full rebuild. A real
  namespace switcher (re-running `get` with `-n`) is deferred.

- **`rollout restart` is not gated; `delete` and `drain` are.** Matches
  `docker`'s "Restart self-recovers, don't gate it" call. `drain` is gated
  because it evicts running pods; its confirm message says so.

- **The node menu is state-aware on `.spec.unschedulable`.** Cordon shows
  only when the node is schedulable, Uncordon only when it is cordoned —
  the same shape as `docker`'s container menu keying on `.State`.

- **Startup gate is `kubectl version -o json`.** The direct analog of
  `docker version --format '{{.Server.Version}}'`: modern `kubectl` contacts
  the API server for `serverVersion` and exits non-zero when the cluster is
  unreachable, so a clean exit confirms the *cluster* answered, not just
  that the binary exists.

## 7. Constraints and Invariants

Same as [../docker/DESIGN.md §7](../docker/DESIGN.md), with `docker` → `kubectl`:
the server form never touches `kubectl`, the filesystem, a socket, or a
subprocess; every `update_*` fully clears its table before repopulating;
every invocation is a real argv array through `uv_spawn`; the `binary`
argument is test-only; stale logs/describe responses are dropped silently;
the Follow thread stops on both checkbox-off and window close.

## 8. Integration Boundaries

Depends on: `wish::form` + `ui_root::on_event` + `import_json()`;
`message_box` + `form::instantiate_child_form()`; existing wish elements
(`Label` colour pairs, `Table` / `TableColumn` / `TableRow`, `MenuButton` /
`MenuItem`, `Combo`, `InputText`, `InputInt`, `Checkbox`, `Spring`,
`HorizontalLayout` / `VerticalLayout`) — no new widget; `uv_a`; no JSON
library; the system `kubectl` binary and a working current-context.

Depended on by: nothing else in wish; a leaf module.

## 9. Testing

- **`tests/test_kubectl.cpp`** — instantiate `KubectlFrontend` over
  `memory_transport`, feed hand-built `update_*` snapshots, and assert:
  table row counts and status-label counts; that all seven windows register;
  that a row's Delete menu item opens a `MessageBox` (root key prefix
  `"__message_box_"`) whose message names the resource + namespace, and that
  Yes emits `pod_action_requested {action:"delete", namespace:…}` while No
  emits nothing; that deployment Restart fires immediately (no dialog); that
  the node menu offers Cordon xor Uncordon by state and Drain emits with
  `name` only; that the phase Combo and namespace filter toggle row
  `visible`; that `command_result` routes to the scoped window; that the
  Logs / Describe menu items set the target label + emit, that
  `update_logs` splits on `\n` and drops a mismatched target; that
  `append_command_log` appends Console rows and "Clear Console" empties
  them. No cluster required.
- **`tests/test_kubectl_process.cpp`** — `run_kubectl_cli({"hello"},
  "printf")` captures stdout; `{...}, "false"` reports a non-zero exit; a
  missing binary reports `exit_code == -1`; a brace/quote-heavy jsonpath arg
  survives verbatim. Compiles `client/kubectl_process.cpp` directly and
  links `uv_a`, exactly like `test_docker_process`. 5 tests, no cluster
  required.
- **End-to-end (not yet done)**: the automation module against `wish client
  --run=kubectl` with a `kind` / `minikube` cluster and a fixture
  Deployment — see [PLAN.md](PLAN.md)'s Verification section.

## 10. Implementation Status

**Implemented and unit-tested; live cluster verification pending.**
`server/kubectl.{hpp,cpp}`, `client/kubectl*.{hpp,cpp}`,
`tests/test_kubectl*.cpp` are in place; `test_kubectl` and
`test_kubectl_process` pass.

- Pods / Deployments / Services / Nodes: four dockable list windows on the
  shared `list_window` / `build_list_window()` / `add_list_row()` path, each
  with a `...` action menu (state-aware for nodes); Pods name / namespace /
  phase filters; Deployments and Services name / namespace filters.
- Logs / Describe: two dockable text windows (`build_text_window()`).
  `logs_requested` runs `kubectl logs --tail N --timestamps` with a 2 s
  re-poll thread while "Follow" is checked; `describe_requested` runs
  `kubectl describe <kind>` (verbatim). Both carry a `name`/`namespace`
  staleness guard.
- Console: a dockable FIFO-capped `Table` trace of every one-shot `kubectl`
  invocation (`append_command_log`, fed by `kubectl_source::run_logged()` /
  `push_command_log()`), green/red by exit status, "Copy Entry" / "Clear
  Console" per-row context menu. The Follow re-poll thread is not traced.
- `kubectl version` startup gate; `MessageBox` confirm for delete and drain;
  no background polling.

**Not planned for v1** (deferred future work): `kubectl apply` / `edit` /
scale-to-N; `exec` / `port-forward` (needs PTY / socket streaming over RMI);
`kubectl top` metrics columns and sparklines; `kubectl get events`; rollout
history / undo; ReplicaSet / StatefulSet / DaemonSet / Job / ConfigMap /
Secret / Ingress / PVC / CRD windows; context and namespace switching UI;
true `kubectl logs -f` streaming and multi-container log selection.
