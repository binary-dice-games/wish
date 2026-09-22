# wish curl Module — Architecture & Design

**Status: implemented and live-verified.** Unit tests (29/29 passing)
plus two rounds of automation-module runs against real endpoints
(`https://httpbin.org`, `https://www.google.com`) found and fixed four
real bugs — see "10. Implementation Status" and PLAN.md's Verification
checklist for the full list and what each automation pass covered.
Reload-from-History/Collections, the delete-confirm flow, and environment
substitution are implemented and unit-tested; driving them live was
initially blocked by what looked like a dock-tab automation limitation,
but that turned out to be a testing-technique mistake (missing click
delay / verifying with a fixed sleep instead of polling — see
docs/automation.md), not a real constraint — switching tabs and driving
those flows live is possible and merely not yet exercised in this pass.

This is a close sibling of the [docker](../docker/DESIGN.md) and
[kubectl](../kubectl/DESIGN.md) modules: a server-side `form` that owns
render state and shells out to a real CLI binary (`curl`) client-side, no
shell, real argv. This document states the shared pattern once and focuses
on where `curl` genuinely differs — a Postman-style REST API client is a
different shape of problem than a dashboard over live external resources:

- **Editable key-value tables.** docker/kubectl only ever populate
  *read-only* display tables from `update_*` snapshots. Params/Headers/
  Form-body/Environment-variables are user-typed input lists — see
  §3 "`kv_table`".
- **Local persistence.** docker/kubectl reflect purely external, live
  state (the Docker daemon, a Kubernetes cluster) with nothing to persist.
  Collections, Environments, and History are the client's own durable app
  state — see §6 "Persistence".
- **Response parsing without a JSON library.** Same "no JSON library"
  constraint as docker/kubectl's tab-delimited `--format` trick, applied to
  an actual HTTP response: `curl -i` plus a `-w` sentinel trailer, parsed
  with plain string operations — see §6 "Command construction & response
  parsing".

## 1. Purpose / Scope

The `curl` module (`wish client --run=curl`) is a Postman-style GUI
frontend for building and sending REST API requests via the local `curl`
binary: a request builder (method, URL, query params, headers, body, a
simple auth helper), a response viewer (status/timing/size, headers, a
JSON-aware pretty-printed body), request History, named/saved requests
organized into Collections, and Environments (named variable sets
substituted into `{{name}}` placeholders at send time). It is a **pure
frontend** — every request runs the real `curl` binary already installed
on the machine; this module never links libcurl (the repo vendors it for
`wish_server`'s own internal HTTP needs, `net::http_get` — a private,
server-side dependency unrelated to this module's client-side, argv-only
convention) and never speaks HTTP itself.

Scope for this pass:

- **Request building**: method, URL, query params, headers, three body
  modes (raw, JSON, form URL-encoded), and a Basic/Bearer auth helper.
- **Response viewing**: status line (code/text/time/size, colour-coded),
  headers table, and a JSON-aware body viewer (pretty-printed + syntax
  highlighted when the response looks like JSON, plain text otherwise, a
  one-line placeholder for binary bodies).
- **History**: every sent request, newest first, reloadable into the
  builder. Persisted locally, capped at 200 entries.
- **Collections**: named/saved requests grouped by collection name,
  reloadable/duplicable/deletable.
- **Environments**: named variable sets; the active one (picked in the
  Request window's toolbar) is substituted into `{{name}}` placeholders in
  the URL, params, headers, body, and auth fields at send time.
- **Console**: a FIFO-capped trace of every `curl` invocation — the
  docker/kubectl Console pattern.

Explicitly deferred — see §10: multipart/file-upload bodies, a cookie jar,
GraphQL/WebSocket helpers, request chaining or scripted tests (Postman's
"Tests" tab / pre-request scripts), OAuth2 flows beyond static
Basic/Bearer, collection/environment rename (delete + re-save / duplicate
covers the common case for v1), true streaming responses.

This directory owns:

- `server/curl.hpp`/`.cpp` — the `CurlFrontend` form (six windows, the
  `update_*` render methods, the `append_command_log` Console trace
  method, the `*_requested` events, the editable `kv_table` plumbing).
- `client/curl.hpp`/`.cpp` — the client runner (`run_curl`, event wiring,
  app registration).
- `client/curl_process.hpp`/`.cpp` — a non-interactive libuv-based "run
  `curl <args>`, capture output" helper (unmodified copy of
  `docker_process`'s shape).
- `client/curl_source.hpp`/`.cpp` — every actual `curl` invocation,
  environment-variable substitution, and the local persistent store
  (Collections/Environments/History).
- `client/curl_response_parser.hpp`/`.cpp` — `parse_curl_output()`: the
  `-i`/`-w`-sentinel response parse, split out into its own
  dependency-free file specifically so it's unit-testable on its own
  (see §6, §9 — this is where the module's one live-verification-found
  bug was) rather than only reachable through `curl_source`'s full
  proxy/session machinery.
- `README.md` — user-facing usage.
- `curl_mock.json` — the UI mockup validated in the `editor` tool before
  implementation (mirrored by `server/curl.cpp`'s window layout strings).

## 2. Design Goals

Goals 1–5 are identical to docker's (§2 there): all `curl` invocation and
parsing is client-side; the server owns all UI/render state; no shell in
any `curl` invocation (real argv via `uv_spawn`); a rebuild is a full
rebuild, never an incremental patch — **except** for the `kv_table` rows
described below, which are added/removed individually by design (§3); and
destructive actions (delete a saved request, delete an environment) are
confirmed via the built-in `MessageBox`, reversible ones are not.

**Goal 6 (new): a `kv_table` row's live value is the only source of
truth.** Params/Headers/Form-body/Environment-variable rows are never
mirrored into a separate C++ struct while the user is editing them —
`collect_request_state()` (and `kv_table_read()`) reads each row's
`InputText`/`Checkbox` widgets' own `"value"` field directly, at the
moment of Send/Save. This means no `"changed"` event handler is needed for
any of those fields (contrast docker's `filter_input_`, which *is*
reactive because it drives live filtering) — see §6.

## 3. Key Abstractions

### `CurlFrontend` (server, `form`)

Bison class `"CurlFrontend"` in the `"wish"` namespace. Owns **six
independently dockable `Window`s**: Request (the main root), Response,
History, Collections, Environments, and Console. Root keys are
`internal_root_key_` and `internal_root_key_ + "_response"/"_history"/
"_collections"/"_environments"/"_console"`.

- **Request**: a toolbar (Method `Combo`, URL `InputText`, an Environment
  `Combo`, a Follow-redirects `Checkbox`, Send), an inline "Save as" row
  (name + collection `InputText`s + Save — the same resolution docker uses
  for pull-image/create-volume: `MessageBox` has no custom-body slot, so a
  small always-visible inline field stands in for a dialog), a status
  `Label`, and a `TabBar` with four `TabItem`s: **Params** and **Headers**
  (each an editable `kv_table`, §3.1), **Body** (a mode `Combo` toggling
  between a raw/JSON multiline text box and a `kv_table` for Form
  URL-Encoded fields), and **Auth** (a mode `Combo` toggling between
  Basic-Auth username/password fields and a Bearer token field).
- **Response**: a status `Label` and a `TabBar` with **Body** (a
  read-only `TextEditor` pointed at a client-uploaded sandbox file,
  `language:"json"` when the response looks like JSON) and **Headers**
  (a plain 2-column `Table`, fully rebuilt per response).
- **History**: a FIFO-style table (Method/URL/Status/Time/When), newest
  first, capped at 200 locally-persisted entries; each row's "Load" menu
  action restores the *entire* builder state (not just method/URL) via
  `update_request_builder`.
- **Collections**: a flat table (Collection/Name/Method/URL, no nested
  tree widget — sufficient for v1); row actions Load/Duplicate/Delete
  (Delete gated behind `MessageBox`).
- **Environments**: a list table (Name/Variable count) with row actions
  Edit/Delete (Delete gated), plus an inline "New Environment" field, and
  below it a second `kv_table` (no "enabled" column) editing whichever
  environment was last "Edit"ed, with its own "Save Variables" button.
  The *active* environment (used for `{{name}}` substitution) is a
  separate piece of state — the Request window's own toolbar `Combo`, not
  anything stored here (mirrors docker's `state_filter_`: a Combo
  selection is ordinary server-side UI state, not part of a pushed
  snapshot).
- **Console**: verbatim the docker/kubectl Console pattern — a FIFO-capped
  trace table fed by `append_command_log`.

Closing any window emits `"closed"` and tears down all six subtrees
(`remove_objects_at` for the five extra roots + `remove_internal_objects`).

#### 3.1 `kv_table` — editable key/value row lists

One shared component backs Params, Headers, the Form-body fields, and
Environment variables — four uses of one shape (`curl.hpp`'s `kv_table`/
`kv_row`). Each row is `[Checkbox?] [InputText key] [InputText value]
[Button "x"]` (`Checkbox` omitted for Environment variables, which have no
"enabled" concept). Three operations, deliberately **not** a
docker-style full rebuild:

- `kv_table_add_row()` appends one row (an "+ Add ..." button per table).
- `kv_table_remove_row()` erases exactly the row whose "x" button was
  clicked (`kv_remove_targets_`, a widget-id → `kv_table*` map, same shape
  as docker's `menu_action_targets_`) — every other row's live-typed text
  is untouched.
- `kv_table_load()` is the one place that *does* fully clear-and-rebuild a
  `kv_table` — used only when deliberately overwriting the builder from
  stored data (`update_request_builder`, `update_environment_vars`).

Reading current values (`kv_table_read()`) walks `kv.rows` and calls
`->as<std::string>("value"_key)` / `->as<bool>("value"_key)` directly on
each row's stored `ui_element_ptr` — the same field a real client render
would have last written via ordinary interaction. No "changed" handler
ever fires for a `kv_table` row's `InputText`s; the widgets themselves
already hold the live truth.

### `curl_source` (client)

Owns the proxy, every actual `curl` invocation, environment substitution,
and the local persistent store. `refresh_all()` loads the store and pushes
the three snapshot RMI calls (`update_collections`/`update_environments`/
`update_history`). One `on_*` method per `*_requested` event; `send_request()`
is the core path (§6). `push_request_builder()` is the "load into the
builder" push, shared by History-Load and Collections-Load.

### `curl_process::run_curl_cli()` (client)

Byte-for-byte the same shape as `docker_process::run_docker_cli()` — see
that module's DESIGN.md §3. No new subprocess capability was needed for
this module: everything module-specific (headers-in-stdout, the `-w`
sentinel trailer) lives in argv construction and output parsing in
`curl_source`, not in the process helper.

## 4. Data Flow / Architecture

```
Startup:
  run_curl(host)
    `curl --version`
      fail -> print "curl: `curl` binary not found on PATH ..." + signal_done()
    instantiate CurlFrontend -> proxy
    wire proxy.onEvent(...) for every *_requested / closed event
    source->refresh_all()   -- loads the local store, pushes Collections/
                                Environments/History (docker's initial-
                                load-race fix: never an on_init()-emitted
                                event)

Send (Request window "Send" button, server-side):
  collect_request_state() reads every live widget directly (method/url/
    follow/env combos, kv_table_read() x3, body/auth mode combos + fields)
    -> emit "send_requested" { ...full builder state... }

send_requested (client):
  decode_request_state(payload) -> request_state
  resolve_environment(state)    -- {{name}} substitution against the
                                    active environment's stored vars
  build argv: -sS -i [-L] -X <method> [-H "K: V"]... [-u user:pass |
              -H "Authorization: Bearer <token>"] [--data-raw <body>]
              -w '\n__WISH_CURL_META__\t%{http_code}\t%{time_total}\t%{size_download}\n'
              <url with percent-encoded query params appended>
  run_curl_cli(argv) -> process_result
  push_command_log(argv, result)               -- Console trace
  parse_curl_output(stdout)                     -- see §6
    -> update_response { ok, status_code, status_text, time_ms,
         size_bytes, headers, body_file, body_is_json }
       (body_file: uploaded via host.upload_file() unless the body looks
        binary, then left empty -> Response window shows no body pane)
  append one History entry (full un-substituted request_state, so a later
    "Load" restores {{name}} literally, not its resolved value)
  save_store(); push_history()

CurlFrontend.update_response (server):
  colour-codes the status line by status_code (2xx green / 3xx amber /
    4xx-5xx red / connection failure red with `error`), fully rebuilds the
    Response Headers table, points the Body TextEditor's file_path/
    language at the client-uploaded file (or clears it on failure/binary)

Load (History or Collections row "Load"):
  emit load_history_requested / load_request_requested { id }
  client looks up the stored entry, calls push_request_builder(state)
    -> update_request_builder { ...full state... }
  server: kv_table_load() x3, sets every Combo/InputText field directly,
    then calls apply_body_mode_visibility()/apply_auth_mode_visibility()
    explicitly -- a server-side field write never triggers on_event, so
    the Raw/Form and Basic/Bearer box visibility must be re-applied by
    hand after a programmatic mode change (unlike a real user click on
    the mode Combo, which goes through on_event's "changed" branch).
```

## 5. Public API Contract

| Symbol | Contract |
|---|---|
| `CurlFrontend.update_response(args)` | RMI method. `{ ok, error? (connection failure only), status_code, status_text, time_ms, size_bytes, headers: [{key,value}], body_file, body_is_json }`. |
| `CurlFrontend.update_request_builder(args)` | `{ method, url, params/headers/form_fields: [{key,value,enabled}], body_mode, body_text, auth_mode, auth_username, auth_password, auth_token }`. Pre-fills the Request window from a stored History/Collections entry. |
| `CurlFrontend.update_history(args)` | `args.entries` — each `{ id, method, url, status_code, ok, time_ms, timestamp }`, newest first. |
| `CurlFrontend.update_collections(args)` | `args.entries` — each `{ id, collection, name, method, url }`. |
| `CurlFrontend.update_environments(args)` | `args.entries` — each `{ id, name, var_count }`. Also refreshes the Request window's Environment `Combo`. |
| `CurlFrontend.update_environment_vars(args)` | `{ environment_id, name, vars: [{key,value}] }`. Populates the Environments window's variable editor; an empty `environment_id` clears it. |
| `CurlFrontend.append_command_log(args)` | `{ command, exit_code, ok, output }`. Console trace row — identical contract to docker's. |
| `"send_requested"` event | The full builder state (method/url/params/headers/body/auth/follow_redirects/environment), snapshotted at Send. |
| `"save_request_requested"` event | The builder state plus `{ collection, name }` from the inline Save-as fields. |
| `"load_request_requested"` / `"delete_request_requested"` / `"duplicate_request_requested"` | `{ id }` — a Collections row action. |
| `"load_history_requested"` event | `{ id }` — a History row action. |
| `"clear_history_requested"` event | No payload. |
| `"new_environment_requested"` event | `{ name }`. |
| `"delete_environment_requested"` / `"select_environment_requested"` | `{ id }` — an Environments row action. |
| `"save_environment_vars_requested"` event | `{ id, vars: [{key,value}] }` — the Environments window's "Save Variables" button. |
| `"closed"` event | Any window's X was clicked; all six subtrees torn down. |
| `wish client --run=curl` | No positional args. |

The internal `ui_element` tree of every window is private.

## 6. Design Decisions

- **No libcurl, ever, in this module.** The repo vendors `libcurl` and
  links it `PRIVATE` into `wish_server` for `net::http_get` — an internal
  server-side dependency, not exposed to module-client targets, and
  irrelevant to this module's convention: `wish client --run=curl` is
  explicitly a *frontend for the `curl` binary*, so it shells out, exactly
  like docker/kubectl shell out to their respective binaries. Using
  libcurl here would need a different client/server split than every
  other frontend module in this repo.

- **`kv_table` rows are read live, never mirrored into a `"changed"`-driven
  C++ struct** (§3.1). docker/kubectl's read-only tables are always
  rebuilt from an external snapshot, so their filter/toggle fields
  (`filter_text_`, `state_filter_`) are reactive by necessity — they drive
  *filtering of already-known data*. A `kv_table` row holds the *only*
  copy of what the user typed; reading it at Send/Save time is simpler
  and, critically, means adding/removing one row never disturbs any other
  row's in-progress edit (a full rebuild-on-every-change, docker's Goal
  4, would blow away whatever the user was mid-typing in every other row).

- **Response headers/body/status are parsed from one captured stdout
  stream — `-i` plus a `-w` sentinel trailer, no temp files.** `curl -i`
  prepends the (final, post-redirect) status line and headers to the body
  on stdout; a `-w '\n__WISH_CURL_META__\t%{http_code}\t%{time_total}\t
  %{size_download}\n'` trailer appends one machine-parseable line after
  the body. `parse_curl_output()` (curl_source.cpp) strips that trailer
  **first**, by locating `__WISH_CURL_META__` in the raw, unsplit stdout
  and trimming exactly the one `\n` its format string's own leading
  newline put immediately before it — only *then* does it scan the
  remaining headers+body text for blank-line boundaries (`-L` prints one
  header block per redirect hop — the *last* boundary is the true
  header/body split) and isolate the final hop's status line + headers.
  This order matters: scanning for boundaries *before* removing the
  trailer looks correct on a response with no body, but a body that
  itself ends in a trailing newline (routine for a JSON API) plus the
  trailer's own leading `\n` together form a *second*, spurious blank-line
  boundary right before the sentinel — scanning first picks that one as
  "the" header/body split and silently leaves the real body empty (caught
  during the automation-driven end-to-end pass against a live endpoint,
  §10 — `get_tree()`/a screenshot showed a real 200 response with correct
  status/timing/size and a populated Console trace, but an empty Response
  Body pane; `cat -A` on a hand-run copy of the exact same `curl` argv
  showed the extra blank `^M$`-less line right before
  `__WISH_CURL_META__`). This is the same "encode structure into the
  CLI's own output format, parse with plain string ops, no JSON
  dependency" trick as docker's tab-delimited `--format` — applied here to
  an actual HTTP response instead of a `docker ps` table. Considered and
  rejected: a two-temp-file approach (`-D <headers-file>`, `-o
  <body-file>`) — unnecessary complexity once `-i` already interleaves
  headers and body deterministically on one stream, and `curl_process`
  would have needed a new capability (temp-file plumbing) that this
  design avoids entirely.

- **Query params are percent-encoded and appended into the URL by
  `curl_source` itself, not via curl's `-G`.** Keeps URL construction
  independent of the chosen HTTP method (curl's `-G` semantics around
  interacting with an explicit `-X` vary by version) and mirrors the
  "we own the parsing, curl just executes" posture used for headers/body.

- **`-u user:pass` for Basic auth, a synthetic `Authorization: Bearer
  <token>` header for Bearer.** `-u` is curl's own, already-correct Basic
  auth implementation — no hand-rolled base64 needed. Bearer has no curl
  flag, so it's just another `-H`. **Precedence**: if the user has *also*
  manually added an `Authorization` header in the Headers tab, that
  manual header wins and the Auth tab's Bearer header is skipped
  (`has_header()` check in `send_request()`) — an explicit, documented
  rule rather than silently sending two conflicting `Authorization`
  headers.

- **`--data-raw`, never `-d`/`--data`.** `--data-raw` never interprets a
  leading `@` as "read from this file" — the same defensive choice
  docker/kubectl make for argv values that come from arbitrary user text.
  `-X <method>` is always passed explicitly regardless of body presence,
  so the chosen method never depends on curl's "a body implies POST"
  default.

- **A HEAD request also gets `--head`, not just `-X HEAD`.** Found live
  (2026-09, automation module against `https://httpbin.org/anything`):
  `-X HEAD` alone still leaves curl expecting `Content-Length` bytes of
  body; when a real HEAD response correctly sends none, curl reports exit
  18 ("transfer closed with N bytes remaining to read") even though the
  request itself succeeded — `send_request()` surfaced this as a false
  "Request failed" for a request that actually worked. `--head` (curl's
  own `-I`) is what actually suppresses that expectation; confirmed
  directly (`curl -X HEAD ...` → exit 18, `curl --head -X HEAD ...` →
  exit 0 on the identical URL) before fixing. `send_request()` now adds
  `--head` specifically when the selected method is `HEAD`, alongside
  (not instead of) the usual `-X HEAD` — every method is still passed the
  same uniform way.

- **A binary-body heuristic, not real Content-Type sniffing.** The
  Response Body viewer needs *some* file to point a `TextEditor` at (or
  none, for a one-line "binary response" fallback) — v1 checks for an
  embedded NUL byte in the parsed body as the binary/non-binary signal.
  Simple and correct for the overwhelmingly common REST-API-testing case
  (JSON/text/XML bodies); a real image/PDF download would need a proper
  Content-Type allowlist, deferred (§10).

- **Each response body is uploaded under a fresh, unique sandbox filename
  — never a fixed `curl_response.txt`.** Found live (2026-09, automation
  module cycling all seven HTTP methods against one URL in a single
  session): with a fixed filename, the Response Body `TextEditor`'s
  `file_path` field is byte-for-byte identical response after response
  even though the sandbox file's *content* legitimately changed each
  time — nothing ever signals the client-side editor to re-fetch it, so
  the Body pane silently kept showing the *first* response's body forever
  while the status line, headers table, and Console (all driven by
  ordinary RMI field writes, not a file re-fetch) updated correctly every
  time. `send_request()` now names each upload `new_id("curl_response") +
  ".txt"`, so `file_path` genuinely changes on every response and the
  editor always reloads.

- **Local persistence lives outside the session sandbox, in a small
  hand-rolled JSON store.** See DESIGN.md-level rationale in
  `curl_source.hpp`'s file comment: Collections/Environments/History are
  the *client's own* durable app state (same trust boundary as
  `imgui.ini`), not a server-directed, per-session widget file path — so
  `session::resource_dir` sandboxing (CLAUDE.md's file-access rules) does
  not apply here; those rules govern paths the *server* hands the
  client, not files the client's own reference app manages for itself.
  The store is one JSON file at a per-user config location
  (`$XDG_CONFIG_HOME/wish/curl/store.json`, falling back to
  `~/.config/wish/curl/store.json`; `%APPDATA%\wish\curl\store.json` on
  Windows/MSYS2 — a narrow `#if defined(_WIN32)` guard in
  `curl_source.cpp`, not a separate `_win`-suffixed file, per this repo's
  platform-support rule). No JSON library dependency: a ~250-line
  hand-rolled reader/writer in `curl_source.cpp`'s anonymous namespace,
  scoped to this module's own flat schema (never exposed as a
  general-purpose parser) — the same constraint that shaped docker's
  tab-delimited list parsing (`nlohmann::json`'s include path is
  `PRIVATE` to a handful of core wish targets, not module-client
  sources).

- **History stores the full, un-substituted builder state per entry, not
  just a method/URL/status summary.** A History row's "Load" action is
  expected to restore *everything* — params, headers, body, auth — not
  just re-populate the URL bar. Storing the state *before*
  `resolve_environment()` means a reloaded request still shows `{{name}}`
  literally (Postman's own behavior), re-resolved against whatever
  environment is active at the time it's next sent, not frozen at the
  value it happened to resolve to originally.

- **Loading a stored request must explicitly re-apply body/auth
  visibility.** `on_event`'s `"changed"` branch is what normally keeps
  the Raw/Form and Basic/Bearer box pairs in sync with their mode
  `Combo`s, but `update_request_builder`'s field writes are ordinary
  server-side renders, not simulated user input — they never reach
  `on_event`. `apply_body_mode_visibility()`/`apply_auth_mode_visibility()`
  are factored out specifically so both the interactive path and the
  programmatic-load path call the same logic (curl.hpp's doc comment on
  these methods states this explicitly, since it is easy to add a new
  mode-driven visibility toggle later and forget the second call site).

- **Separate dockable windows, no unified nav sidebar** — identical
  rationale to docker's (§6 there): every `Window` docks into the host's
  implicit dockspace already; a single-window-with-tabs shape would be
  less flexible and isn't the wish-native convention this repo's other
  frontend modules established.

## 7. Constraints and Invariants

- The server form never touches `curl`, the filesystem, a socket, a
  subprocess, or the local persistent store; `curl_process`/`curl_source`
  (client-only) own all of that entirely.
- Every `curl` invocation is a real argv array through `uv_spawn` — no
  shell string is ever constructed.
- `kv_table_add_row()`/`kv_table_remove_row()` never touch any row other
  than the one being added/removed; only `kv_table_load()` may fully
  clear a `kv_table`.
- Every `update_*` handler that overwrites a `Combo` driving Body/Auth
  visibility must call the matching `apply_*_visibility()` afterward.
- `run_curl_cli()`'s `binary` argument is never passed anything but the
  default in production code — only tests supply it.
- The local store is written synchronously (`save_store()`) after every
  mutation (send, save/delete/duplicate a request, new/delete an
  environment, save variables, clear history) — never batched or
  deferred, so a crash mid-session loses at most the in-flight action.

## 8. Integration Boundaries

Depends on:

- `wish::form`, `ui_root::on_event`'s catch-all dispatch, `import_json()`
  — same pattern every other module uses.
- `message_box` (`src/ui/forms/message_box.hpp`) +
  `form::instantiate_child_form()` — the destructive-action confirm
  dialog (Collections/Environments Delete).
- `TextEditor` (`file_path`, `language`, `read_only`), `Table`/
  `TableColumn`/`TableRow`, `TabBar`/`TabItem`, `MenuButton`/`MenuItem`,
  `Combo`, `InputText` (including `multiline`), `Checkbox`,
  `HorizontalLayout`/`VerticalLayout` — all existing wish elements, no
  new widget needed.
- `wish_app_host::upload_file()` — writes the response body into the
  session sandbox for the Response Body `TextEditor` (the nano/editor
  upload pattern).
- `uv_a` (libuv) — `curl_process`'s subprocess helper; already linked
  into module-client targets by `wish_finalize_app_modules()`.
- No JSON library — a hand-rolled parser/writer scoped to this module's
  own store schema (§6); `curl`'s response body is shown verbatim
  (pretty-printing is `TextEditor`'s own JSON-aware rendering, not
  something this module parses).
- `std::filesystem` — creating the local store's config directory.
- The system `curl` binary (must be on `PATH`).

Depended on by: nothing else in wish; this is a leaf module.

## 9. Testing

- **`tests/test_curl.cpp`** — instantiates `CurlFrontend` over
  `memory_transport` and asserts: all six windows/expected widgets exist;
  adding/removing a Params row changes only that row's presence; Send
  emits `send_requested` with the exact method/url/headers currently in
  the live widgets; Save with a blank name is a no-op, with a name emits
  `save_request_requested`; Body/Auth mode `"changed"` events toggle the
  right box's `visible` field; `update_response` renders status colour/
  headers/error correctly; `update_request_builder` pre-fills fields and
  re-applies visibility; History/Collections/Environments rebuild their
  tables from `update_*` and their row actions emit the right
  `*_requested` events (Collections Delete through a `MessageBox` confirm,
  mirroring `test_docker.cpp`'s `ConfirmRemoveYesEmits...` pattern);
  closing any window emits `"closed"` and tears down every root. No
  network access required.
- **`tests/test_curl_process.cpp`** — `run_curl_cli({"hello"}, "printf")`
  captures stdout; `run_curl_cli({...}, "false")` reports a non-zero exit;
  a missing binary reports `exit_code == -1`. Compiles
  `client/curl_process.cpp` directly and links `uv_a`, exactly like
  `tests/test_docker_process.cpp`. No network access required.
- **`tests/test_curl_response_parser.cpp`** — pure `parse_curl_output()`
  string-parsing cases, zero framework dependency: a body ending in its
  own trailing newline is preserved (the httpbin.org regression, see §6/
  §10); a body with no trailing newline is preserved exactly; a simulated
  `-L` redirect chain isolates the *final* hop's status/headers; a body
  containing its own embedded blank line is not mistaken for an extra
  redirect hop and truncated (the google.com regression, see §6/§10);
  empty input and sentinel-less/truncated input degrade gracefully instead
  of crashing. Fastest-building of the three test binaries (no bison/RMI/
  libuv link needed at all). No network access required.
- **End-to-end** (see §10): two rounds of automation-module runs against
  real endpoints. Round 1 (`https://httpbin.org/get`, then
  `https://www.google.com`) found and fixed the two response-parsing bugs
  documented in §6. Round 2 (all seven HTTP methods cycled against
  `https://httpbin.org/anything` in one session, plus a fresh session
  each for a `-L` redirect chain and 404/500 status colouring) found and
  fixed the HEAD `--head` bug and the stale-response-body-file bug, also
  in §6, and confirmed status-line colour coding (green 2xx, red 4xx/5xx)
  and `-L` redirect-chain handling both render correctly. Reload-from-
  History/Collections, delete-confirm, and environment substitution are
  unit-tested and (per the corrected docs/automation.md guidance) are
  known to be drivable live the same way, but weren't re-exercised in
  this pass — see PLAN.md's Verification checklist.

## 10. Implementation Status

**Implemented and live-verified.** `server/curl.{hpp,cpp}`,
`client/curl*.{hpp,cpp}`, `client/curl_response_parser.{hpp,cpp}`,
`tests/test_curl*.cpp` are in place; all three test binaries (`test_curl`,
`test_curl_process`, `test_curl_response_parser`) require neither network
access nor a running server and all 29 cases pass. Two automation-module
verification passes against real endpoints (see §9's "End-to-end" bullet)
found and fixed four real bugs, all now covered by regression tests or
documented fixes:
1. A JSON response body ending in its own trailing newline was silently
   dropped (found against `https://httpbin.org/get`).
2. A large HTML/JS body containing its own coincidental blank-line
   sequence was truncated and partly misparsed as headers (found against
   `https://www.google.com`).
3. `HEAD` requests failed with curl exit 18 even though they succeeded,
   because `-X HEAD` alone doesn't suppress curl's body-length check
   (found against `https://httpbin.org/anything`).
4. The Response Body pane silently kept showing the *first* response's
   content forever once more than one request had been sent in the same
   session, because every response reused the same sandbox filename
   (found by cycling all seven HTTP methods in one session against
   `https://httpbin.org/anything`).

**Not planned for v1** (deferred future work): multipart/file-upload
request bodies; a cookie jar (curl's `-c`/`-b` are stateless per-request
here); GraphQL/WebSocket-specific helpers; request chaining or scripted
pre-/post-request assertions (Postman's "Tests" tab); OAuth2 flows beyond
static Basic/Bearer; Collections/Environments rename (Delete + re-save, or
Duplicate, cover v1's common case); a nested collection-folder tree (the
Collections table is flat, grouped by a `collection` string column); true
response streaming for very large bodies; Content-Type-based (rather than
NUL-byte-heuristic) binary-body detection.
