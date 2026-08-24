// MIT License © 2025 Binary Dice Games
/// @file top.hpp
/// @brief Server-side form for top (a top/htop-style system monitor).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class message_box;
class properties_dialog;

/// @brief Top/htop-style system monitor form, with a per-row right-click
/// menu for managing individual processes.
///
/// Shows CPU and memory history graphs (via `plot_elements`), one meter per
/// logical CPU core, and a process table the user can sort by clicking any
/// column header (PID, Name, State, CPU %, Memory, or Command; defaults to
/// CPU % descending). All process/CPU/memory *sampling* happens client-side
/// (gathering this information is inherently OS-specific, and the machine a
/// user actually wants visibility into is their own, not necessarily the
/// wish server's host) -- the client periodically calls `update_snapshot`
/// with the latest data; this form only renders whatever it was last given.
/// See `app/wish_cli/client/apps/top/` for the reference
/// client that owns the sampling loop, exactly as `nano`'s reference
/// client owns `upload_file`/`download_file` while the server only manages
/// tabs.
///
/// Right-clicking a row opens a `ContextMenu` (see `src/ui/ui_elements/menu.cpp`
/// and `render_table()` in `imgui_ui_renderer.cpp`) with: Properties (opens
/// an extended-info dialog), Pause/Resume, Kill Process (behind a
/// confirmation dialog), a Priority submenu, and Set CPU Affinity (opens a
/// per-core checkbox dialog). Actual process-control work is inherently
/// OS-specific and happens on the *client's* machine, same rationale as
/// sampling: this form only builds the menu and, on a click, `emit()`s a
/// request event for the client to act on (see `on_process_action_requested`/
/// `on_process_details_requested` below) -- mirroring `tree`'s
/// `on_upload_requested`/`on_download_requested` pattern.
///
/// Emitted events:
///   - `"closed"` — user clicked the window X button; internal UI is removed.
///   - `"on_process_action_requested"` — a context-menu action was
///     confirmed; `{ pid: int32, action: string }` where `action` is one of
///     `"kill"`, `"pause"`, `"resume"`, `"set_priority"` (adds `nice: int32`,
///     the Linux nice-value scale on every platform), or `"set_affinity"`
///     (adds `cores: int32[]`, the 0-based logical core indices to allow).
///     The client is expected to call `report_action_result` afterward.
///   - `"on_process_details_requested"` — the "Properties..." item was
///     clicked; `{ pid: int32 }`. The client is expected to call
///     `report_process_details` with the result.
class top : public form {
 public:
  explicit top(bison::dynamic&& base);

  /// @brief RMI method: replace the currently-displayed system/process
  /// snapshot. @p args holds:
  ///   - `cpu_percent` (float) — overall CPU usage, 0..100.
  ///   - `per_core_percent` (vector<float>) — one entry per logical core.
  ///   - `mem_total_bytes`, `mem_used_bytes` (float).
  ///   - `processes` (dynamic array) — each entry a dynamic with `pid`
  ///     (int32), `name`, `command`, `state` (single-character string),
  ///     `cpu_percent` (float), `mem_rss_bytes` (float), `nice_value`
  ///     (int32, Linux nice-value scale), `affinity_cores` (int32[],
  ///     0-based logical core indices).
  /// Rows are reconciled by `pid` (added/updated/removed in place) and kept
  /// sorted by whichever column the user last clicked (see `on_event`'s
  /// `"sorted"` handling below; defaults to `cpu_percent` descending). The
  /// per-core meter row is sized once, from the first call's
  /// `per_core_percent` length.
  bison::dynamic do_update_snapshot(const bison::dynamic& args);

  /// @brief RMI method: reports the outcome of a previously-requested
  /// `on_process_action_requested` action. @p args holds `pid` (int32),
  /// `action` (string), `success` (bool), and `error` (string, only
  /// meaningful when `success` is false). Updates the status label at the
  /// bottom of the window.
  bison::dynamic do_report_action_result(const bison::dynamic& args);

  /// @brief RMI method: delivers the result of a previously-requested
  /// `on_process_details_requested` lookup. @p args holds `pid` (int32),
  /// `found` (bool), and -- when `found` is true -- `ppid` (int32), `user`,
  /// `thread_count` (int32), `start_time`, `exe_path`, `cwd`, `cmdline`,
  /// `nice` (int32), `affinity_cores` (int32[]); when `found` is false,
  /// `error` (string) instead. Ignored if the Properties dialog isn't
  /// currently open for this exact `pid` (it may have been closed, or
  /// reopened for a different process, before this response arrived).
  bison::dynamic do_report_process_details(const bison::dynamic& args);

 protected:
  void on_init() override;
  /// @brief Reacts to: `"closed"` (window X button); `"sorted"` (a
  /// `proc_table_` column header was clicked -- see `Table`'s docs in
  /// `src/ui/ui_elements/table.cpp`); a row context-menu item's `"clicked"`
  /// (looked up via `action_item_targets_`); and the set-affinity dialog's
  /// own button clicks (the confirm-kill/properties dialogs are privately
  /// instantiated MessageBox/PropertiesDialog forms -- see
  /// show_confirm_kill()/show_properties_dialog() -- and handle their own
  /// button clicks internally).
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// Which context-menu action a `MenuItem`'s `__wish_id` maps to (see
  /// `action_item_targets_`). `priority` additionally carries the nice
  /// value that specific submenu entry represents.
  enum class row_action_kind { kill, pause_or_resume, properties, affinity_dialog, priority };

  struct row_action_target {
    int pid{0};
    row_action_kind kind{row_action_kind::kill};
    int32_t nice{0}; ///< Only meaningful when kind == priority.
  };

  struct row_entry {
    ui_element_ptr row;
    ui_element_ptr pid_label;
    ui_element_ptr name_label;
    ui_element_ptr state_label;
    ui_element_ptr cpu_bar;
    ui_element_ptr mem_label;
    ui_element_ptr command_label;
    /// Label toggles "Pause"/"Resume" based on `state`; kept live so
    /// update_process_table() can refresh it without rebuilding the menu.
    ui_element_ptr pause_resume_item;
    /// One MenuItem per kPriorityLevels entry, in the same order; `checked`
    /// is refreshed from `nice` on every snapshot.
    std::vector<ui_element_ptr> priority_items;
    bison::key_t kill_id;
    bison::key_t properties_id;
    bison::key_t affinity_id;
    size_t child_key{0};
    std::string name;
    std::string state;
    std::string command;
    float cpu_percent{0.0f};
    float mem_rss_bytes{0.0f};
    int32_t nice{0};
    std::vector<int32_t> affinity_cores;
  };

  void ensure_core_meters(size_t core_count);
  void update_process_table(const bison::dynamic& args);
  static void push_history(std::vector<float>& history, float value);
  void update_history_xs(size_t count);
  /// @brief Re-sort `pid_to_row_` by `sort_column_id_`/`sort_ascending_` and
  /// refresh each row's `order` field -- no new data needed, so this can run
  /// directly from `on_event` for instant feedback on a header click.
  void resort_rows();

  /// @brief Builds the row's ContextMenu element (Properties/Pause-Resume/
  /// Kill/Priority submenu/Set CPU Affinity), registering every item's
  /// `__wish_id` in `action_item_targets_` and populating @p entry's
  /// `pause_resume_item`/`priority_items`/`kill_id`/`properties_id`/
  /// `affinity_id`. Returns the ContextMenu element, appended as the row's
  /// last child by the caller.
  ui_element_ptr build_row_context_menu(row_entry& entry, int pid, const std::string& state, int32_t nice);
  /// @brief Updates an existing row's ContextMenu state (pause/resume
  /// label, priority checkmarks) from freshly-sampled `state`/`nice` --
  /// called from update_process_table()'s existing-row branch.
  void update_row_context_menu(row_entry& entry, const std::string& state, int32_t nice);
  void set_status(const std::string& text);

  void show_confirm_kill(int pid);

  void show_affinity_dialog(int pid);
  void request_close_affinity();
  void remove_affinity_objects();

  void show_properties_dialog(int pid);

  /// @brief Builds a "ProcessDetails" dynamic (see register_top()) from the
  /// fields do_report_process_details() receives -- pid alone (everything
  /// else at its type default) for the placeholder shown immediately by
  /// show_properties_dialog(), or the full set once the client responds.
  static bison::dynamic_ptr make_process_details(
      int32_t pid, int32_t ppid, const std::string& user, int32_t thread_count, const std::string& start_time,
      const std::string& exe_path, const std::string& cwd, const std::string& cmdline, int32_t nice,
      const std::vector<int32_t>& affinity_cores);

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
  ui_element_ptr status_label_;

  std::vector<float> cpu_history_;
  std::vector<float> mem_history_;
  std::vector<float> history_xs_; ///< Shared index axis (0, 1, 2, ...) for both plots.

  std::unordered_map<int, row_entry> pid_to_row_;
  size_t next_child_key_{0};

  /// Maps a row context-menu item's `__wish_id` to the (pid, action) it
  /// represents; erased alongside the row when its process vanishes.
  std::unordered_map<bison::key_t, row_action_target, bison::key_t, bison::key_t> action_item_targets_;

  /// Confirm-kill dialog: a privately-instantiated MessageBox (see
  /// form::instantiate_child_form()) with a "yes_no" preset. Only one may
  /// be open at a time; a new kill request just overwrites this member --
  /// the stale instance's destructor tears down its own internal objects,
  /// same effect the old direct remove_objects_at() call had.
  std::shared_ptr<message_box> confirm_dialog_;

  /// Set CPU Affinity dialog: one Checkbox per logical core (built at
  /// show-time from the current core count and the row's current
  /// `affinity_cores`), plus Apply/Cancel. Checkbox state is read directly
  /// from each element's own `value` field at Apply time -- no separate
  /// "changed" handler needed, since render_checkbox() already writes it
  /// back on toggle.
  std::string affinity_root_key_;
  bison::key_t affinity_window_id_;
  bison::key_t affinity_apply_id_;
  bison::key_t affinity_cancel_id_;
  int affinity_dialog_pid_{0};
  std::vector<std::pair<ui_element_ptr, int32_t>> affinity_checkboxes_; ///< (checkbox, core index).

  /// Properties (extended info) dialog: a privately-instantiated
  /// PropertiesDialog (see form::instantiate_child_form()), opened
  /// immediately with a pid-only placeholder target; do_report_process_details()
  /// calls its set_target() again once the client responds, if it's still
  /// open for the same pid (see properties_dialog_pid_'s staleness check).
  std::shared_ptr<properties_dialog> properties_dialog_;
  int properties_dialog_pid_{0};
};

/// @brief Register Top in the "wish" bison namespace.
void register_top();

} // namespace bdg::wish
