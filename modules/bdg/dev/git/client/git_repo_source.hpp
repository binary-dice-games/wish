// MIT License © 2025 Binary Dice Games
/// @file git_repo_source.hpp
/// @brief Client-side git command orchestration for the git module.
///
/// Owns the local repository path, runs every `git` plumbing command via
/// git_process::run_git(), parses the output, and pushes structured
/// snapshots to the server-side GitRepo form via its update_refs/update_log/
/// update_status/update_commit_files/update_diff RMI methods. Also reacts to
/// the form's `*_requested` events (see git.hpp's class doc comment) by
/// running the corresponding mutating git command and refreshing.
///
/// Mirrors process_explorer's client/server split: "gathering this
/// information is inherently OS-specific, and the machine a user wants
/// visibility into is their own" there becomes "the repo a user wants to
/// operate on is on their own machine, reachable only from the client"
/// here -- the server never touches git or the filesystem directly.
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::git {

class git_repo_source {
 public:
  git_repo_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::string repo_path);

  /// @brief Pushes refs, the commit graph, and working-directory status, in
  /// that order. Called once on startup (in response to "refresh_requested")
  /// and after every mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────
  void on_stage(const std::string& path);
  void on_unstage(const std::string& path);
  void on_commit(const std::string& message);
  void on_checkout(const std::string& ref);
  void on_create_branch(const std::string& name, const std::string& start_point);
  void on_delete_branch(const std::string& name, bool force);
  void on_fetch();
  void on_pull();
  void on_push();
  void on_merge(const std::string& ref);
  void on_stash_push();
  void on_stash_pop(int32_t index);
  void on_stash_apply(int32_t index);
  void on_stash_drop(int32_t index);
  void on_commit_files_requested(const std::string& hash);
  void on_diff_requested(const std::string& hash, const std::string& path, bool staged);

 private:
  void push_refs();
  void push_log();
  void push_status();
  bool working_tree_dirty();
  /// @brief Runs a mutating command, reports its result via GitRepo's
  /// command_result RMI method, and (on success) calls refresh_all().
  void run_and_refresh(const std::string& command_label, const std::vector<std::string>& args);

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  std::string repo_path_;
};

} // namespace bdg::wish::git
