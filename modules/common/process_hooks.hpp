// MIT License © 2026 Binary Dice Games
/// @file process_hooks.hpp
/// @brief Optional live-output / tick callbacks for the shared
///        run_process() helper (see process.hpp).
///
/// Separate from process.hpp so that command_worker.hpp, which drives a
/// command through these hooks, needs no libuv include. Header-only,
/// standard library only.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bdg::wish::common {

/// @brief Optional callbacks for a long-running command. Both run on the
/// calling thread, from inside the run_process() call.
struct run_hooks {
  /// Called with each chunk of stdout / stderr as it arrives (chunks are not
  /// line-aligned).
  std::function<void(const std::string& chunk)> on_output;
  /// Called every `tick_ms` while the process runs. Returning false asks the
  /// process to stop (SIGTERM); the run call still waits for it to exit.
  std::function<bool()> on_tick;
  uint64_t tick_ms{150};
};

} // namespace bdg::wish::common
