// MIT License © 2026 Binary Dice Games
/// @file pip_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// A near-verbatim copy of `modules/bdg/dev/helm/client/helm_process.hpp`
/// (itself a copy of the `kubectl` / `docker` / `git` modules' helpers -- see
/// kubectl_process.hpp's header comment for the full rationale). libuv is
/// already vendored by bison and already linked into every module-client
/// target by `wish_finalize_app_modules()` (`cmake/WishModules.cmake`), so
/// this file needs no CMake change.
///
/// The one difference: pip is run as `<python> -m pip`, not as a bare `pip`
/// binary, so the program is a *launcher* (an argv prefix) rather than a
/// single binary name.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish::pip {

/// @brief Result of one `run_pip_cli` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Optional callbacks for a long-running command. Both run on the
/// calling thread, from inside run_pip_cli().
struct run_hooks {
  /// Called with each chunk of stdout / stderr as it arrives (chunks are not
  /// line-aligned).
  std::function<void(const std::string& chunk)> on_output;
  /// Called every `tick_ms` while the process runs. Returning false asks the
  /// process to stop (SIGTERM); run_pip_cli() still waits for it to exit.
  std::function<bool()> on_tick;
  uint64_t tick_ms{150};
};

/// @brief Runs `<launcher...> <args...>` (no shell involved -- both are real
/// argv arrays, so package specs / paths with spaces or shell metacharacters
/// need no escaping), blocking until it exits. stdin is closed, so a command
/// that would prompt fails instead of hanging.
///
/// @param args      Arguments after the launcher, e.g.
///                  `{"list", "--format=json"}`.
/// @param launcher  The program and its leading arguments, e.g.
///                  `{"python3", "-m", "pip"}`; `launcher[0]` is PATH-searched
///                  when it has no path separator. `tests/test_pip_process.cpp`
///                  passes a guaranteed-present stub (`{"printf"}`, `{"false"}`)
///                  to exercise the argv / pipe / exit-code plumbing.
/// @param hooks     Live output / periodic tick callbacks, or null for none.
/// @return exit_code -1 (with stderr_text set) when @p launcher is empty or
///         the process cannot be spawned.
process_result run_pip_cli(
    const std::vector<std::string>& args, const std::vector<std::string>& launcher, const run_hooks* hooks = nullptr);

} // namespace bdg::wish::pip
