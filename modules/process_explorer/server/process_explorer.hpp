// MIT License © 2025 Binary Dice Games
/// @file process_explorer.hpp
/// @brief Server-side ProcessExplorer form (top/htop-style system monitor).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Read-only, top/htop-style system monitor form.
///
/// Shows CPU and memory history graphs (via `plot_elements`), one meter per
/// logical CPU core, and a process table the user can sort by clicking any
/// column header (PID, Name, State, CPU %, Memory, or Command; defaults to
/// CPU % descending). All process/CPU/memory *sampling* happens client-side
/// (gathering this information is inherently OS-specific, and the machine a
/// user actually wants visibility into is their own, not necessarily the
/// wish server's host) -- the client periodically calls `update_snapshot`
/// with the latest data; this form only renders whatever it was last given.
/// See `app/wish_cli/client/apps/process_explorer/` for the reference
/// client that owns the sampling loop, exactly as `Notepad`'s reference
/// client owns `upload_file`/`download_file` while the server only manages
/// tabs.
///
/// Emitted events:
///   - `"closed"` — user clicked the window X button; internal UI is removed.
class process_explorer : public form {
 public:
  explicit process_explorer(bison::dynamic&& base);

  /// @brief RMI method: replace the currently-displayed system/process
  /// snapshot. @p args holds:
  ///   - `cpu_percent` (float) — overall CPU usage, 0..100.
  ///   - `per_core_percent` (vector<float>) — one entry per logical core.
  ///   - `mem_total_bytes`, `mem_used_bytes` (float).
  ///   - `processes` (dynamic array) — each entry a dynamic with `pid`
  ///     (int32), `name`, `command`, `state` (single-character string),
  ///     `cpu_percent` (float), `mem_rss_bytes` (float).
  /// Rows are reconciled by `pid` (added/updated/removed in place) and kept
  /// sorted by whichever column the user last clicked (see `on_event`'s
  /// `"sorted"` handling below; defaults to `cpu_percent` descending). The
  /// per-core meter row is sized once, from the first call's
  /// `per_core_percent` length.
  bison::dynamic do_update_snapshot(const bison::dynamic& args);

 protected:
  void on_init() override;
  /// @brief Reacts to `"closed"` (window X button) and `"sorted"` (a
  /// `proc_table_` column header was clicked -- see `Table`'s docs in
  /// `src/ui/ui_elements/table.cpp`). The latter updates the active sort
  /// column/direction and immediately re-sorts the existing rows, without
  /// waiting for the client's next `update_snapshot` call.
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct row_entry {
    ui_element_ptr row;
    ui_element_ptr pid_label;
    ui_element_ptr name_label;
    ui_element_ptr state_label;
    ui_element_ptr cpu_bar;
    ui_element_ptr mem_label;
    ui_element_ptr command_label;
    size_t child_key{0};
    std::string name;
    std::string state;
    std::string command;
    float cpu_percent{0.0f};
    float mem_rss_bytes{0.0f};
  };

  void ensure_core_meters(size_t core_count);
  void update_process_table(const bison::dynamic& args);
  static void push_history(std::vector<float>& history, float value);
  void update_history_xs(size_t count);
  /// @brief Re-sort `pid_to_row_` by `sort_column_id_`/`sort_ascending_` and
  /// refresh each row's `order` field -- no new data needed, so this can run
  /// directly from `on_event` for instant feedback on a header click.
  void resort_rows();

  bison::key_t window_id_;
  bison::key_t proc_table_id_;

  /// `column_id` (see `TableColumn.column_id`) of the column rows are
  /// currently sorted by; defaults to the CPU % column, matching the
  /// `ImGuiTableColumnFlags_DefaultSort` set on it in `on_init()`.
  int32_t sort_column_id_{3};
  bool sort_ascending_{false};

  ui_element_ptr cpu_summary_label_;
  ui_element_ptr mem_summary_label_;
  ui_element_ptr cores_container_;
  std::vector<ui_element_ptr> core_bars_;
  ui_element_ptr cpu_plot_series_;
  ui_element_ptr mem_plot_series_;
  ui_element_ptr proc_table_;

  std::vector<float> cpu_history_;
  std::vector<float> mem_history_;
  std::vector<float> history_xs_; ///< Shared index axis (0, 1, 2, ...) for both plots.

  std::unordered_map<int, row_entry> pid_to_row_;
  size_t next_child_key_{0};
};

/// @brief Register ProcessExplorer in the "wish" bison namespace.
void register_process_explorer();

} // namespace bdg::wish
