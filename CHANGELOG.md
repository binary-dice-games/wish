# Changelog

All notable user-facing changes to wish are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `wish_server_set_log_level()` (C ABI) and `set_log_level()` / `SetLogLevel()` (C++, Python, C# bindings): set the server log verbosity by name.
- Rust (`bindings/rust/`) and Go (`bindings/go/`) client language bindings for the wish client C ABI.
- Rust server language binding (`bindings/rust/wish-server/`): host and render a real wish session from Rust over `wish_server_c.h` (`Server`, `Params`).

### Changed

- `--verbose` is now a verbosity level (`none` | `fatal` | `error` | `warning` | `info` | `trace`, default `none`) instead of an on/off flag. RMI trace lines and session lifecycle lines in `wish_logs/server.log` (and mirrored to stdout) are produced only at `info` and above; decoded call payloads (`args=`, `set` values, response bodies) only at `trace`. `fatal`/`error`/`warning` only raise the severity floor for client log messages. At `none` (the default) the server log stays silent and the trace string is never built.
- `wish_server_set_verbose()` / binding `set_verbose(bool)` are deprecated: `true` now maps to log level `trace`, `false` to `none`.
- Web renderer: a bare mouse hover over the canvas no longer forces a full re-render and frame broadcast every frame — mouse-move is debounced (4 px / 100 ms, matching the SDL3 renderer) server-side and coalesced to one message per animation frame in the browser client. Also removes redundant per-frame buffer copies when encoding and broadcasting each frame.
- Web renderer: a bare mouse hover over the canvas no longer forces a full re-render and frame broadcast every frame — mouse-move is debounced (4 px / 100 ms, matching the SDL3 renderer) server-side and coalesced to one message per animation frame in the browser client. Also removes redundant per-frame buffer copies when encoding and broadcasting each frame.

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/wish/releases/tag/v1.0.0