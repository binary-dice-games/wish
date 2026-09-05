// MIT License © 2026 Binary Dice Games
/// @file win32_debug_backend.hpp
/// @brief Windows implementation of debug_backend (PLAN.md Step 4).
///
/// Owns the whole client-side debug-API surface: DebugActiveProcess /
/// WaitForDebugEvent / ContinueDebugEvent on a dedicated debug thread (only
/// the thread that attached to a process may wait for its debug events --
/// a hard Win32 constraint, see DESIGN.md §7), DbgHelp symbol/line
/// resolution, and INT3 software breakpoints. See DESIGN.md §3 for the
/// full design rationale.
///
/// Windows-only: the whole file compiles to nothing on other platforms, so
/// enabling WISH_MODULE_BDG_DEV_DBG on Linux/macOS still builds (the client
/// module simply has no working attach path there yet -- see PLAN.md "Not
/// implemented").
#pragma once

#if defined(_WIN32)

#include "debug_backend.hpp"

#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

namespace bdg::wish::dbg {

/// @brief Windows debug_backend: Win32 debug API + DbgHelp.
///
/// One instance attaches to at most one process at a time. `attach()`
/// spawns the dedicated debug thread, which owns DebugActiveProcess,
/// WaitForDebugEvent/ContinueDebugEvent, and (per DESIGN.md's Attach flow)
/// blocks between reporting a stop via on_stop() and being told to
/// continue by resume()/step_into()/step_over()/step_out(), called from
/// whatever thread the caller (dbg_source, on the RMI dispatch thread)
/// runs on.
class win32_debug_backend : public debug_backend {
 public:
  win32_debug_backend() = default;
  ~win32_debug_backend() override;

  bool attach(uint32_t pid) override;
  void detach() override;
  void pause() override;
  void resume() override;
  void step_into(uint32_t thread_id) override;
  void step_over(uint32_t thread_id) override;
  void step_out(uint32_t thread_id) override;

  bool set_breakpoint(const std::string& file, int line) override;
  void clear_breakpoint(const std::string& file, int line) override;

  std::vector<thread_info> get_threads() override;
  std::vector<frame_info> get_callstack(uint32_t thread_id) override;
  std::vector<watch_entry> evaluate(uint32_t frame_id, const std::vector<std::string>& exprs) override;

  void on_stop(stop_callback cb) override;

 private:
  struct breakpoint_state {
    std::string file;
    int line{};
    DWORD64 address{};
    BYTE original_byte{};
    bool patched{};
  };

  struct pending_step {
    enum class kind { none, into, over, out } kind{kind::none};
    DWORD64 target_address{}; ///< For over/out: address of the temp breakpoint.
    bool temp_bp_patched{};
    BYTE temp_bp_original_byte{};
  };

  // ── Debug-thread body and event handlers ──────────────────────────────
  void debug_thread_main(uint32_t pid);
  void handle_create_process(const DEBUG_EVENT& ev);
  void handle_load_dll(const DEBUG_EVENT& ev);
  void handle_exit_thread(const DEBUG_EVENT& ev);
  DWORD handle_exception(const DEBUG_EVENT& ev, bool& should_stop, stop_event& out);

  /// @brief Resolves an instruction pointer to {function, file, line} via
  ///        DbgHelp. Returns false (leaving `out` partially filled with
  ///        just the address-derived function name, if any) when no line
  ///        info is available -- e.g. CRT startup code with no PDB lines.
  bool resolve_address(DWORD64 address, std::string& function, std::string& file, int32_t& line);

  /// @brief Blocks the debug thread until resume()/step_*()/detach() wakes
  ///        it, then applies whatever continue-status/context change was
  ///        requested (setting the trap flag for step_into, or patching a
  ///        temporary return-address breakpoint for step_over/step_out).
  void wait_for_continue_command();

  /// @brief Minimal length decode of the x86-64 CALL forms MSVC emits
  ///        (E8 rel32; FF /2 register or memory indirect) so step_over can
  ///        place a temporary breakpoint at the return address instead of
  ///        single-stepping into the callee. Returns 0 if the instruction
  ///        at `address` isn't a recognized CALL form -- callers fall back
  ///        to single-stepping (still correct, just doesn't skip the call).
  int decode_call_length(DWORD64 address);

  void restore_all_breakpoints(); ///< Un-patches every INT3 byte (detach()).
  HANDLE thread_handle(uint32_t thread_id);

  HANDLE process_{nullptr};
  DWORD pid_{0};
  std::thread debug_thread_;
  std::atomic<bool> attached_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> pause_requested_{false};

  // Set when a just-hit breakpoint's original byte was restored and a
  // single-step was forced past it (see handle_exception's EXCEPTION_BREAKPOINT
  // case) -- the following EXCEPTION_SINGLE_STEP must re-patch the INT3 at
  // `repatch_addr_` before continuing, rather than being treated as a
  // step_into completion.
  bool awaiting_repatch_after_bp_{false};
  DWORD64 repatch_addr_{0};

  // Handoff between the debug thread (waits here after reporting a stop)
  // and pause()/resume()/step_*() (called from an arbitrary external
  // thread, per dbg_source's ownership). Plain mutex/condition_variable is
  // the lowest-level primitive case CLAUDE.md carves out for
  // synchronized<T> -- this is exactly that: a two-state handshake, not
  // general shared data.
  std::mutex cv_mtx_;
  std::condition_variable cv_;
  bool resume_signaled_{false};
  DWORD last_event_pid_{0};
  DWORD last_event_tid_{0};
  DWORD continue_status_{DBG_CONTINUE};
  pending_step pending_step_;

  bison::synchronized<std::map<uint32_t, HANDLE>> thread_handles_;
  bison::synchronized<std::vector<breakpoint_state>> breakpoints_;
  bison::synchronized<stop_callback> on_stop_cb_;

  DWORD64 module_base_{0};
  bool sym_initialized_{false};
};

} // namespace bdg::wish::dbg

#endif // defined(_WIN32)
