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

## Verification

- **Unit tests** (no cluster needed):
  `cmake --build build --target test_kubectl test_kubectl_process` with
  `-DWISH_MODULE_BDG_DEV_KUBECTL=ON`, then run both binaries. `test_kubectl`
  drives `KubectlFrontend` over `memory_transport` with synthetic `update_*`
  snapshots; `test_kubectl_process` drives `run_kubectl_cli()` with stub
  binaries (`printf` / `false`). 23/23 passing.

- **End-to-end**: see Step 4 — pending a cluster.

## Not implemented (deferred future work)

- **`kubectl apply` / `edit` / `scale --replicas=N`** — no YAML editor, no
  scale dialog (v1 has no numeric input for replica count).
- **`exec` / `port-forward` into a pod** — needs PTY / socket streaming over
  RMI.
- **`kubectl top`** — no CPU/memory columns or sparklines.
- **`kubectl get events`**, rollout history / `undo`.
- **True `kubectl logs -f` streaming** and multi-container log selection —
  the v1 "Follow" toggle is a 2 s re-poll of `kubectl logs --tail N` on the
  default container.
- **More resource kinds** — ReplicaSets, StatefulSets, DaemonSets, Jobs,
  CronJobs, ConfigMaps, Secrets, Ingresses, PVCs, CRDs.
- **Context / namespace switching UI** — lists are always `-A` + client-side
  filter.
- **A `kubectl`-subprocess trace window** (`git` has one).
- **In-app kubeconfig / auth prompts.**
