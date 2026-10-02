// MIT License © 2026 Binary Dice Games
/// @file process_hooks.hpp
/// @brief Optional live-output / tick callbacks shared by every bdg/dev
///        module's `run_<tool>_cli()` process helper.
///
/// Each dev module keeps its own small libuv-based "run this argv, capture
/// its output" helper (see kubectl_process.hpp for why). They all accept
/// the same optional hooks, defined once here, so one command_worker (see
/// command_worker.hpp) can drive any of them. Header-only, standard library
/// only -- usable from the modules' process helpers and their unit tests.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bdg::wish::dev {

/// @brief Optional callbacks for a long-running command. Both run on the
/// calling thread, from inside the `run_<tool>_cli()` call.
struct run_hooks {
  /// Called with each chunk of stdout / stderr as it arrives (chunks are not
  /// line-aligned).
  std::function<void(const std::string& chunk)> on_output;
  /// Called every `tick_ms` while the process runs. Returning false asks the
  /// process to stop (SIGTERM); the run call still waits for it to exit.
  std::function<bool()> on_tick;
  uint64_t tick_ms{150};
};

} // namespace bdg::wish::dev
