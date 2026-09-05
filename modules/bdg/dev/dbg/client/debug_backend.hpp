// MIT License © 2026 Binary Dice Games
/// @file debug_backend.hpp
/// @brief Platform-agnostic debugger backend interface consumed by
///        client/dbg_source.hpp.
///
/// All process-attach / symbol-resolution / breakpoint-patching logic lives
/// behind this seam so the RMI contract (server/dbg.hpp) can be proven out
/// against a synthetic fake implementation (see tests/test_dbg.cpp) before
/// `win32_debug_backend` (Step 4 of PLAN.md) exists. Mirrors how
/// client/docker_process.hpp isolates docker's one platform dependency
/// (running the `docker` CLI) behind a narrow seam.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish::dbg {

/// @brief One row of the Threads window (see server/dbg.hpp's
///        do_update_threads doc comment for the matching RMI payload shape).
struct thread_info {
  uint32_t id{};
  std::string state; ///< "running" or "suspended".
  std::string current_function;
};

/// @brief One row of the Call Stack window for a given thread.
struct frame_info {
  int32_t index{};
  std::string function;
  std::string file;
  int32_t line{};
};

/// @brief One row of the Watch window, resolved for a given frame.
struct watch_entry {
  std::string name;
  std::string value;
  std::string type;
};

/// @brief A debuggee stop (breakpoint hit, step completed, exception) that
///        the backend reports asynchronously via debug_backend::on_stop().
struct stop_event {
  uint32_t thread_id{};
  std::string reason; ///< e.g. "breakpoint", "step", "exception".
  std::string file;
  int32_t line{};
};

/// @brief Seam between the dbg module's client-side orchestration
///        (dbg_source) and a real debugger implementation.
///
/// v1 (PLAN.md Step 4) implements this once, for Windows, as
/// `win32_debug_backend` (DebugActiveProcess/WaitForDebugEvent + DbgHelp).
/// Linux/macOS backends are additive future work (see dbg's DESIGN.md and
/// PLAN.md "Not implemented").
class debug_backend {
 public:
  virtual ~debug_backend() = default;

  virtual bool attach(uint32_t pid) = 0;
  virtual void detach() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void step_into(uint32_t thread_id) = 0;
  virtual void step_over(uint32_t thread_id) = 0;
  virtual void step_out(uint32_t thread_id) = 0;

  virtual bool set_breakpoint(const std::string& file, int line) = 0;
  virtual void clear_breakpoint(const std::string& file, int line) = 0;

  virtual std::vector<thread_info> get_threads() = 0;
  virtual std::vector<frame_info> get_callstack(uint32_t thread_id) = 0;
  virtual std::vector<watch_entry> evaluate(uint32_t frame_id, const std::vector<std::string>& exprs) = 0;

  using stop_callback = std::function<void(const stop_event&)>;
  /// @brief Registers the callback invoked whenever the debuggee stops.
  ///        Only one callback is supported; a later call replaces the
  ///        earlier one (dbg_source registers exactly one, at construction).
  virtual void on_stop(stop_callback cb) = 0;
};

} // namespace bdg::wish::dbg
