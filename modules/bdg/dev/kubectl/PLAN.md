# wish kubectl Module — Implementation Plan

See [DESIGN.md](DESIGN.md) for the architecture this plan implements, and
[../docker/PLAN.md](../docker/PLAN.md) for the sibling module this one is
modelled on — the client/server split (all `kubectl` invocation
client-side, server only renders), the tab-delimited `-o jsonpath` parsing,
the `MessageBox` confirm for destructive actions, and the six-window layout.

## Steps

1. **DESIGN.md + UI mockup** — `DESIGN.md`, `kubectl_mock.json` (renders in
   the `editor` tool), `kubectl_mock.html` (full-width review of all six
   windows). ✅ Done.

2. **Scaffold + `kubectl_process` + all four list windows** —
   `server/kubectl.{hpp,cpp}` (`KubectlFrontend` form, `register_kubectl()`,
   the shared `build_list_window()` / `build_text_window()` paths, all six
   windows, the state-coloured status cells, the state-aware node menu, the
   name/namespace/phase filters, `MessageBox` confirm on delete/drain),
   `client/kubectl.{hpp,cpp}` (`run_kubectl` + registrar, `kubectl version`
   startup gate, every `*_requested` wiring), `client/kubectl_process.{hpp,cpp}`
   (libuv `uv_spawn` helper, `binary` param for tests),
   `client/kubectl_source.{hpp,cpp}` (the four `push_*` snapshots via
   `kubectl get … -A -o jsonpath`, the client-side age/ready/restarts
   humanization, `run_and_refresh`, logs + describe with the 2 s Follow
   re-poll thread and the staleness guard). ✅ Done.
   - Tests: `tests/test_kubectl.cpp` (18 RMI + local tests) and
     `tests/test_kubectl_process.cpp` (5 stub-binary tests). All passing.
   - Build wiring: `tests/CMakeLists.txt` entries; the module itself is
     auto-discovered by `wish_add_collection(bdg/dev)` (has `server/` +
     `client/`), so no root `CMakeLists.txt` edit is needed.

3. **Docs** — this file, `DESIGN.md`, module `README.md`, `docs/building.md`
   CMake-options table, `CHANGELOG.md` `### Added`, `modules/bdg/dev/README.md`
   (collection README). ✅ Done.

4. **End-to-end verification against a live cluster** — ⬜ Not yet done.
   Build with
   `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON -DWISH_MODULE_BDG_DEV_KUBECTL=ON`;
   bring up a throwaway cluster (`kind create cluster` or `minikube start`),
   `kubectl create deployment web --image=nginx --replicas=2`; launch
   `wish standalone --run=kubectl --renderer web`; drive it with
   `wish.automation.AutomationClient` (screenshot + `get_tree()` +
   path-targeted `click()`). Confirm: the Pods table populates with the two
   `web-*` pods and green Running status; the `...` menu → Delete opens the
   `MessageBox` ("Delete pod '…' in namespace 'default'?"); Yes runs
   `kubectl delete pod` and the deployment recreates it after the
   auto-refresh; Deployments `...` → Restart runs `kubectl rollout restart`;
   Nodes `...` → Cordon flips the node to `SchedulingDisabled` and the menu
   now offers Uncordon; a pod's Logs and Describe panes fill. Record any
   new gotchas in `docs/automation.md` and flip DESIGN.md §10 to
   "live-verified".

5. **Top window — live `kubectl top` graphs** (added after steps 1-3
   shipped). ✅ Done.
   - `server/kubectl.{hpp,cpp}`: an 8th dockable window (`_top`) — a
     scrolling `VerticalLayout` with four `Plot`s (pod CPU millicores, pod
     memory MiB, node CPU %, node memory %), each one `PlotLine` per
     pod/node (top 15) plus an aggregate ("Total" for the additive pod
     plots, "Cluster avg" for the node % plots) over a rolling
     `kMaxStatsHistory = 120` buffer, and pod + node current-values
     `Table`s. New `update_stats` RMI method + `stats_plot` line tracker
     (shared shape with `docker`'s Stats window).
   - `client/kubectl_source.{hpp,cpp}`: `start_stats_polling()` — a detached
     thread running `kubectl top pods -A --no-headers` +
     `kubectl top nodes --no-headers` every ~10 s, with file-local
     millicore / MiB / percent parse helpers, into `update_stats`. Uses
     `run_kubectl_cli()` directly, **never `run_logged()`**, so the poll
     never reaches the Console. On `kubectl top` failure (no metrics-server)
     it sends `{pods:[], nodes:[], error}` and the form shows the reason.
     `client/kubectl.cpp` calls it once after wiring.
   - Tests: 4 more in `test_kubectl.cpp` (`*Top*`) — window builds, per
     pod/node lines + aggregate added, stale lines/rows dropped, `error`
     shown in the status label. 25 total across both suites, all passing.
   - End-to-end still pending a cluster with metrics-server.

## Verification

- **Unit tests** (no cluster needed):
  `cmake --build build --target test_kubectl test_kubectl_process` with
  `-DWISH_MODULE_BDG_DEV_KUBECTL=ON`, then run both binaries. `test_kubectl`
  drives `KubectlFrontend` over `memory_transport` with synthetic `update_*`
  snapshots; `test_kubectl_process` drives `run_kubectl_cli()` with stub
  binaries (`printf` / `false`). 25/25 passing (incl. the `*Top*` cases).

- **End-to-end**: see Step 4 — pending a cluster. The Top window's error
  path is exercisable against any reachable cluster without metrics-server
  (`kubectl top` exits non-zero → the status label shows the reason).

## Not implemented (deferred future work)

- **`kubectl apply` / `edit` / `scale --replicas=N`** — no YAML editor, no
  scale dialog (v1 has no numeric input for replica count).
- **`exec` / `port-forward` into a pod** — needs PTY / socket streaming over
  RMI.
- **`kubectl get events`**, rollout history / `undo`.
- **True `kubectl logs -f` streaming** and multi-container log selection —
  the v1 "Follow" toggle is a 2 s re-poll of `kubectl logs --tail N` on the
  default container.
- **More resource kinds** — ReplicaSets, StatefulSets, DaemonSets, Jobs,
  CronJobs, ConfigMaps, Secrets, Ingresses, PVCs, CRDs.
- **Context / namespace switching UI** — lists are always `-A` + client-side
  filter.
- **In-app kubeconfig / auth prompts.**
