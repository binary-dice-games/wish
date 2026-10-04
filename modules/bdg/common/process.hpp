// MIT License © 2026 Binary Dice Games
/// @file process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper shared by every bdg/dev module, built on libuv
///        (`uv_spawn`).
///
/// `bdg::bison::term::terminal` (`extern/bison/src/term/terminal.hpp`) looks
/// like an obvious reuse candidate for this but isn't one: it exists purely
/// for the interactive `--transport term` session -- it spawns the child
/// attached to a real pseudo-terminal (`forkpty()`/ConPTY), takes a single
/// shell command *string* rather than an argv array, and for its lifetime
/// redirects the *calling process's own* stdout/stderr fds through a
/// CRLF-translating pump. None of that is safe or appropriate for issuing
/// many quick, argv-array `<tool> <args>` invocations from inside a
/// long-running `wish_client` process.
///
/// Bison already vendors/builds libuv (`extern/bison/extern/libuv`, CMake
/// target `uv_a`); `wish_finalize_app_modules()` (`cmake/WishModules.cmake`)
/// links it into every module-client target. The implementation
/// (process.cpp) is the only file that includes `<uv.h>`, which on native
/// Windows drags in `<windows.h>` and its macros. `wish_add_module()`
/// compiles this directory's sources into the client of every enabled bdg
/// module (an organization's `common/` directory).
#pragma once

#include "modules/bdg/common/process_hooks.hpp"

#include <string>
#include <vector>

namespace bdg::wish::common {

/// @brief Result of one run_process() invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Optional settings for one run_process() invocation.
struct process_options {
  /// Working directory of the child; empty inherits the caller's.
  std::string cwd;
  /// When non-empty, written to the child's stdin, which is then closed.
  /// When empty, stdin is not connected, so a command that would prompt (a
  /// password, a yes/no question) fails instead of hanging.
  std::string stdin_text;
  /// Live-output / tick callbacks (see process_hooks.hpp); a tick returning
  /// false stops the process. Null for none.
  const run_hooks* hooks{nullptr};
};

/// @brief What a failed command had to say: its stderr, or its stdout when
/// stderr is empty (some tools report errors on stdout).
inline const std::string& error_output(const process_result& r) {
  return r.stderr_text.empty() ? r.stdout_text : r.stderr_text;
}

/// @brief @p head followed by @p tail -- e.g. a program (or a launcher such
/// as `{"python3", "-m", "pip"}`) followed by its arguments.
inline std::vector<std::string> concat_args(std::vector<std::string> head, const std::vector<std::string>& tail) {
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

// ── run_process ──────────────────────────────────────────────────────────────

/// @brief Runs @p argv (no shell involved -- it is a real argv array, so
/// names, paths, SQL text or URLs with spaces or shell metacharacters need no
/// escaping), blocking until it exits.
///
/// @param argv     The program followed by its arguments. `argv[0]` is
///                 PATH-searched when it has no path separator, the same way
///                 execvp()/CreateProcess() would.
/// @param options  Working directory, stdin text and hooks; see
///                 process_options.
/// @return exit_code -1 (with stderr_text set) when @p argv is empty or the
///         process cannot be spawned; otherwise the child's exit code
///         (128 + signal when it was stopped by a signal).
process_result run_process(const std::vector<std::string>& argv, const process_options& options = {});

} // namespace bdg::wish::common
