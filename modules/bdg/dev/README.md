# bdg/dev

wish's own bundled developer-tool modules — GUI frontends for developer
CLIs and workflows, in the same shape as the [bdg/desktop](../desktop/README.md)
collection and doubling as reference implementations for the
[module system](../../README.md). All off by default; enable the whole
collection with `-DWISH_COLLECTION_BDG_DEV=ON`, or individual modules with
their own `WISH_MODULE_BDG_DEV_<NAME>` option (see
[docs/building.md](../../../docs/building.md)).

| Module | Description |
|--------|-------------|
| [editor](editor/README.md) | Live JSON/YAML UI mock editor: a syntax-highlighted source panel next to a continuously re-parsed preview, plus an event log and schema-aware autocomplete. The tool the `wish-module` / `wish-ui` skills use to preview a UI. |
| [docker](docker/README.md) | Docker Desktop-style GUI for the local `docker` CLI: dockable windows for containers/images/volumes/networks with per-row lifecycle actions, plus logs and inspect. Server owns the UI, client shells out to the `docker` binary. |
