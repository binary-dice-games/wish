# wish docker Module — Architecture & Design

**Status: implemented and live-verified.** All six windows (Containers /
Images / Volumes / Networks / Logs / Inspect) work against a real Docker
daemon. See [PLAN.md](PLAN.md) and "10. Implementation Status" below.

## 1. Purpose / Scope

The `docker` module (`wish client --run=docker`) is a Docker Desktop-style
GUI frontend for the local `docker` command line: dockable windows listing
containers, images, volumes, and networks, each with per-row lifecycle
actions, plus a container log viewer and a formatted `docker inspect`
panel. It is a **pure frontend** — every operation runs the real `docker`
binary already installed on the machine; this module never speaks the
Docker Engine API directly, never links libdocker, and never parses image
manifests or the container filesystem itself.

Scope for this pass, agreed with the user up front:

- **Resource types**: containers, images, volumes, networks (the four
  cheap `docker … ls` surfaces). Docker Desktop's Builds / Dev
  Environments / Scout / Extensions are out.
- **Refresh model**: an explicit Refresh button per window plus an
  automatic refresh after every mutating action — **no background
  polling** (see §6).
- **Detail views**: a Logs window (`docker logs`, with a re-poll "Follow"
  toggle) and an Inspect window (formatted `docker inspect`). **No
  exec/terminal into a container** (that needs PTY streaming over RMI —
  see §10).
- **Confirmation**: destructive actions (stop / kill / remove / prune) are
  gated behind the built-in `MessageBox` form (see §6).

Explicitly deferred — see §10 and [PLAN.md](PLAN.md): `docker compose`,
`docker build`/Buildx, `exec`/interactive terminal, live `docker stats`
CPU/memory streaming, true `docker logs -f` streaming, registry
login/push, image history/layers, a `docker`-subprocess trace window
(`git` has one), Docker context switching.

This directory owns:

- `server/docker.hpp`/`.cpp` — the `DockerFrontend` form (six windows, the
  `update_*` render methods, the `*_requested` events).
- `client/docker.hpp`/`.cpp` — the client runner (`run_docker`, event
  wiring, app registration).
- `client/docker_process.hpp`/`.cpp` — a non-interactive libuv-based
  "run `docker <args>`, capture output" helper.
- `client/docker_source.hpp`/`.cpp` — every actual `docker` invocation,
  the tab-delimited `--format` output parsing, and the snapshot push /
  event reactions.
- `README.md` — user-facing usage.
- `docker_mock.json` — the UI mockup validated in the `editor` tool before
  implementation (kept for reference, mirrored by `server/docker.cpp`'s
  window layout strings).

## 2. Design Goals

1. **All `docker` invocation and output parsing is client-side.** The
   Docker daemon a user wants to manage is the one reachable from *their*
   machine (the default socket, or whatever `DOCKER_HOST` / `docker
   context` points at) — reachable only from the client. This mirrors
   `top`'s reasoning for client-side sampling and `git`'s for client-side
   `git` invocation. The server never touches `docker`, the filesystem, a
   socket, or a subprocess.

2. **The server owns all UI/render state** (selection, which container's
   logs are open, table row widgets) and renders whatever snapshot it was
   last given via its `update_*` RMI methods — matches
   `src/ui/forms/DESIGN.md`'s Design Goal 1 ("server-side logic,
   client-side data"), the same split `git` and `top` use.

3. **No shell in any `docker` invocation.** Every `docker` call goes
   through a real argv array via `uv_spawn` (`docker_process::run_docker_cli`,
   §3), never a shell command string — container names, image refs, and
   volume names need no escaping and carry no injection surface.

4. **A rebuild is a full rebuild, never an incremental patch.** Every
   `update_*` RMI call clears and fully repopulates the table it owns
   rather than diffing against the previous state — simpler to reason
   about and correct by construction, at the cost of some redundant
   `ui_element` churn on each refresh. Acceptable because refresh is only
   ever user-initiated (a Refresh click or the click behind a mutating
   action — §6), not a hot loop. `top`'s per-row reconciliation by key is
   the alternative; it is only worth its complexity for `top`'s
   once-a-second cadence, which this module deliberately does not have.

5. **Destructive actions are confirmed; reversible ones are not.** Stop,
   kill, remove, and every prune go through a modal `MessageBox`; start,
   restart, pause, unpause, pull, run, create, logs, and inspect fire
   directly — matching Docker Desktop's own convention and the user's
   "destructive like stopping an instance" framing.

## 3. Key Abstractions

### `DockerFrontend` (server, `form`)

Bison class `"DockerFrontend"` in the `"wish"` namespace. Owns **six
independently dockable `Window`s** (`form::init()` auto-registers only one
top-level root; the others are registered by hand in `on_init()` exactly
as `git.cpp`'s `build_*_window()` and `editor.cpp`'s Help/Log windows do —
`sess().ui_objects.merge(tree, root_key)` +
`sess().top_level_objects[root_key]` + `sess().top_level_handlers[root_key]
= this` + `(*root)["__path__"] = root_key`). The four list windows
(Containers / Images / Volumes / Networks) share one `build_list_window()`
/ `list_window` / `list_row` / `add_list_row()` path; the `...` menu
dispatch keys on a `{scope, key, action}` `row_action`, so one
`on_event()` clause routes every window's actions. Root keys are
`internal_root_key_` and `internal_root_key_ + "_images" / "_volumes" /
"_networks" / "_logs" / "_inspect"`.

- **Containers** (`internal_root_key_`, the main window): toolbar (Refresh,
  Prune stopped, a filter `InputText`, a state `Combo`), a status `Label`,
  and the containers `Table` (Name, Image, Status, Ports, Created). The
  Status cell is colour-coded by `.State` (green running, gray
  exited/created, amber paused/restarting, red dead). Each row's last cell
  is a `MenuButton` (`"..."`) whose `MenuItem`s are state-aware: Start on a
  stopped container; Stop / Restart / Pause / Kill on a running one;
  Unpause on a paused one — plus Logs / Inspect / Remove for all.
- **Images**: toolbar (Refresh, a pull-ref `InputText` + Pull button,
  Prune dangling), the images `Table` (Repository, Tag, Image ID, Created,
  Size). Row menu: Run / Inspect / Remove.
- **Volumes**: toolbar (Refresh, a new-name `InputText` + Create button,
  Prune), the volumes `Table` (Name, Driver, Mountpoint). Row menu:
  Inspect / Remove.
- **Networks**: toolbar (Refresh, Prune), the networks `Table` (Name,
  Driver, Scope, Network ID). Row menu: Inspect / Remove — the three
  built-in networks (`bridge`/`host`/`none`) get Inspect only.
- **Logs** (`internal_root_key_ + "_logs"`): toolbar (a container `Label`, a Follow
  `Checkbox`, a Lines `InputInt`, Refresh), a read-only text viewer
  showing `docker logs` output.
- **Inspect** (`internal_root_key_ + "_inspect"`): toolbar (a target
  `Label`, Refresh), a read-only text viewer showing `docker inspect`
  output.

  The text-viewer widget for both is decided in implementation (see §6):
  either a multiline read-only `InputText` (no sandbox file, simplest) or a
  `TextEditor` fed a client-uploaded sandbox file (`language: "none"` /
  `"json"`, gives selection + syntax highlighting at the cost of an
  upload round trip per refresh).

Closing the Containers window emits `"closed"` and tears down all six
subtrees (`remove_objects_at` for the five extra roots +
`remove_internal_objects` — `git.cpp`'s `on_event` close branch).

### `docker_source` (client)

Owns the proxy and every actual `docker` invocation. `refresh_all()` runs
the four `docker … ls` snapshots (`push_containers` / `push_images` /
`push_volumes` / `push_networks`) and calls the matching `update_*` RMI
method. One method per `*_requested` event, each running a mutating
`docker` command via `run_and_refresh()` (which reports the outcome via
`command_result` and then calls `refresh_all()`) — the exact shape of
`git_repo_source`. `on_logs_requested` / `on_inspect_requested` run their
read-only command and call `update_logs` / `update_inspect` directly.

### `docker_process::run_docker_cli()` (client)

`run_docker_cli(const std::vector<std::string>& args, std::string binary =
"docker")` → `{int exit_code; std::string stdout_text, stderr_text; bool
ok();}`. A near-verbatim copy of `git_process::run_git()`: `uv_loop_init` →
prepend `binary` as argv[0] → stdout/stderr pipes → `uv_spawn` → `uv_run`
(blocking) → collect exit code. No shell, no PTY (bison's
`bdg::bison::term::terminal` is unsuitable for the same reasons documented
in `git_process.hpp`). The `binary` parameter defaults to `"docker"` and
exists only so `test_docker_process` can drive the helper with `printf` /
`false` on a machine without Docker installed.

`uv_a` (libuv) is **already** linked into every module-client target by
`wish_finalize_app_modules()` (`cmake/WishModules.cmake`) — unlike `git`,
which had to add that link, this module needs no CMake change.

### `docker_source::run_logged()` (client)

Every `docker` invocation goes through this thin wrapper over
`run_docker_cli()` (`git_repo_source::run_logged`'s shape). For this pass
it just runs the command; a `docker`-subprocess trace window (like `git`'s
Log window) is deferred (§10), so `run_logged` currently only exists as
the single choke point a future trace hook would attach to.

## 4. Data Flow / Architecture

```
Startup:
  run_docker(host)
    `docker version --format '{{.Server.Version}}'`
      fail -> print "Docker daemon not reachable ..." + signal_done()  (git's is-a-repo gate)
    instantiate DockerFrontend -> proxy
    wire proxy.onEvent(...) for every *_requested / closed event
    source->refresh_all()   -- explicit initial call once every handler is wired
                               (git's initial-load-race fix; never an on_init()-emitted event)
    proxy.onEvent("refresh_requested", source->refresh_all())  -- every window's Refresh button

refresh_all() (client):  -- each command uses a TAB-delimited Go --format template,
                            split on \t line by line (git's for-each-ref parsing shape)
  push_containers() -> `docker ps -a --no-trunc --format '{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.State}}\t{{.Status}}\t{{.Ports}}\t{{.RunningFor}}'`
                       -> DockerFrontend.update_containers({containers:[...]})
  push_images()     -> `docker images --no-trunc --format '{{.ID}}\t{{.Repository}}\t{{.Tag}}\t{{.CreatedSince}}\t{{.Size}}'`
                       -> update_images({images:[...]})
  push_volumes()    -> `docker volume ls --format '{{.Name}}\t{{.Driver}}\t{{.Mountpoint}}'`
                       -> update_volumes({volumes:[...]})
  push_networks()   -> `docker network ls --no-trunc --format '{{.ID}}\t{{.Name}}\t{{.Driver}}\t{{.Scope}}'`
                       -> update_networks({networks:[...]})

DockerFrontend.update_containers (server):
  clear existing TableRow children (walk + erase by class)
  one add_container_row() per entry, each building TableRow{Label state-dot,
    Label x5, MenuButton + MenuItem x N}, wiring each MenuItem's __wish_id
    into menu_action_targets_[{scope:container, key:id, action}]
  local 0-based row-key counter (dynamic::size() gotcha, §6)

Row action (MenuItem "clicked" -> DockerFrontend.on_event):
  menu_action_targets_[id] -> {scope, key, action}
    destructive (stop/kill/remove/prune) : show_confirm(msg, [emit <scope>_action_requested{...}])
    reversible  (start/restart/pause/...) : emit <scope>_action_requested{key, action} directly
    logs    : emit logs_requested{id, follow:false, lines:<Lines field>}
    inspect : emit inspect_requested{kind:<scope>, id:key}

<scope>_action_requested (client):
  run_and_refresh(label, {docker, <verb>, <key>})
    -> command_result({command, ok, output})  (status label, red on failure)
    -> refresh_all()

logs_requested (client):
  on_logs_requested(id, follow, lines)
    `docker logs --tail <lines> --timestamps <id>`
    -> update_logs({container_id:id, title:"<name> logs", text:<output>})
    if follow: start a background thread re-running the above every ~2s until
      the Follow checkbox is unchecked (top's sampling-thread pattern; no
      streaming subprocess -- run_docker_cli stays blocking-only)

inspect_requested (client):
  `docker <kind> inspect <id>`  (pretty JSON)
    -> update_inspect({kind, target_id:id, title:"<name>", text:<output>})

DockerFrontend.update_logs / update_inspect (server):
  discard the call (no-op) if container_id / target_id no longer matches the
  window's currently-open target -- a stale response arriving late after the
  user selected something else (git's do_update_diff staleness guard)
```

## 5. Public API Contract

| Symbol | Contract |
|---|---|
| `DockerFrontend.update_containers(args)` | RMI method. `args.containers` — dynamic array, each `{ id, name, image, state ("running"/"exited"/"paused"/"created"/"restarting"/"dead"), status (human string), ports, created }`. Fully rebuilds the containers table. |
| `DockerFrontend.update_images(args)` | `args.images` — each `{ id, repository, tag, created, size }`. |
| `DockerFrontend.update_volumes(args)` | `args.volumes` — each `{ name, driver, mountpoint }`. |
| `DockerFrontend.update_networks(args)` | `args.networks` — each `{ id, name, driver, scope }`. |
| `DockerFrontend.update_logs(args)` | `{ container_id, title, text }`. `container_id` echoed from `logs_requested`; discarded if it no longer matches the Logs window's open target. |
| `DockerFrontend.update_inspect(args)` | `{ kind, target_id, title, text }`. Same staleness guard on `target_id`. |
| `DockerFrontend.command_result(args)` | `{ command (string), ok (bool), output (string, shown on failure) }`. Writes the relevant window's status `Label` green/red. |
| `"refresh_requested"` event | No payload. Client re-runs all four `docker … ls` snapshots. Fired by any window's Refresh button. |
| `"container_action_requested"` event | `{ id, action }` — `action` ∈ `start`, `stop`, `restart`, `pause`, `unpause`, `kill`, `remove`. |
| `"image_action_requested"` event | `{ id, action }` — `action` ∈ `run`, `remove`. |
| `"volume_action_requested"` event | `{ name, action }` — `action` ∈ `remove`. |
| `"network_action_requested"` event | `{ id, action }` — `action` ∈ `remove`. |
| `"prune_requested"` event | `{ scope }` — `scope` ∈ `containers`, `images`, `volumes`, `networks`. |
| `"pull_image_requested"` event | `{ ref }` — from the Images window's inline field. |
| `"create_volume_requested"` event | `{ name }` — from the Volumes window's inline field. |
| `"logs_requested"` event | `{ id, follow (bool), lines (int32) }`. |
| `"inspect_requested"` event | `{ kind, id }` — `kind` ∈ `container`, `image`, `volume`, `network`. |
| `"closed"` event | The Containers window's X was clicked; all six subtrees torn down. The client should `signal_done()`. |
| `wish client --run=docker` | No positional args — the module talks to whatever Docker daemon the `docker` CLI itself would (`DOCKER_HOST` / default socket / current context). |

The internal `ui_element` tree of every window is private — clients must
not address its nodes by dot-path (they instantiate `DockerFrontend` and
use only the methods/events above).

## 6. Design Decisions

- **`docker_process::run_docker_cli()` is built on libuv (`uv_spawn`), not
  `bdg::bison::term::terminal`** — verbatim the rationale in
  `modules/bdg/desktop/git/client/git_process.hpp`: `terminal` is
  PTY-attached, takes a shell *string*, and redirects the calling
  process's own stdio for its lifetime — none of which is safe for many
  quick argv-array `docker <args>` calls from inside a long-running
  `wish_client`. libuv is already vendored by bison and, as of the `git`
  module, already linked into every module-client target, so this file
  needs no CMake change.

- **The `binary` parameter on `run_docker_cli()` (default `"docker"`)
  exists purely for testability.** `test_docker_process` runs on CI and
  dev machines that have no Docker installed; passing `"printf"` /
  `"false"` / `"sh"` lets it exercise the argv-building, pipe-reading, and
  exit-code paths against a guaranteed-present binary. Production code
  never passes the second argument. This is the one deviation from a
  straight copy of `git_process` (which hard-codes `"git"`), justified
  because a throwaway `git init` repo is trivial to create in a test but a
  running Docker daemon is not.

- **No background polling — every refresh is user-initiated.** `git`
  removed a ~2 s background `refresh_all()` poll after it caused visible
  window "vibration" (full-tree rebuild every tick shifting focus and
  layout mid-interaction) and flooded its trace window. This module starts
  from that lesson: `refresh_all()` runs only from `run_docker()`'s
  explicit initial call, a Refresh button click (`"refresh_requested"`),
  or the tail of a mutating action (`run_and_refresh`). Picking up an
  out-of-band change (a container started from another terminal) requires
  clicking Refresh. The one bounded exception is the Logs window's
  "Follow" toggle (below), which re-polls a *single* command into a
  *single* `TextEditor` — no tree rebuild, no focus theft.

- **Every `update_*` is a full clear-and-rebuild, not `top`-style per-row
  reconciliation by container id.** `top` reconciles because it refreshes
  once a second and a full rebuild at that cadence would thrash
  `ctx().objects`. This module refreshes only on an explicit user action,
  so the simpler "erase every `TableRow` child, re-add from scratch"
  (`git`'s Design Goal 4) is correct by construction and cheap enough.
  **Invariant**: each `update_*` handler must fully clear its table's
  `children` map before repopulating — never append — both to avoid
  visual duplication and to keep the numeric child keys a contiguous
  0-based sequence (next point).

- **Each table's row-key counter is local to its rebuild, never a shared
  member.** `bison::dynamic::size()` reports "highest numeric key + 1",
  not a true count (`bison_object.hpp`); a counter shared across the four
  tables would make every table rebuilt after the first over-report its
  row count. Since each `update_*` rebuilds one whole table in a single
  call, a `size_t` local at the top of each rebuild loop is trivially
  correct — the same fix `git.cpp`'s `rebuild_section()` /
  `rebuild_graph_table()` use.

- **Destructive actions go through the built-in `MessageBox` form, not a
  hand-rolled dialog.** `show_confirm(message, on_confirm)` is a direct
  port of `git_repo::show_confirm()`:
  `form::instantiate_child_form<message_box>("MessageBox", {title,
  message, icon:"warning", buttons:"yes_no"}, on_result)`, with the
  `on_result` callback running `on_confirm()` only when `payload.button ==
  "yes"`. One `std::shared_ptr<message_box> confirm_dialog_` member holds
  it; a second destructive click just overwrites that member (the stale
  instance's destructor tears itself down). `MessageBox` is a genuine
  `Window.modal = true` blocking overlay that owns and closes its own
  tree; `DockerFrontend::on_event()` needs no confirm-specific branch. The
  gated set is: container `stop` / `kill` / `remove`, image / volume /
  network `remove`, and every `prune`. `restart` is **not** gated (Docker
  Desktop doesn't gate it either, and it self-recovers); `stop` **is**,
  because the user explicitly called it out.

- **`MessageBox` has no custom-body slot, so "pull image" and "create
  volume" use inline toolbar fields, not a dialog.** Same constraint and
  same resolution as `git`'s new-branch field: a small always-visible
  `InputText` + adjacent Button in the Images / Volumes window toolbar,
  wired to `pull_image_requested` / `create_volume_requested`. The field
  deliberately does **not** set `EnterReturnsTrue` — its `"changed"` must
  fire per keystroke so the adjacent Button's click handler reads the
  current text (`git.cpp`'s documented new-branch trap).

- **The Logs "Follow" toggle is a client-side re-poll, not `docker logs
  -f` streaming.** True `-f` needs a non-blocking / streaming subprocess
  helper (a `read_cb` that forwards partial lines and a `uv_async` stop
  signal) — real work that would complicate `run_docker_cli`'s otherwise
  exact reuse of `git_process`. Instead, checking Follow starts a
  background thread (the `top` client's sampling-thread pattern) that
  re-runs `docker logs --tail <lines> --timestamps <id>` every ~2 s and
  calls `update_logs` until the box is unchecked or the window closes.
  Bounded, single-command, single-widget — none of the jitter that killed
  `git`'s background poll. True streaming is listed in §10.

- **Logs and Inspect responses carry a staleness guard.** `logs_requested`
  / `inspect_requested` capture the target id *before* an async client
  round trip; by the time `docker logs` / `docker inspect` returns and the
  RMI call lands, the user may have opened a different container's logs.
  `update_logs` / `update_inspect` echo `container_id` / `target_id` back
  and the server discards the call if it no longer matches the window's
  current target — the exact pattern (and rationale) as `git`'s
  `do_update_diff` / `do_update_commit_files`. **Invariant**: any RMI
  method that repopulates UI state from a pre-captured async request must
  echo enough of that request back for the server to verify it still
  matches current selection, and silently drop it if not.

- **A tab-delimited Go `--format` template for the list views, split on
  `\t` — not `{{json .}}`, and no JSON library.** `docker
  ps`/`images`/`volume ls`/`network ls` accept an arbitrary Go template;
  `'{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.State}}\t{{.Status}}\t…'` gives one
  tab-separated line per row with every column already humanized by Docker
  itself (`.Status` = "Up 3 hours", `.Size` = "431MB", `.RunningFor` = "3
  hours ago", `.State` = the machine-readable "running"/"exited"/… used for
  the status colour). `docker_source` splits each line on `\t` exactly as
  `git_repo_source` splits `for-each-ref`'s output — no client-side
  humanization, no `inspect` per row, and (unlike `{{json .}}`) no JSON
  parsing and no dependency on `nlohmann::json`, whose header path is
  `PRIVATE` to a handful of wish targets and is *not* propagated to
  module-client sources. `docker inspect`'s output is JSON but the Inspect
  window shows it verbatim as text — never parsed. Fields whose value can
  itself contain a tab or newline (none of the ones used here do — Docker
  renders `.Ports` and `.Status` as single-line strings) would need a
  different separator; `\t` is safe for this column set.

- **Colours are `text_color_light` / `text_color_dark` pairs on `Label`,
  via a local `theme_hex` helper** — copied from `git.cpp`. Container
  state gets a coloured dot: green (`#1A7F37` / `#3FB950`) running, gray
  (`#656D76` / `#8B949E`) exited/created, amber (`#9A6700` / `#D29922`)
  paused/restarting, red (`#CF222E` / `#F85149`) dead. Status-label
  success/failure reuses the green/red pair. A single `text_color` tuned
  for one theme reads poorly against the other (the reported `git` bug).

- **Six separate dockable windows, no unified nav sidebar** (the user's
  choice). This matches `git` (4 windows) and `editor` (4 windows) and is
  the more "wish-native" shape: every `Window` without `NoDocking` is
  already a dock candidate in the session's implicit host dockspace, so
  the user arranges them (tabbed, split, floating) however they like. A
  Docker-Desktop-style left rail would mean one window swapping content
  panels — rejected as less flexible and not what the user asked for.

- **The Logs / Inspect body is a text viewer, not a live-updating tree.**
  Two workable widgets: (a) a multiline read-only `InputText` — renders
  immediately, no session-sandbox file, no round trip, but no syntax
  highlighting; (b) a `TextEditor` — the client uploads the `docker logs`
  / `docker inspect` text to the session sandbox and points the editor's
  `file_path` at it (the `nano` / `editor` upload pattern), giving
  selection, scroll-position retention, and JSON highlighting for Inspect,
  at the cost of one `upload_file` per refresh. Starting with (a) for v1
  simplicity; (b) is a clean upgrade if the plain field proves
  insufficient. Either way the server form only renders — the text is
  always produced client-side.

- **`run_docker()` gates on `docker version` before instantiating the
  form.** Mirrors `git`'s `rev-parse --is-inside-work-tree` fast-fail: if
  the daemon isn't reachable (`docker` not on `PATH`, socket permission,
  daemon down), print one clear line to stderr and `signal_done()` rather
  than opening six empty windows. `--format '{{.Server.Version}}'` also
  confirms the *daemon* answered, not just that the client binary exists.

## 7. Constraints and Invariants

- The server form never touches `docker`, the filesystem, a socket, or a
  subprocess; `docker_process` / `docker_source` (client-only) own that
  entirely.
- Every `update_*` RMI handler fully clears its owned table before
  repopulating — never appends (§6).
- Every `docker` invocation is a real argv array through `uv_spawn` — no
  shell string is ever constructed (§2 Goal 3).
- `run_docker_cli()`'s `binary` argument is never passed anything but the
  default in production code — only tests supply it.
- Logs/Inspect `update_*` calls that fail their staleness guard are
  dropped silently, never applied "because it's the only data we have".
- The Logs "Follow" background thread must stop on both checkbox-off and
  window close, and must tolerate the form being torn down mid-call
  (`try { proxy->call(...) } catch`).

## 8. Integration Boundaries

Depends on:

- `wish::form`, `ui_root::on_event`'s catch-all dispatch, `import_json()`
  — server-side UI construction, same pattern every other module uses.
- `message_box` (`src/ui/forms/message_box.hpp`) +
  `form::instantiate_child_form()` — the destructive-action confirm
  dialog.
- `Label.text_color_light` / `text_color_dark`, `TextEditor` (`read_only`,
  `language`), `Table` / `TableColumn` / `TableRow`, `MenuButton` /
  `MenuItem`, `Combo`, `InputText`, `InputInt`, `Checkbox`,
  `HorizontalLayout` / `VerticalLayout` — all existing wish elements, no
  new widget needed.
- `uv_a` (libuv, vendored by bison) — `docker_process`'s subprocess
  helper; already linked into module-client targets by
  `wish_finalize_app_modules()`.
- No JSON library — the list views use a tab-delimited `--format`
  template split on `\t` (see §6); `docker inspect` output is shown
  verbatim.
- The system `docker` binary (must be on `PATH`) and a reachable Docker
  daemon.

Depended on by: nothing else in wish; this is a leaf module.

## 9. Testing

- **`tests/test_docker.cpp`** — instantiate `DockerFrontend` over
  `memory_transport`, feed hand-built `update_containers` / `update_images`
  / … snapshots, and assert: table row counts; the state-dot colour for
  each state; that a row's "Remove" menu item opens a `MessageBox` (root
  key prefix `"__message_box_"`) and clicking its Yes button emits
  `container_action_requested {action:"remove"}` while No emits nothing;
  that `update_logs` with a mismatched `container_id` is a no-op. Mirrors
  `tests/test_git.cpp`'s `DeleteBranchClickShowsConfirmDialog…` /
  `StaleDiffResponseIgnored…` tests. No Docker daemon required.
- **`tests/test_docker_process.cpp`** — `run_docker_cli({"hello"},
  "printf")` captures stdout; `run_docker_cli({...}, "false")` reports a
  non-zero exit; a missing binary reports `exit_code == -1`. Compiles
  `client/docker_process.cpp` directly and links `uv_a`, exactly like
  `tests/test_git_process.cpp`. No Docker daemon required.
- **End-to-end**: automation module against `wish client --run=docker`
  with a real fixture container — see [PLAN.md](PLAN.md)'s Verification
  section. Requires the invoking user to be in the `docker` group (or a
  rootless / `DOCKER_HOST` daemon).

## 10. Implementation Status

**Implemented and live-verified** (see [PLAN.md](PLAN.md)).
`server/docker.{hpp,cpp}`, `client/docker*.{hpp,cpp}`,
`tests/test_docker*.cpp` are in place.

- Containers / Images / Volumes / Networks: four dockable list windows on
  one shared `list_window` / `build_list_window()` / `add_list_row()`
  path, each with a state-aware `...` action menu; Containers text + state
  filters; inline pull-image / create-volume fields; built-in networks get
  an Inspect-only menu.
- Logs / Inspect: two dockable text windows (`build_text_window()` —
  toolbar + single-column scrolling `Table` of `Label` lines).
  `logs_requested` runs `docker logs --tail N --timestamps` with a
  `top`-style 2 s re-poll thread while "Follow" is checked;
  `inspect_requested` runs `docker <kind> inspect` (JSON shown verbatim).
  `update_logs`/`update_inspect` carry a `container_id`/`target_id`
  staleness guard.
- `docker version` startup gate; `MessageBox` confirm for stop/kill/remove
  and every prune; no background polling of the list windows.

33 tests pass across `test_docker` (28) and `test_docker_process` (5).
End-to-end verified against a real Docker daemon: container stop with
confirm, image remove with confirm, volume create, four-window grid,
`docker logs` / `docker inspect` panes.

**Not planned for v1** (deferred future work): `docker compose` project
view and up/down; `docker build` / Buildx; `exec` / interactive terminal
into a container (needs PTY streaming over RMI); live `docker stats`
CPU/memory columns and sparklines; true `docker logs -f` streaming (v1
Follow is a 2 s re-poll); registry login / image push; image history and
layer inspection; volume / network creation with options beyond a name; a
dedicated `docker`-subprocess trace window (`git` has one — `run_logged`
is the choke point it would attach to); Docker context switching UI;
Extensions / Scout / Dev Environments.
