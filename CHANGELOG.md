# Changelog

All notable user-facing changes to wish are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Server RMI trace lines in `wish_logs/server.log` no longer include decoded call payloads (`args=`, `set` values, response bodies) unless `--verbose` is passed; envelope metadata (operation, session, object, method) is still logged. Keeps the log file compact by default.
- Web renderer: a bare mouse hover over the canvas no longer forces a full re-render and frame broadcast every frame — mouse-move is debounced (4 px / 100 ms, matching the SDL3 renderer) server-side and coalesced to one message per animation frame in the browser client. Also removes redundant per-frame buffer copies when encoding and broadcasting each frame.

## [1.0.0] - 2026-08-27

Initial release.

[1.0.0]: https://github.com/carloslopezmdez/wish/releases/tag/v1.0.0