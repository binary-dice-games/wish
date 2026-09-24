// MIT License © 2026 Binary Dice Games
/// @file sq_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// Same shape as `modules/bdg/dev/docker/client/docker_process.hpp` (see that
/// file for the rationale: libuv is already linked into every module-client
/// target, and `bdg::bison::term::terminal` is unsuitable for many quick
/// argv-array invocations), plus one addition needed by `sq add -p`: an
/// optional text fed to the child's stdin, so a database password never
/// appears in the argv (visible to every local user via `ps`).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::sq {

/// @brief Result of one `run_sq_cli` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Runs `<binary> <args>` (no shell involved -- `args` is a real argv
/// array, so SQL text / connection strings with spaces, quotes or shell
/// metacharacters need no escaping), blocking until it exits.
///
/// @param args        Arguments after the program name.
/// @param binary      Program to exec. Defaults to `"sq"`; production code
///                    never passes anything else. Exists so
///                    `tests/test_sq_process.cpp` can exercise the argv /
///                    pipe / exit-code plumbing with a guaranteed-present
///                    stub (`printf`, `false`, `cat`) on a machine without
///                    `sq` installed.
/// @param stdin_text  When non-empty, written to the child's stdin, which is
///                    then closed. When empty, stdin is not connected.
process_result run_sq_cli(
    const std::vector<std::string>& args, const std::string& binary = "sq", const std::string& stdin_text = {});

} // namespace bdg::wish::sq
