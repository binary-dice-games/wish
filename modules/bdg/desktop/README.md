# bdg/desktop

wish's own bundled desktop-tool modules — small, self-contained UI apps
that exercise the framework end-to-end and double as reference
implementations for the [module system](../../README.md). All off by
default; enable the whole collection with `-DWISH_COLLECTION_BDG_DESKTOP=ON`,
or individual modules with their own `WISH_MODULE_BDG_DESKTOP_<NAME>` option
(see [docs/building.md](../../../docs/building.md)).

| Module | Description |
|--------|-------------|
| [bc](bc/README.md) | Four-function calculator; demonstrates self-contained server-side form logic. |
| [tail](tail/README.md) | `tail`-like log viewer: colorized by severity, filterable by regex, with a dedicated tab per `[Tag]` token seen in the stream. |
| [nano](nano/README.md) | Multi-file, syntax-highlighted text editor. |
| [pix](pix/README.md) | Local image folder viewer: thumbnail grid + zoomable/pannable full preview, client-driven decode/resize/upload. |
| [top](top/README.md) | top/htop-style system monitor; client samples CPU/memory/processes, server only renders. |
| [mc](mc/README.md) | Two-panel file browser for the client's local filesystem vs. session sandbox; server owns the UI, client does the actual filesystem I/O. |
| [zip](zip/README.md) | Zip/unzip tool for the client's local filesystem; server owns the UI, client does the actual compress/extract/list-contents I/O. |
