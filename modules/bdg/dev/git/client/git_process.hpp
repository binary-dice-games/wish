// MIT License © 2025 Binary Dice Games
/// @file git_process.hpp
/// @brief Cross-platform, non-interactive "run a command, capture its
///        output" helper, built on libuv (`uv_spawn`).
///
/// `bdg::bison::term::terminal` (`extern/bison/src/term/terminal.hpp`) looks
/// like an obvious reuse candidate for this but isn't one: it exists purely
/// for the interactive `--transport term` session -- it spawns the child
/// attached to a real pseudo-terminal (`forkpty()`/ConPTY), takes a single
/// shell command *string* rather than an argv array, and for its lifetime
/// redirects the *calling process's own* stdout/stderr fds through a
/// CRLF-translating pump. None of that is safe or appropriate for issuing
/// many quick, argv-array `git <args>` invocations from inside a
/// long-running `wish_client`/`wish_server` process.
///
/// Bison already vendors/builds libuv (`extern/bison/extern/libuv`, CMake
/// target `uv_a`) for its own RMI transports; `uv_a` is linked into this
/// module's client target by `wish_finalize_app_modules()`
/// (`cmake/WishModules.cmake`) specifically so this file can use it
/// directly, with no bison changes required -- see that function's comment.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::git {

/// @brief Result of one `run_git` invocation.
struct process_result {
  int exit_code{-1}; // -1 == the process could not be spawned at all.
  std::string stdout_text;
  std::string stderr_text;

  bool ok() const {
    return exit_code == 0;
  }
};

/// @brief Runs `git <args>` (no shell involved -- `args` is a real argv
/// array, so paths/messages with spaces or shell metacharacters need no
/// escaping) with working directory @p cwd, blocking until it exits.
///
/// Sets `GIT_TERMINAL_PROMPT=0` in the child's environment so a command
/// needing interactive credentials the system git credential helper/SSH
/// agent can't supply (e.g. `fetch`/`pull`/`push` against a private repo
/// with no cached credentials) fails fast with a clear stderr message
/// instead of hanging forever waiting for a prompt this frontend has no UI
/// for -- a documented limitation, not an oversight (see this module's
/// README.md).
///
/// @param cwd  Repository working directory. Must not be empty.
/// @param args Arguments after "git" itself, e.g. `{"status", "--porcelain=v2"}`.
process_result run_git(const std::string& cwd, const std::vector<std::string>& args);

} // namespace bdg::wish::git
