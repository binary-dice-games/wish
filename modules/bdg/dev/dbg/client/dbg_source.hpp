// MIT License © 2026 Binary Dice Games
/// @file dbg_source.hpp
/// @brief Client-side debugger orchestration for the dbg module.
///
/// Owns the RMI proxy to the server-side DebuggerFrontend form and a
/// debug_backend implementation. Reacts to the form's `*_requested` events
/// (see server/dbg.hpp) by calling the corresponding debug_backend method,
/// and reacts to debug_backend::on_stop() by pushing fresh update_* RMI
/// snapshots. Mirrors docker_source's client/server split: the server never
/// touches the debuggee process directly.
#pragma once

#include "debug_backend.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::dbg {

class dbg_source {
 public:
  dbg_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::unique_ptr<debug_backend> backend);

  // ── *_requested event reactions ─────────────────────────────────────────
  void on_attach_requested(uint32_t pid);
  void on_detach_requested();
  void on_pause_requested();
  void on_resume_requested();
  void on_step_requested(const std::string& kind, uint32_t thread_id);
  void on_toggle_breakpoint_requested(const std::string& path, int32_t line);
  void on_select_thread_requested(uint32_t thread_id);
  void on_select_frame_requested(uint32_t frame_id);
  void on_add_watch_requested(const std::string& expr);

 private:
  /// @brief Pushes a fresh Threads snapshot (update_threads RMI call).
  void push_threads();
  /// @brief Pushes a fresh Call Stack snapshot for @p thread_id
  /// (update_callstack RMI call).
  void push_callstack(uint32_t thread_id);
  /// @brief Re-evaluates and pushes the Watch table for @p frame_id
  /// (update_watch RMI call).
  void push_watch(uint32_t frame_id);
  /// @brief Pushes the full Breakpoints table (update_breakpoints RMI call).
  void push_breakpoints();

  /// @brief debug_backend::on_stop callback: pushes threads + callstack for
  /// the stopped thread, sets run state to "paused", and appends an Output
  /// row describing the stop.
  void handle_stop(const stop_event& ev);

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  std::unique_ptr<debug_backend> backend_;

  bool attached_{false};
  uint32_t selected_thread_id_{0};
  bool has_selected_thread_{false};

  // Tracked so on_toggle_breakpoint_requested can flip enabled state and
  // push_breakpoints() can rebuild the full table (full-rebuild pattern,
  // same as server-side rebuild_* methods -- no incremental diffing).
  struct breakpoint_entry {
    std::string file;
    int32_t line{};
    bool enabled{};
  };
  std::vector<breakpoint_entry> breakpoints_;

  std::vector<std::string> watch_exprs_;
};

} // namespace bdg::wish::dbg
