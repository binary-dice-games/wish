// MIT License © 2025 Binary Dice Games
/// @file process_info.hpp
/// @brief Platform-agnostic process/CPU/memory/network/disk/port sampling
///        interface.
///
/// Gathering this information is inherently OS-specific, so this header
/// declares only plain data and a pimpl'd sampler class; the implementation
/// lives in a platform-suffixed source file (`process_info_linux.cpp` for
/// Linux/MSYS2 today; a future `process_info_windows.cpp` would provide a
/// native-Windows implementation behind the same interface).
///
/// This is client-side code: the client always monitors its own local
/// machine (the one the user is actually sitting at), not necessarily the
/// wish server's host -- see top.cpp in this directory, which
/// periodically pushes a sample into the server-side Top form
/// via its `update_snapshot` RMI method.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
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
  int nice{0}; ///< Scheduling priority, Linux nice-value scale (-20 highest .. 19 lowest).
  std::vector<int> affinity_cores; ///< Logical core indices this process is currently allowed to run on.
};

/// @brief System-wide CPU, memory, network and disk usage at a point in time.
///
/// Network counters cover every non-loopback interface; disk counters cover
/// physical disks only (not partitions or virtual block devices, which would
/// count the same bytes twice). Rates are deltas since the previous sample()
/// call divided by the elapsed wall-clock time, so the first call reports 0.
struct system_sample {
  double cpu_percent{0.0}; ///< Overall CPU usage, 0..100.
  std::vector<double> per_core_percent; ///< One entry per logical CPU.
  uint64_t mem_total_bytes{0};
  uint64_t mem_used_bytes{0};
  double net_rx_bytes_per_sec{0.0}; ///< Received, all non-loopback interfaces.
  double net_tx_bytes_per_sec{0.0}; ///< Sent, all non-loopback interfaces.
  uint64_t net_rx_total_bytes{0}; ///< Received since boot (or counter reset).
  uint64_t net_tx_total_bytes{0}; ///< Sent since boot (or counter reset).
  double disk_read_bytes_per_sec{0.0}; ///< Read, all physical disks.
  double disk_write_bytes_per_sec{0.0}; ///< Written, all physical disks.
  uint64_t disk_read_total_bytes{0}; ///< Read since boot.
  uint64_t disk_write_total_bytes{0}; ///< Written since boot.
};

/// @brief One listening socket: a TCP socket in the LISTEN state, or a UDP
/// socket bound to a local port with no fixed remote peer.
struct open_port {
  std::string protocol; ///< `"TCP"`, `"TCP6"`, `"UDP"` or `"UDP6"`.
  std::string address; ///< Local address in text form (`"0.0.0.0"`, `"::1"`, ...).
  int port{0}; ///< Local port number.
  int pid{0}; ///< Owning process, 0 when unknown (e.g. another user's socket).
  std::string process; ///< Owning process name; empty when unknown.
};

/// @brief One full sample of system, per-process and listening-port state.
struct system_snapshot {
  system_sample system;
  std::vector<process_sample> processes;
  /// Sorted by port, then protocol, then address.
  std::vector<open_port> ports;
};

/// @brief Per-second rate of a monotonically-increasing counter; 0 when
/// either the elapsed time is not positive or the counter went backwards
/// (wrapped, or an interface/disk disappeared between samples).
inline double counter_rate(uint64_t prev, uint64_t cur, double elapsed_seconds) {
  if (elapsed_seconds <= 0.0 || cur < prev)
    return 0.0;
  return static_cast<double>(cur - prev) / elapsed_seconds;
}

/// @brief Sorts @p ports by port, then protocol, then address (the order
/// system_snapshot::ports documents).
inline void sort_ports(std::vector<open_port>& ports) {
  std::sort(ports.begin(), ports.end(), [](const open_port& a, const open_port& b) {
    return std::tie(a.port, a.protocol, a.address) < std::tie(b.port, b.protocol, b.address);
  });
}

/// @brief Samples system and per-process CPU/memory usage, network and disk
/// throughput, and the list of listening ports.
///
/// CPU percentages are computed from the delta between two consecutive
/// sample() calls, so the sampler holds internal state (previous jiffie
/// counters, network/disk byte counters). The first sample() call after
/// construction reports 0 for every CPU percentage and every rate field.
///
/// Not thread-safe: callers must serialize their own access. In practice
/// each `run_top` session owns exactly one instance and calls
/// sample() only from its single background sampling thread.
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
