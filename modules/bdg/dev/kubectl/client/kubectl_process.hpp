// MIT License © 2026 Binary Dice Games
/// @file kubectl_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// A near-verbatim copy of
/// `modules/bdg/dev/docker/client/docker_process.hpp` (which is itself a
/// copy of the `git` module's `git_process.hpp` -- see that file's header
/// comment for the full rationale). In short: `bdg::bison::term::terminal`
/// is a pty-attached, shell-string, stdio-hijacking helper for the
/// interactive `--transport term` session -- unsuitable for issuing many
/// quick argv-array `kubectl <args>` invocations from inside a long-running
/// `wish_client`. libuv is already vendored by bison and already linked into
/// every module-client target by `wish_finalize_app_modules()`
/// (`cmake/WishModules.cmake`), so this file needs no CMake change.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::kubectl {

/// @brief Result of one `run_kubectl_cli` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Runs `<binary> <args>` (no shell involved -- `args` is a real argv
/// array, so resource names / namespaces / jsonpath templates with spaces or
/// shell metacharacters need no escaping), blocking until it exits.
///
/// @param args    Arguments after the program name, e.g.
///                `{"get", "pods", "-A", "-o", "jsonpath={range .items[*]}..."}`.
/// @param binary  Program to exec. Defaults to `"kubectl"`; production code
///                never passes anything else. The parameter exists purely so
///                `tests/test_kubectl_process.cpp` can exercise the argv /
///                pipe / exit-code plumbing with a guaranteed-present stub
///                (`printf`, `false`) on a machine with no Kubernetes
///                cluster -- the one deviation from a straight copy of
///                `git_process::run_git()` (which hard-codes `"git"`),
///                justified because a throwaway `git init` repo is trivial to
///                create in a test but a reachable cluster is not.
process_result run_kubectl_cli(const std::vector<std::string>& args, const std::string& binary = "kubectl");

} // namespace bdg::wish::kubectl
