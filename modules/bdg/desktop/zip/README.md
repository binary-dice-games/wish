# zip

A zip/unzip tool for the client's local filesystem: browse a directory,
compress one or more selected files/folders into a `.zip`, extract a
selected `.zip` into a folder, and view an archive's contents (name, size,
compressed size, ratio) without extracting it. The file table fills the
window (mc/top-style stretch layout) and supports mc-style multi-row
selection (plain click, Ctrl+click, Shift+click/drag); a progress bar at
the bottom of the window tracks the current compress/extract operation
while the status label names the file currently being processed.

- **server/**: `Zip` form (`register_zip()`), a `bdg::wish::form`
  subclass owning the window/browser/table and all selection and
  compress/extract/view-contents *UI* logic. It has no filesystem access of
  its own — every file it browses lives on the client's machine — so it
  emits `on_navigate`, `on_compress_requested`, `on_extract_requested`, and
  `on_view_contents_requested` for the client to act on, and `closed` when
  the window is dismissed. The "already exists?" overwrite check is
  answered from the last listing the client reported, not a filesystem
  probe.
- **client/**: `run_zip(wish_app_host&)`, self-registered as the
  `"zip"` embedded app — instantiates the form, enumerates the local
  filesystem in response to `on_navigate`, and does the actual zip I/O
  (via miniz, the same library wish_server uses to unpack its own embedded
  resources) in response to `on_compress_requested`/`on_extract_requested`/
  `on_view_contents_requested`.
- **resources/**: none.

Mirrors `tree`'s client/server split for its local (left) panel:
the server owns and renders the UI, the client owns the local filesystem.
