# Profiling

wish inherits bison's Perfetto track-event profiler and instruments its own
render loop, ImGui renderer, and layout engine with it. A captured trace
opens directly at https://ui.perfetto.dev/ — no Perfetto SDK or protobuf
dependency required. See bison's [docs/profiling.md](../extern/bison/docs/profiling.md)
for the underlying mechanism (`BISON_TRACE_SCOPE`, `enable_profiling()`,
`attach_profiling()`).

## Capturing a trace from `wish-server`

```sh
wish-server --renderer=sdl3 --profiling_dir=./traces --profiling_autostart
```

- `--profiling_dir=DIR` enables profiling and writes the trace file under
  `DIR`. Omit it (default `""`) to disable profiling entirely — the
  `BISON_TRACE_SCOPE` calls in the render loop become no-ops.
- `--profiling_autostart` starts capture immediately at server startup.
  Without it, profiling is enabled but idle until something starts capture
  via the `__BisonProfiler` RMI singleton (see bison's doc above).

The trace is finalized and flushed to disk when the server shuts down
(Ctrl+C, window close, or the client disconnecting a `--transport=term`
session) — capture is always stopped before `server::stop()` runs.

## Instrumented scopes

| Scope | Where | Fires |
|-------|-------|-------|
| `render_loop_tick` | `src/server/server.cpp::render_loop()` | Once per render loop iteration |
| `tick` | `src/server/server.cpp::render_loop()` | Around `renderer_->tick(...)` |
| `render_frame` | `src/server/server.cpp::render_loop()` | Around the per-session render pass |
| `render_session` | `src/imgui/imgui_renderer.cpp` | Once per session, per frame |
| `render_node` | `src/imgui/imgui_renderer.cpp` | Once per UI element, per frame |
| `measure_node` | `src/imgui/imgui_layout.cpp` | Per layout measure pass |
| `arrange_node` | `src/imgui/imgui_layout.cpp` | Per layout arrange pass |
| `ensure_arranged` | `src/imgui/imgui_layout.cpp` | Layout self-heal check before render |

`measure_node`/`arrange_node` nested repeatedly under a single
`render_frame` slice indicates the layout engine is re-walking the tree
more than once per frame — check `ensure_arranged`'s call sites first.

## Client-side capture

The native CLI client (`wish client`) attaches a `client_recorder` on every
session via `bison::rmi::attach_profiling(c)` in `wish_client_app::on_session()`.
This only records anything when the server it connects to has profiling
enabled; it is otherwise inert.

## Viewing a trace

Open https://ui.perfetto.dev/, choose "Open trace file", and select the
`.perfetto-trace` file under `--profiling_dir`. Each traced thread (server
render thread, client thread) appears as its own track.
