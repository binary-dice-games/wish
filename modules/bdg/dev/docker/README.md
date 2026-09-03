# docker

A Docker Desktop-style GUI frontend for the local `docker` command line:
dockable windows listing containers, images, volumes, and networks, each
with per-row lifecycle actions, plus a container log viewer and a formatted
`docker inspect` panel. This is a pure frontend — every operation runs the
real `docker` binary already on the machine; it never speaks the Docker
Engine API directly.

`wish client --run=docker` (no positional args — it talks to whatever
Docker daemon the `docker` CLI itself would: `DOCKER_HOST` / the default
socket / the current `docker context`). Opens as independently dockable
windows (drag/resize/tab like any other wish window).

Refresh of the list windows is **manual**: a Refresh button per window plus
an automatic refresh after every mutating action — no background polling
(the `git` module's window-jitter lesson). The **Stats** window is the
exception: it graphs live `docker stats` CPU/memory usage from a background
poll (~3 s), which is deliberately kept out of the Console trace.
Destructive actions (**Stop**, **Kill**,
**Remove**, **Prune**) are gated behind a `MessageBox` confirm; everything
else (Start, Restart, Pause, Unpause, Pull, Run, …) fires directly.

- **server/**: `DockerFrontend` form (`register_docker()`) — renders
  whatever snapshot it was last given via `update_containers` /
  `update_images` / `update_volumes` / `update_networks` / `update_logs` /
  `update_inspect` / `append_command_log` / `update_stats`, and emits `*_requested` events (see `server/docker.hpp`'s
  class doc comment for the full contract) for the client to react to by
  running the corresponding `docker` command.
- **client/**: `run_docker(wish_app_host&)`, self-registered as the
  `"docker"` embedded app — owns all `docker` invocation.
  `client/docker_process.hpp`/`.cpp` is a small, non-interactive,
  libuv-based (`uv_spawn`) "run this argv array, capture
  stdout/stderr/exit code" helper (a near-copy of the `git` module's
  `git_process`). `client/docker_source.hpp`/`.cpp` runs every actual
  `docker` command (tab-delimited `--format` templates, split on `\t`),
  parses the output, and pushes snapshots / reacts to `*_requested`
  events. `run_docker()` gates on `docker version` and prints a clear
  error if the daemon isn't reachable.
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_DOCKER=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).

See [DESIGN.md](DESIGN.md) for the full architecture and [PLAN.md](PLAN.md)
for what's implemented vs. deferred.

## Implementation status

All eight windows are implemented and live-verified against a real Docker
daemon:

- **Containers / Images / Volumes / Networks** — each a toolbar + `Table`
  with a `...` per-row action menu (state-aware for containers: Start on a
  stopped one, Stop/Restart/Pause/Kill on a running one; Inspect-only for
  built-in networks), the Containers name/image text filter +
  All/Running/Stopped state filter, inline pull-image / create-volume
  fields.
- **Logs** — `docker logs --tail N --timestamps` in a scrolling pane; the
  "Follow" checkbox starts a 2 s client-side re-poll.
- **Inspect** — `docker <kind> inspect` output shown verbatim.
- **Console** — a scrolling, FIFO-capped table tracing every `docker`
  command the module ran (`#` / Command / Exit / Output), green on
  success, red on failure; right-click a row for "Copy Entry" or "Clear
  Console". (`git`'s "Log" window, renamed to avoid clashing with Logs.)
- **Stats** — live `docker stats`: a CPU % and a Memory % graph with one
  line per running container (busiest 15) plus a "Total" line, and a
  current-values table (Name / CPU % / Mem % / Mem Usage). Sampled every
  ~3 s on a background thread; these polls are *not* shown in the Console.

Plus a `docker version` startup gate and a `MessageBox` confirm for
Stop/Kill/Remove and every Prune.

## Known limitations (v1)

- **No `docker compose`, `docker build`, or `exec`/interactive terminal.**
- **The Logs "Follow" toggle is a client-side 2 s re-poll**, not true
  `docker logs -f` streaming.
- **`docker` must be on `PATH` and the daemon reachable** by the invoking
  user (in the `docker` group, or a rootless / `DOCKER_HOST` daemon). No
  in-app auth/permission prompt.

See [PLAN.md](PLAN.md) for the complete list of deferred features.
