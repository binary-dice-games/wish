# process_explorer

top/htop-style system monitor: CPU and memory history graphs, one meter per
logical core, and a sortable process table. All sampling is client-side
(inherently OS-specific, and the machine a user wants visibility into is
their own) — the server-side form only renders whatever snapshot it was
last given.

- **server/**: `ProcessExplorer` form (`register_process_explorer()`) —
  read-only rendering of whatever snapshot its `update_snapshot` RMI method
  was last called with.
- **client/**: `run_process_explorer(wish_app_host&)`, self-registered as
  the `"process_explorer"` embedded app — owns process/CPU/memory sampling
  (platform-specific: `process_info_linux.cpp`/`process_info_win.cpp`) and
  periodically pushes a fresh snapshot to the form.
- **resources/**: none.
