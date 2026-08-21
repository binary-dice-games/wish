// MIT License © 2025 Binary Dice Games
/// @file process_control.hpp
/// @brief Platform-agnostic process-management actions (kill/pause/resume/
/// priority/affinity) and on-demand extended process info.
///
/// Companion to process_info.hpp: that header is read-only periodic
/// sampling (all processes, every tick); this one is one-shot,
/// side-effecting actions and on-demand detail lookups for a single pid,
/// both driven by context-menu clicks relayed from the server-side
/// Top form (see server/top.cpp's
/// "on_process_action_requested"/"on_process_details_requested" events and
/// this module's client/top.cpp, which subscribes to them).
/// Implementations live in platform-suffixed source files
/// (process_control_linux.cpp for Linux/MSYS2, a future
/// process_control_win.cpp for native Windows), same split as
/// process_info.hpp/process_info_linux.cpp/process_info_win.cpp.
#pragma once

#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Result of a one-shot process-control action.
struct process_action_result {
  bool success{true};
  std::string error; ///< Human-readable failure reason; empty when success is true.
};

/// @brief On-demand extended info for one process (the "Properties" dialog).
struct process_details {
  bool found{false}; ///< False if `pid` no longer existed by the time this was gathered.
  int pid{0};
  int ppid{0};
  std::string user; ///< Owning user name; empty if it could not be resolved.
  int thread_count{0};
  std::string start_time; ///< Human-readable local start time; empty if unavailable.
  std::string exe_path; ///< Full path to the executable; empty if unavailable.
  std::string cwd; ///< Working directory; empty if unavailable (always empty on Windows).
  std::string cmdline;
  int nice{0}; ///< Linux nice-value scale (-20 highest .. 19 lowest) on every platform.
  std::vector<int> affinity_cores; ///< Logical core indices this process may run on.
  std::string error; ///< Set (with `found: false`) instead of the fields above on failure.
};

/// @brief Terminates `pid` unconditionally (SIGKILL, or Win32 TerminateProcess).
process_action_result kill_process(int pid);

/// @brief Suspends all execution of `pid` (SIGSTOP, or SuspendThread on every thread).
process_action_result pause_process(int pid);

/// @brief Resumes a `pid` previously suspended by pause_process() (SIGCONT,
/// or ResumeThread on every thread).
process_action_result resume_process(int pid);

/// @brief Sets `pid`'s scheduling priority. `nice_value` uses the Linux
/// nice-value scale (-20 highest .. 19 lowest) on every platform; the
/// Windows implementation buckets it into the nearest of the 6 Win32
/// priority classes.
process_action_result set_process_priority(int pid, int nice_value);

/// @brief Restricts `pid` to run only on the given logical `cores` (0-based
/// indices). @p cores must not be empty -- callers must validate that
/// before calling, since an empty mask would leave the process unable to
/// run on any core.
process_action_result set_process_affinity(int pid, const std::vector<int>& cores);

/// @brief Fetches extended, on-demand info for `pid` (the "Properties" dialog).
process_details get_process_details(int pid);

} // namespace bdg::wish
