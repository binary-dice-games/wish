# Changelog

All notable user-facing changes to wish are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `docker` module (`WISH_MODULE_BDG_DEV_DOCKER`, off by default): a Docker Desktop-style GUI frontend for the local `docker` CLI, run with `wish client --run=docker`. Ships four dockable windows — Containers, Images, Volumes, Networks — each a table with a state-aware per-row `...` action menu (start/stop/restart/pause/unpause/kill/remove, image run/remove, volume/network remove, inspect), plus name/image and running/stopped container filters, inline pull-image and create-volume fields, and a `MessageBox` confirm on stop/kill/remove/prune. All `docker` invocation is client-side; refresh is manual (Refresh button + after every action), no background polling.
- `wish_server_set_log_level()` (C ABI) and `set_log_level()` / `SetLogLevel()` (C++, Python, C# bindings): set the server log verbosity by name.
- Rust (`bindings/rust/`) and Go (`bindings/go/`) client language bindings for the wish client C ABI.
- Rust (`bindings/rust/wish-server/`) and Go (`bindings/go/wishserver/`) server language bindings: host and render a real wish session over `wish_server_c.h` (`Server`, `Params`).
- `examples/ui/json/` and `examples/ui/yaml/` — standalone editor example UIs, one file per tab of the `demo` app, in both formats.
- `tooltip` field on every UI element: when non-empty, its text is shown via `ImGui::SetTooltip()` while the element is hovered.
- `VerticalLayout` (and `TabItem`) gain a `scroll` boolean field: when true, the layout's children render inside a vertically-scrolling child region sized to the space the layout was given, so overflowing content scrolls instead of clipping the enclosing window.

### Fixed

- `CollapsingHeader` could not be collapsed/expanded by clicking unless the app wrote the new state back onto its `open` field in the `toggled` handler. The renderer now writes `open` back on toggle itself (matching `Checkbox`/`Slider`), so clicks work without app-side wiring.

### Changed

- `editor` module: loads `.yaml` / `.yml` UI files in addition to JSON, selected by file extension, with full feature parity — YAML syntax highlighting, schema-aware autocomplete, and the cursor-tracked Help panel / preview highlight all work for YAML sources.
- `editor` module: closing with unsaved edits now uses a real modal `MessageBox` ("Save changes … before closing?", Yes / No / Cancel) instead of an inline panel.
- `TextEditor`: new `"yaml"` value for the `language` field (YAML syntax highlighting); `wish_ui_schema` autocomplete now also applies when `language` is `"yaml"`.
- `CollapsingHeader`'s `open` field is now server-authoritative: it is forced into ImGui every frame (`ImGuiCond_Always`) instead of being applied once and then owned by ImGui. Update the field in the `toggled` handler to keep the header responsive to clicks. This fixes a spurious open/close oscillation when a `CollapsingHeader`'s subtree is rebuilt every frame.
- `--verbose` is now a verbosity level (`none` | `fatal` | `error` | `warning` | `info` | `trace`, default `none`) instead of an on/off flag. RMI trace lines and session lifecycle lines in `wish_logs/server.log` (and mirrored to stdout) are produced only at `info` and above; decoded call payloads (`args=`, `set` values, response bodies) only at `trace`. `fatal`/`error`/`warning` only raise the severity floor for client log messages. At `none` (the default) the server log stays silent and the trace string is never built.
- `wish_server_set_verbose()` / binding `set_verbose(bool)` are deprecated: `true` now maps to log level `trace`, `false` to `none`.
- Server render loop: reduced per-frame CPU and heap-allocation churn — zero-copy string field access on the render/measure/arrange hot path, `ui_element::for_each_child_ordered()` no longer allocates per call, cached MenuBar-child detection, and fewer per-frame map/style copies in the render loop. No behavior change.
- Read-only RMI requests (`get` / `describe` / `dictionary` / `help`) no longer schedule redraw frames — a client polling widget values with `get` no longer holds a session at full framerate. Mutating requests (`set`, `call`, `instantiate`, …) are unaffected.

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/wish/releases/tag/v1.0.0