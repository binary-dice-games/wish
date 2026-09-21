// MIT License © 2026 Binary Dice Games
/// @file curl_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// A near-verbatim copy of `modules/bdg/dev/docker/client/docker_process.hpp`
/// (itself a copy of `modules/bdg/desktop/git/client/git_process.hpp` --
/// see that file's header comment for the full rationale). In short:
/// `bdg::bison::term::terminal` is a pty-attached, shell-string,
/// stdio-hijacking helper unsuitable for issuing many quick argv-array
/// `curl <args>` invocations from inside a long-running `wish_client`.
/// libuv is already linked into every module-client target by
/// `wish_finalize_app_modules()` (`cmake/WishModules.cmake`), so this file
/// needs no CMake change.
///
/// All of this module's structure -- headers via `curl -i`, a `-w`
/// sentinel trailer for status/timing/size -- lives in argv construction
/// and output parsing in `curl_source`, not here. This helper adds no new
/// capability over `docker_process`/`kubectl_process`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::curl {

/// @brief Result of one `run_curl_cli` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Runs `<binary> <args>` (no shell involved -- `args` is a real argv
/// array, so URLs / header values / body text with spaces or shell
/// metacharacters need no escaping), blocking until it exits.
///
/// @param args    Arguments after the program name, e.g.
///                `{"-sS", "-i", "-X", "GET", "https://example.com"}`.
/// @param binary  Program to exec. Defaults to `"curl"`; production code
///                never passes anything else. The parameter exists purely so
///                `tests/test_curl_process.cpp` can exercise the argv / pipe
///                / exit-code plumbing with a guaranteed-present stub
///                (`printf`, `false`) without making a real network call.
process_result run_curl_cli(const std::vector<std::string>& args, const std::string& binary = "curl");

} // namespace bdg::wish::curl
