# zip

A zip/unzip tool for the client's local filesystem: browse a directory,
compress one or more selected files/folders into a `.zip`, extract a
selected `.zip` into a folder, and view an archive's contents (name, size,
compressed size, ratio) without extracting it. Laid out as three dockable
panels inside its own nested "Zip" dockspace (see
[docs/dock-layout.md](../../../../docs/dock-layout.md)): **Files** (path
bar and file table, with mc-style multi-row selection: plain click,
Ctrl+click, Shift+click/drag), **Contents** (the archive last viewed via
View Contents or a double-click), and **Actions** (Compress, Extract, View
Contents, Refresh and the status line). The first run seeds Actions along
the top with Files beside Contents below it; rearrange freely afterwards
(Shift+drag to re-dock). Closing any panel closes the tool. A compress or
extract that takes more than a moment opens the same modal progress dialog
mc and the dev tools use, with a bar of the entries processed and a Cancel
button; a failure stays in the dialog until closed.

- **server/**: `Zip` form (`register_zip()`), a `bdg::wish::form`
  subclass owning the three panels and all selection and
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
  `on_view_contents_requested`. Each of those runs as a job on a
  `common::command_worker` (`modules/bdg/common/command_worker.hpp`),
  which shows the progress dialog for long ones.
- **resources/**: none.

Mirrors `tree`'s client/server split for its local (left) panel:
the server owns and renders the UI, the client owns the local filesystem.
