# wish docker Module — Implementation Plan

See [DESIGN.md](DESIGN.md) for the architecture this plan implements — the
client/server split (all `docker` invocation client-side, server only
renders), the tab-delimited `--format` parsing, the `MessageBox` confirm
for destructive actions, and the six-window layout.

## Steps

1. **DESIGN.md + UI mockup** — `DESIGN.md`, `docker_mock.json` (renders in
   the `editor` tool), `docker_mock.html` (full-width review of all six
   windows). ✅ Done — validated with the user before implementation.

2. **Scaffold + `docker_process` + Containers table live** —
   `server/docker.{hpp,cpp}` (`DockerFrontend` form, `register_docker()`),
   `client/docker.{hpp,cpp}` (`run_docker` + registrar, `docker version`
   startup gate), `client/docker_process.{hpp,cpp}` (libuv `uv_spawn`
   helper, `binary` param for tests), `client/docker_source.{hpp,cpp}`
   (`push_containers()` via `docker ps -a --format '{{.ID}}\t…'`). ✅ Done.

3. **Containers window complete** — all columns, state-coloured Status
   (`theme_hex` green/gray/amber/red), the `...` per-row `MenuButton`
   (state-aware item set), name/image text filter + All/Running/Stopped
   `Combo` (server-side `visible` toggle), `container_action_requested` +
   `run_and_refresh`, `prune_requested`, `show_confirm` (MessageBox
   `yes_no`) for Stop/Kill/Remove and Prune. ✅ Done.
   - Tests: `tests/test_docker.cpp` (13 RMI + local tests) and
     `tests/test_docker_process.cpp` (5 stub-binary tests). All passing.
   - Live-verified end-to-end via the automation module against a real
     Docker daemon: table populates from `docker run` fixtures with
     correct state colours; the `...` menu → Stop → `MessageBox` → Yes runs
     `docker stop` and the row flips to `exited` after the auto-refresh.

4. **Images / Volumes / Networks windows** — three more dockable `Window`s
   (`internal_root_key_` + `_images` / `_volumes` / `_networks`, registered
   by hand via the shared `build_list_window()`), their `push_*` snapshots,
   `image_action_requested` (run / remove), `volume_action_requested` /
   `network_action_requested` (remove), the three more `prune` scopes, and
   the inline pull-image / create-volume `InputText` + Button pairs. Built
   on one generic `list_window` / `list_row` / `add_list_row()` path shared
   by all four windows; the `...` menu dispatch keys on a `{scope, key,
   action}` row_action. Built-in networks (`bridge`/`host`/`none`) get an
   Inspect-only menu. ✅ Done.
   - Tests: `tests/test_docker.cpp` grew to 22 RMI tests (image remove
     confirm→emit, volume remove emits `{name}`, built-in network has no
     Remove, prune scope routing, inline Pull field, `command_result`
     `scope` routing, any-window close). All passing (28 total with
     `test_docker_process`).
   - Live-verified: all four windows dock into a grid; Volumes inline
     "Create" ran `docker volume create` and the new volume appeared;
     Images row `...` → Remove... → "Remove image 'busybox:latest'?" → Yes
     ran `docker rmi` and the image vanished after the refresh.

5. **Logs + Inspect windows** — `internal_root_key_ + "_logs" / "_inspect"`,
   built by a shared `build_text_window()` (toolbar + single-column
   scrolling `Table` of `Label` lines — git's diff-viewer shape, no
   sandbox file, no `InputText` length cap). `logs_requested` (`docker
   logs --tail N --timestamps`, plus a `top`-style 2 s re-poll thread in
   `docker_source` while "Follow" is checked — the thread is torn down on
   the next `on_logs_requested()` or `~docker_source()`) and
   `inspect_requested` (`docker <kind> inspect`, JSON shown verbatim).
   `update_logs` / `update_inspect` discard a response whose
   `container_id` / `target_id` no longer matches the window's open target
   (git's `do_update_diff` staleness guard). ✅ Done.
   - Tests: 5 more in `test_docker.cpp` (logs menu sets target + emits,
     `update_logs` line-splitting + stale-target guard, Follow checkbox
     re-emits `follow:true`, inspect menu emits + `update_inspect` fills).
     33 total across both suites, all passing.
   - Live-verified: container `...` → Logs shows `docker logs` output;
     Follow re-poll updates it; `...` → Inspect shows the `docker inspect`
     JSON.

6. **Docs** — this file, `DESIGN.md`, module `README.md`,
   `docs/building.md` CMake-options table, `CHANGELOG.md` `### Added`,
   `modules/bdg/dev/README.md` (collection README). ✅ Done.

## Verification

- **Unit tests** (no Docker daemon needed):
  `cmake --build build --target test_docker test_docker_process` with
  `-DWISH_MODULE_BDG_DEV_DOCKER=ON`, then run both binaries. `test_docker`
  drives `DockerFrontend` over `memory_transport` with synthetic `update_*`
  snapshots; `test_docker_process` drives `run_docker_cli()` with stub
  binaries (`printf` / `false`). 18/18 passing as of Step 3.

- **End-to-end** (performed, not just described — needs the invoking user
  in the `docker` group): built with
  `-DWISH_ENABLE_WEB=ON -DWISH_ENABLE_AUTOMATION=ON -DWISH_MODULE_BDG_DEV_DOCKER=ON`;
  created throwaway fixtures (`docker run -d --name … nginx:alpine`, a
  short-lived `alpine sleep`); launched
  `wish standalone --run=docker --renderer web`; drove it with
  `wish.automation.AutomationClient` (screenshot + `get_tree()` +
  path-targeted `click()`). Confirmed: the Containers table populated with
  correct columns and green/gray Status colours; the running container's
  `...` menu showed Stop/Restart/Pause/Kill (the stopped one showed Start);
  clicking Stop opened the `MessageBox` ("Stop container '…'?", Yes/No);
  clicking Yes ran `docker stop` (verified via `docker ps`) and the row
  flipped to `exited` with the status count updating after the automatic
  refresh.

## Not implemented (deferred future work)

- **`docker compose`** — project view, `up`/`down`, per-service logs.
- **`docker build` / Buildx** — no build UI or build-cache view.
- **`exec` / interactive terminal into a container** — needs PTY streaming
  over RMI.
- **Live `docker stats`** — no CPU/memory columns or sparklines.
- **True `docker logs -f` streaming** — the v1 "Follow" toggle is a 2 s
  re-poll of `docker logs --tail N`.
- **Registry login / image push**, image history & layer inspection.
- **Volume / network creation with options** beyond a bare name.
- **Docker context switching UI**; Extensions / Scout / Dev Environments.
- **In-app credential / permission prompts.**
