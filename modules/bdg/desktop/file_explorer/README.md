# file_explorer

Two-panel file browser: the local machine (left, client-driven) next to the
session sandbox (right, server-driven), with upload/download transfer
buttons and an "Open in Explorer" shortcut for the sandbox side.

- **server/**: `FileExplorer` form (`register_file_explorer()`), a
  `bdg::wish::form` subclass owning the window/panels/tables and all
  sandbox navigation/listing (`std::filesystem` + `file_service::resolve_path()`
  against `context::resource_dir`). Emits `on_local_navigate`,
  `on_upload_requested`, `on_download_requested`, and `closed` for the client
  to react to.
- **client/**: `run_file_explorer(wish_app_host&)`, self-registered as the
  `"file_explorer"` embedded app — instantiates the form, enumerates the
  local filesystem in response to `on_local_navigate`, and moves bytes
  between the local machine and the sandbox in response to
  `on_upload_requested`/`on_download_requested`.
- **resources/**: none.
