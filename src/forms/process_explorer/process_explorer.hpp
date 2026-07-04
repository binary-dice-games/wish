// MIT License © 2025 Binary Dice Games
/// @file process_explorer.hpp
/// @brief Server-side ProcessExplorer form (top/htop-style system monitor).
#pragma once

#include <forms/form.hpp>
#include <ui_element.hpp>

#include "process_info.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Self-contained, read-only process/CPU/memory monitor form.
///
/// Shows live CPU and memory history graphs (via `plot_elements`), one
/// meter per logical CPU core, and a sortable-by-CPU% table of running
/// processes. All data acquisition and UI updates happen server-side on an
/// internal background thread; the client only needs to instantiate the
/// form and listen for the `"closed"` event, exactly like `calculator`.
///
/// Emitted events:
///   - `"closed"` — user clicked the window X button; internal UI is removed.
class process_explorer : public form {
 public:
  explicit process_explorer(bison::dynamic&& base);
  ~process_explorer() override;

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

  /// @brief One process's row in `proc_table`, plus its cached cell widgets.
  struct row_entry {
    ui_element_ptr row;
    ui_element_ptr pid_label;
    ui_element_ptr name_label;
    ui_element_ptr state_label;
    ui_element_ptr cpu_bar;
    ui_element_ptr mem_label;
    ui_element_ptr command_label;
    size_t child_key{0};
    double cpu_percent{0.0};
  };

  /// @brief Everything the background refresh thread needs, kept alive by a
  /// shared_ptr independent of the owning `process_explorer` object.
  ///
  /// The refresh thread is detached (not joined) when `~process_explorer()`
  /// runs while an RMI dispatch already holds this session's write lock
  /// (see the destructor) -- in that case the thread may briefly keep
  /// running after `this` is destroyed. Since the thread only ever touches
  /// `refresh_state` (via its own shared_ptr) and never `this`, that is
  /// safe; `ctx` remains valid for as long as the client connection is open,
  /// which outlives that brief window.
  struct refresh_state {
    bison::rmi::context* ctx{nullptr};
    process_info_source source;

    ui_element_ptr cpu_summary_label;
    ui_element_ptr mem_summary_label;
    ui_element_ptr cores_container;
    std::vector<ui_element_ptr> core_bars;
    ui_element_ptr cpu_plot_series;
    ui_element_ptr mem_plot_series;
    ui_element_ptr proc_table;

    std::vector<float> cpu_history;
    std::vector<float> mem_history;
    std::unordered_map<int, row_entry> pid_to_row;
    size_t next_child_key{0};
  };

  /// @brief Apply one system_snapshot to the internal UI tree.
  ///
  /// Pure UI-tree update, no I/O -- static (no `this`) so it is safe to call
  /// from the detached background thread, and independently testable with
  /// hand-built snapshots.
  static void apply_snapshot(refresh_state& state, const system_snapshot& snap);

  std::shared_ptr<refresh_state> state_;

 private:
  static void update_process_table(refresh_state& state, const std::vector<process_sample>& processes);
  static void push_history(std::vector<float>& history, float value);

  /// @brief Create one ProgressBar per logical core in the "cores" row.
  /// Runs once, synchronously, from on_init() -- core count never changes.
  void build_core_meters(size_t core_count);

  void start_refresh_thread();

  bison::key_t window_id_;

  std::thread refresh_thread_;
  std::shared_ptr<std::atomic<bool>> stop_{std::make_shared<std::atomic<bool>>(false)};
};

/// @brief Register ProcessExplorer in the "wish" bison namespace.
void register_process_explorer();

} // namespace bdg::wish
