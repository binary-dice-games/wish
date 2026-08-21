# mc

Two-panel file browser: the local machine (left, client-driven) next to the
session sandbox (right, server-driven), with upload/download transfer
buttons and an "Open in Explorer" shortcut for the sandbox side.

- **server/**: `Mc` form (`register_mc()`), a
  `bdg::wish::form` subclass owning the window/panels/tables and all
  sandbox navigation/listing (`std::filesystem` + `file_service::resolve_path()`
  against `context::resource_dir`). Emits `on_local_navigate`,
  `on_upload_requested`/`on_download_requested` (or, when the transfer
  target already exists, `on_upload_conflict`/`on_download_conflict`), and
  `closed` for the client to react to.
- **client/**: `run_mc(wish_app_host&)`, self-registered as the
  `"mc"` embedded app — instantiates the form, enumerates the
  local filesystem in response to `on_local_navigate`, and moves bytes
  between the local machine and the sandbox in response to
  `on_upload_requested`/`on_download_requested`. A conflict event instead
  instantiates the built-in `MessageBox` form ("yes_no" preset) to confirm
  with the user before overwriting.
- **resources/**: none.
