# curl

A Postman-style GUI frontend for the local `curl` command line: a request
builder (method, URL, query params, headers, body, a Basic/Bearer auth
helper), a response viewer (status/timing/size, headers, a JSON-aware
pretty-printed body), request History, named requests organized into
Collections, and Environments (`{{name}}` variable substitution). This is
a pure frontend — every request runs the real `curl` binary already on
the machine; it never links libcurl or speaks HTTP itself.

`wish client --run=curl` (no positional args). Opens as six independently
dockable windows (drag/resize/tab like any other wish window): Request,
Response, History, Collections, Environments, Console.

- **server/**: `CurlFrontend` form (`register_curl()`) — renders whatever
  snapshot it was last given via `update_response` / `update_history` /
  `update_collections` / `update_environments` / `update_environment_vars`
  / `update_request_builder` / `append_command_log`, and emits
  `*_requested` events (see `server/curl.hpp`'s class doc comment for the
  full contract) for the client to react to. Also owns the Params/
  Headers/Form-body/Environment-variable *editable* key-value row lists
  (`kv_table`) — unlike docker/kubectl's read-only tables, these hold
  live user input, read directly off the widgets at Send/Save time.
- **client/**: `run_curl(wish_app_host&)`, self-registered as the
  `"curl"` embedded app. `client/curl_process.hpp`/`.cpp` is a small,
  non-interactive, libuv-based (`uv_spawn`) "run this argv array, capture
  stdout/stderr/exit code" helper (a near-copy of `docker_process`).
  `client/curl_source.hpp`/`.cpp` builds and runs every actual `curl`
  invocation, parses the response (`-i` for headers + a `-w` sentinel
  trailer for status/timing/size — no temp files, no JSON library),
  applies `{{name}}` environment substitution, and owns a small local
  JSON store for Collections/Environments/History (a per-user config
  file, e.g. `~/.config/wish/curl/store.json` on Linux/MSYS2 —
  independent of the session sandbox, since this is the client's own
  local app state).
- **resources/**: none.

Build: off by default. `cmake -S . -B build -DWISH_MODULE_BDG_DEV_CURL=ON`
(or `-DWISH_COLLECTION_BDG_DEV=ON` for the whole `bdg/dev` collection).

See [DESIGN.md](DESIGN.md) for the full architecture and [PLAN.md](PLAN.md)
for what's implemented vs. deferred.

## Implementation status

All six windows are implemented and unit-tested. The request/response/
Console/Save-to-collection path is also live-verified against a real HTTP
endpoint; reload/delete/environment-substitution are implemented and
unit-tested but not yet driven through a live browser session (a dock-tab
automation limitation, not a known product issue — see PLAN.md's
Verification checklist):

- **Request** — Method/URL/Environment/Follow-redirects toolbar, an inline
  "Save as" row, and four tabs: Params/Headers (editable key-value tables
  with an enable checkbox), Body (None/Raw/JSON/Form URL-Encoded), Auth
  (None/Basic/Bearer).
- **Response** — a colour-coded status line, a Headers table, and a
  read-only `TextEditor` body view (JSON syntax highlighting when the
  response looks like JSON; a one-line placeholder for binary bodies).
- **History** — every sent request, newest first, capped at 200 and
  persisted locally; "Load" restores the entire builder state.
- **Collections** — named/saved requests grouped by a collection name;
  Load / Duplicate / Delete (Delete gated behind a confirm dialog).
- **Environments** — named variable sets with their own key-value editor;
  the active environment (picked in the Request window) is substituted
  into `{{name}}` placeholders across the URL, params, headers, body, and
  auth fields at send time.
- **Console** — a scrolling, FIFO-capped table tracing every `curl`
  invocation the module ran (`#` / Command / Exit / Output), green on
  success, red on failure — the docker/kubectl Console pattern.

## Known limitations (v1)

- **No multipart/file-upload request bodies, no cookie jar.**
- **No GraphQL/WebSocket helpers, no request chaining or scripted
  pre-/post-request tests.**
- **Auth is static Basic/Bearer only** — no OAuth2 flows.
- **No Collection/Environment rename** — Delete + re-save, or Duplicate,
  cover the common case for v1.
- **Binary-vs-text body detection is a simple heuristic** (an embedded
  NUL byte), not real Content-Type-based sniffing.
- **`curl` must be on `PATH`.** No in-app installation/setup flow.

See [PLAN.md](PLAN.md) for the complete list of deferred features and the
end-to-end verification checklist.
