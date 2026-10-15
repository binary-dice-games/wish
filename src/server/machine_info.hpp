// MIT License © 2026 Binary Dice Games
/// @file machine_info.hpp
/// @brief Facts about the machine and process a wish server runs on, for
///        the server overlay's information page.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace bdg::wish {

/// @brief A snapshot of the local machine and the current process.
struct machine_info {
  std::string hostname; ///< Empty when unknown.
  std::string os; ///< e.g. "Linux 6.8.0 (x86_64)" or "Windows".
  std::string user; ///< OS user running the process; empty when unknown.
  unsigned cpu_count{0}; ///< Logical CPUs; 0 when unknown.
  uint64_t total_memory{0}; ///< Bytes of physical memory; 0 when unknown.
  uint64_t available_memory{0}; ///< Bytes available; 0 when unknown.
  uint64_t process_memory{0}; ///< Resident set size of this process; 0 when unknown.
  uint64_t pid{0}; ///< This process' id.
  std::optional<double> load_average; ///< 1-minute load average (POSIX only).
};

/// @brief Reads the current machine_info. Never throws: facts it can't read
///        are left at their "unknown" value.
machine_info query_machine_info();

/// @brief A byte count as text with a binary unit: "512 B", "1.5 KB",
///        "3.2 GB".
std::string format_bytes(uint64_t bytes);

/// @brief A duration as compact text: "42s", "5m 03s", "2h 07m", "3d 04h".
std::string format_duration(std::chrono::seconds d);

} // namespace bdg::wish
