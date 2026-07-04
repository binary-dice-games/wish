// MIT License © 2025 Binary Dice Games
/// @file process_info.hpp
/// @brief Platform-agnostic process/CPU/memory sampling interface.
///
/// Gathering this information is inherently OS-specific, so this header
/// declares only plain data and a pimpl'd sampler class; the implementation
/// lives in a platform-suffixed source file (`process_info_linux.cpp` for
/// Linux/MSYS2 today; a future `process_info_windows.cpp` would provide a
/// native-Windows implementation behind the same interface).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief One sampled process at a point in time.
struct process_sample {
  int pid{0};
  std::string name; ///< Short process name (e.g. from `/proc/<pid>/comm`).
  std::string command; ///< Full command line, space-joined; falls back to `"[name]"` when unavailable.
  char state{'?'}; ///< OS-reported run state (e.g. 'R', 'S', 'D', 'Z', 'T').
  double cpu_percent{0.0}; ///< 0..100*num_cores; delta since the previous sample() call.
  uint64_t mem_rss_bytes{0}; ///< Resident set size.
};

/// @brief System-wide CPU and memory usage at a point in time.
struct system_sample {
  double cpu_percent{0.0}; ///< Overall CPU usage, 0..100.
  std::vector<double> per_core_percent; ///< One entry per logical CPU.
  uint64_t mem_total_bytes{0};
  uint64_t mem_used_bytes{0};
};

/// @brief One full sample of system and per-process state.
struct system_snapshot {
  system_sample system;
  std::vector<process_sample> processes;
};

/// @brief Samples system and per-process CPU/memory usage.
///
/// CPU percentages are computed from the delta between two consecutive
/// sample() calls, so the sampler holds internal state (previous jiffie
/// counters). The first sample() call after construction reports 0 for
/// every CPU percentage field.
///
/// Not thread-safe: callers must serialize their own access. In practice
/// each `process_explorer` form owns exactly one instance and calls
/// sample() only from its single background refresh thread.
class process_info_source {
 public:
  process_info_source();
  ~process_info_source();

  process_info_source(const process_info_source&) = delete;
  process_info_source& operator=(const process_info_source&) = delete;

  /// @brief Take a new sample, computing deltas against the previous call.
  system_snapshot sample();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::wish
