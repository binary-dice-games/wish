// MIT License © 2025 Binary Dice Games
/// @file git_process.cpp
/// @brief Implementation of run_git() / resolve_repo_root() on top of the
///        shared dev::run_process().
#include "git_process.hpp"

#include "modules/bdg/dev/common/text.hpp"

#include <cstdlib>

namespace bdg::wish::git {

namespace {

// Ensures GIT_TERMINAL_PROMPT=0 is set in *this process's* environment
// exactly once -- every spawned child inherits it (run_process() leaves the
// environment alone). A tiny, narrowly-scoped platform guard (CLAUDE.md's
// convention for a small divergence) rather than a separate _posix/_win
// file: setenv()/_putenv_s() are the only differing calls.
void ensure_git_terminal_prompt_disabled() {
  static const bool done = [] {
#if defined(_WIN32)
    _putenv_s("GIT_TERMINAL_PROMPT", "0");
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
#endif
    return true;
  }();
  (void)done;
}

} // namespace

process_result run_git(const std::string& cwd, const std::vector<std::string>& args, const common::run_hooks* hooks) {
  ensure_git_terminal_prompt_disabled();
  if (cwd.empty())
    return {};

  dev::process_options options;
  options.cwd = cwd;
  options.hooks = hooks;
  return dev::run_process(dev::concat_args({"git"}, args), options);
}

std::string resolve_repo_root(const std::string& path) {
  auto r = run_git(path, {"rev-parse", "--show-toplevel"});
  if (!r.ok() || r.stdout_text.empty())
    return path;
  return dev::trim_eol(r.stdout_text);
}

} // namespace bdg::wish::git
