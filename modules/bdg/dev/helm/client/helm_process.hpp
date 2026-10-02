// MIT License © 2026 Binary Dice Games
/// @file helm_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// A near-verbatim copy of
/// `modules/bdg/dev/kubectl/client/kubectl_process.hpp` (itself a copy of
/// the `docker` / `git` modules' helpers -- see that file's header
/// comment for the full rationale). In short: `bdg::bison::term::terminal`
/// is a pty-attached, shell-string, stdio-hijacking helper for the
/// interactive `--transport term` session -- unsuitable for issuing many
/// quick argv-array `helm <args>` invocations from inside a long-running
/// `wish_client`. libuv is already vendored by bison and already linked into
/// every module-client target by `wish_finalize_app_modules()`
/// (`cmake/WishModules.cmake`), so this file needs no CMake change.
#pragma once

#include "modules/bdg/dev/common/process_hooks.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::helm {

/// @brief Result of one `run_helm_cli` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Runs `<binary> <args>` (no shell involved -- `args` is a real argv
/// array, so release names / namespaces / repository URLs with spaces or
/// shell metacharacters need no escaping), blocking until it exits.
///
/// @param args    Arguments after the program name, e.g.
///                `{"list", "-A", "--deployed"}`.
/// @param binary  Program to exec. Defaults to `"helm"`; production code
///                never passes anything else. The parameter exists purely so
///                `tests/test_helm_process.cpp` can exercise the argv /
///                pipe / exit-code plumbing with a guaranteed-present stub
///                (`printf`, `false`) on a machine with no `helm`
///                install -- the one deviation from a straight copy of
///                `git_process::run_git()` (which hard-codes `"git"`),
///                justified because a throwaway `git init` repo is trivial to
///                create in a test but a `helm` install with releases is not.
/// @param hooks  Optional live-output / tick callbacks (see
///               common/process_hooks.hpp); a tick returning false stops the
///               process. Null for none.
process_result run_helm_cli(
    const std::vector<std::string>& args, const std::string& binary = "helm", const dev::run_hooks* hooks = nullptr);

} // namespace bdg::wish::helm
