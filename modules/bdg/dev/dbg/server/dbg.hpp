// MIT License © 2026 Binary Dice Games
/// @file dbg.hpp
/// @brief Server-side DebuggerFrontend form -- Visual-Studio-style debugger
///        GUI over a client-side debug_backend.
///
/// All process-attach / symbol-resolution / breakpoint-patching happens
/// client-side (see client/debug_backend.hpp, client/dbg_source.hpp) --
/// this form only renders whatever snapshot it was last given via its
/// update_* RMI methods, and emits high-level `*_requested` events the
/// client reacts to by calling the corresponding debug_backend method and
/// pushing a fresh snapshot. Mirrors docker/git's client/server split.
///
/// Owns six independently dockable Windows: Source (main root, toolbar +
/// per-open-file TextEditor tabs), Threads, Call Stack, Watch, Breakpoints,
/// Output -- registered by hand in on_init() exactly like git.cpp's
/// build_*_window()/docker.cpp's build_list_window() do.
///
/// See DESIGN.md sec 3 ("UI Layout") and sec 5 ("Public API Contract") for
/// the full, authoritative event/method shapes this class implements.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// Visual-Studio-style debugger GUI form.
///
/// Emitted events (see DESIGN.md sec 5):
/// - `"closed"` -- tears down all six subtrees.
/// - `"attach_requested"` -- `{ pid }`.
/// - `"detach_requested"`, `"pause_requested"`, `"resume_requested"` -- no
///   payload.
/// - `"step_requested"` -- `{ kind ("into"/"over"/"out"), thread_id }`.
/// - `"toggle_breakpoint_requested"` -- `{ path, line }`.
/// - `"select_thread_requested"` -- `{ thread_id }`.
/// - `"select_frame_requested"` -- `{ frame_id }`.
/// - `"add_watch_requested"` -- `{ expr }`.
/// - `"open_file_requested"` -- `{ path, line }`.
class debugger_frontend : public form {
 public:
  explicit debugger_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the Threads table. @p args.threads is a
  /// dynamic array, each `{ id (uint32), state ("running"/"suspended"),
  /// current_function (string) }`. Full rebuild.
  bison::dynamic do_update_threads(const bison::dynamic& args);

  /// @brief RMI method: replace the Call Stack table for one thread. @p args
  /// holds `{ thread_id, frames: [{ index, function, file, line }] }`.
  /// `thread_id` is echoed back and the call is a no-op if it no longer
  /// matches the Threads window's current selection (docker/git's
  /// staleness-guard pattern).
  bison::dynamic do_update_callstack(const bison::dynamic& args);

  /// @brief RMI method: replace the Watch table for one frame. @p args holds
  /// `{ frame_id, entries: [{ name, value, type }] }`. Same staleness guard
  /// on `frame_id` as do_update_callstack's `thread_id`.
  bison::dynamic do_update_watch(const bison::dynamic& args);

  /// @brief RMI method: replace the Breakpoints table. @p args.breakpoints
  /// is a dynamic array, each `{ file, line, enabled }`. Full rebuild.
  bison::dynamic do_update_breakpoints(const bison::dynamic& args);

  /// @brief RMI method: ensure a Source tab exists for @p args.path and set
  /// its `breakpoint_lines` (int array) / `current_line` (int, 0 = none)
  /// fields.
  bison::dynamic do_update_source(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Output window. @p args holds
  /// `{ text, level ("info"/"warn"/"error") }`. FIFO-capped at
  /// kMaxOutputRows.
  bison::dynamic do_append_output(const bison::dynamic& args);

  /// @brief RMI method: drive the Source toolbar's enablement. @p args holds
  /// `{ state ("detached"/"running"/"paused") }`.
  bison::dynamic do_set_run_state(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── Window construction (one per dockable root) ─────────────────────────
  void build_source_window();
  void build_threads_window();
  void build_callstack_window();
  void build_watch_window();
  void build_breakpoints_window();
  void build_output_window();

  // ── Rebuild helpers ──────────────────────────────────────────────────────
  void rebuild_threads(const bison::dynamic& args);
  void rebuild_callstack(const bison::dynamic& args);
  void rebuild_watch(const bison::dynamic& args);
  void rebuild_breakpoints(const bison::dynamic& args);

  /// @brief Tears down every window's internal objects and emits "closed".
  ///        Shared by all six windows' close handlers (DESIGN.md: "Closing
  ///        any window emits 'closed'; tears down all six subtrees.").
  void close_all();

  // ── Small element builders (git.cpp/docker.cpp's shared idiom) ─────────
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);

  // ── Root keys ────────────────────────────────────────────────────────────
  std::string threads_root_key_;
  std::string callstack_root_key_;
  std::string watch_root_key_;
  std::string breakpoints_root_key_;
  std::string output_root_key_;

  // Top-level window ids (for matching "closed" events in on_event()).
  bison::key_t source_window_id_;
  bison::key_t threads_window_id_;
  bison::key_t callstack_window_id_;
  bison::key_t watch_window_id_;
  bison::key_t breakpoints_window_id_;
  bison::key_t output_window_id_;

  // ── Source window widgets ────────────────────────────────────────────────
  ui_element_ptr pid_input_;
  std::string pid_text_;
  ui_element_ptr run_state_label_;

  // ── Threads window ───────────────────────────────────────────────────────
  ui_element_ptr threads_table_;
  bison::key_t threads_table_id_;
  std::vector<uint32_t> thread_row_ids_; // row index -> thread_id

  // ── Call Stack window ────────────────────────────────────────────────────
  ui_element_ptr callstack_table_;
  bison::key_t callstack_table_id_;
  std::vector<int32_t> frame_row_ids_; // row index -> frame index
  uint32_t selected_thread_id_{0};
  bool has_selected_thread_{false};

  // ── Watch window ─────────────────────────────────────────────────────────
  ui_element_ptr watch_table_;
  ui_element_ptr watch_expr_input_;
  std::string watch_expr_text_;
  uint32_t selected_frame_id_{0};
  bool has_selected_frame_{false};

  // ── Breakpoints window ───────────────────────────────────────────────────
  ui_element_ptr breakpoints_table_;

  // ── Output window ────────────────────────────────────────────────────────
  ui_element_ptr output_table_;
  static constexpr size_t kMaxOutputRows = 500;
  size_t next_output_child_key_{0};
  std::vector<bison::key_t> output_row_keys_; // oldest first, for FIFO eviction

  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
};

/// Register DebuggerFrontend in the "wish" bison namespace.
void register_dbg();

} // namespace bdg::wish
