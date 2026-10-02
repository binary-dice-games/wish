// MIT License © 2026 Binary Dice Games
/// @file pkg_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// A near-verbatim copy of `modules/bdg/dev/pip/client/pip_process.hpp` (see
/// kubectl_process.hpp's header comment for why each dev module carries its
/// own). The command is one whole argv: the program differs per package
/// manager, and a privileged command is wrapped in `sudo` / `pkexec`.
#pragma once

#include "modules/bdg/dev/common/process_hooks.hpp"

#include <string>
#include <vector>

namespace bdg::wish::pkg {

/// @brief Result of one `run_pkg_cli` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Runs @p command (no shell involved -- it is a real argv array, so
/// package names / search text with spaces or shell metacharacters need no
/// escaping), blocking until it exits. stdin is closed, so a command that
/// would prompt (a password, a yes/no question) fails instead of hanging.
///
/// @param command  The program (PATH-searched when it has no path separator)
///                 followed by its arguments.
/// @param hooks    Optional live-output / tick callbacks (see
///                 common/process_hooks.hpp); a tick returning false stops
///                 the process. Null for none.
/// @return exit_code -1 (with stderr_text set) when @p command is empty or
///         the process cannot be spawned.
process_result run_pkg_cli(const std::vector<std::string>& command, const dev::run_hooks* hooks = nullptr);

} // namespace bdg::wish::pkg
