// MIT License © 2025 Binary Dice Games
/// @file git.cpp
/// @brief Client-side runner for the git (GitRepo) embedded app.
///
/// Requires a repository path via app_args(): `wish client --run=git -- /path/to/repo`.
/// Owns a git_repo_source instance (all actual `git` invocation/parsing) and
/// wires the GitRepo form's `*_requested` events to it -- see git.hpp
/// (server) for the full event contract, and git_repo_source.hpp for what
/// each handler below actually runs.
#include "git.hpp"
#include "git_process.hpp"
#include "git_repo_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <iostream>
#include <memory>

namespace bdg::wish {

using namespace bison;

void run_git(wish_app_host& s) {
  if (s.app_args().empty()) {
    std::cerr << "git: a repository path is required, e.g. `wish client --run=git -- /path/to/repo`\n";
    s.signal_done();
    return;
  }
  const std::string repo_path_arg = s.app_args()[0];

  {
    auto check = git::run_git(repo_path_arg, {"rev-parse", "--is-inside-work-tree"});
    if (!check.ok()) {
      std::cerr << "git: '" << repo_path_arg << "' is not a git repository (or git is not on PATH)\n";
      s.signal_done();
      return;
    }
  }

  // Resolve to the repo's actual top-level directory rather than using
  // repo_path_arg (e.g. "." or any other relative/non-root path) verbatim
  // as every subsequent git subprocess's cwd -- see resolve_repo_root()'s
  // doc comment for why this matters (a real, live-reproduced bug when
  // wish itself is launched from somewhere other than the repo root, not
  // merely theoretical -- see DESIGN.md's "Design Decisions" entry).
  const std::string repo_path = git::resolve_repo_root(repo_path_arg);

  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "GitRepo"_key).get());
  auto source = std::make_shared<git::git_repo_source>(proxy, repo_path);

  proxy->onEvent("refresh_requested"_key, [source](dynamic) { source->refresh_all(); });

  proxy->onEvent("stage_requested"_key, [source](dynamic payload) { source->on_stage(payload.as<std::string>("path"_key)); });
  proxy->onEvent(
      "unstage_requested"_key, [source](dynamic payload) { source->on_unstage(payload.as<std::string>("path"_key)); });
  proxy->onEvent(
      "commit_requested"_key, [source](dynamic payload) { source->on_commit(payload.as<std::string>("message"_key)); });
  proxy->onEvent(
      "checkout_requested"_key, [source](dynamic payload) { source->on_checkout(payload.as<std::string>("ref"_key)); });
  proxy->onEvent("create_branch_requested"_key, [source](dynamic payload) {
    source->on_create_branch(payload.as<std::string>("name"_key), payload.as<std::string>("start_point"_key));
  });
  proxy->onEvent("delete_branch_requested"_key, [source](dynamic payload) {
    source->on_delete_branch(payload.as<std::string>("name"_key), payload.as<bool>("force"_key));
  });
  proxy->onEvent("fetch_requested"_key, [source](dynamic) { source->on_fetch(); });
  proxy->onEvent("pull_requested"_key, [source](dynamic) { source->on_pull(); });
  proxy->onEvent("push_requested"_key, [source](dynamic) { source->on_push(); });
  proxy->onEvent(
      "merge_requested"_key, [source](dynamic payload) { source->on_merge(payload.as<std::string>("ref"_key)); });
  proxy->onEvent("stash_push_requested"_key, [source](dynamic) { source->on_stash_push(); });
  proxy->onEvent(
      "stash_pop_requested"_key, [source](dynamic payload) { source->on_stash_pop(payload.as<int32_t>("index"_key)); });
  proxy->onEvent("stash_apply_requested"_key, [source](dynamic payload) {
    source->on_stash_apply(payload.as<int32_t>("index"_key));
  });
  proxy->onEvent(
      "stash_drop_requested"_key, [source](dynamic payload) { source->on_stash_drop(payload.as<int32_t>("index"_key)); });
  proxy->onEvent("commit_files_requested"_key, [source](dynamic payload) {
    source->on_commit_files_requested(payload.as<std::string>("hash"_key));
  });
  proxy->onEvent("diff_requested"_key, [source](dynamic payload) {
    source->on_diff_requested(
        payload.as<std::string>("hash"_key), payload.as<std::string>("path"_key), payload.as<bool>("staged"_key));
  });

  proxy->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  // Initial population. GitRepo::on_init() (server) used to emit its own
  // one-time "refresh_requested" for this, but that event fires as part of
  // on_init() itself -- i.e. before instantiate() above even returns, let
  // alone before the onEvent() wiring just above runs -- so it was reliably
  // lost (confirmed live: graph/sidebar/files/log all stayed empty
  // indefinitely with no user interaction). It went unnoticed for as long
  // as the client also ran a ~2s background refresh_all() poll (removed --
  // see DESIGN.md's "no background polling" entry), whose first tick
  // silently papered over the missing initial load. Calling refresh_all()
  // directly here, now that every handler above is registered, has no such
  // race.
  source->refresh_all();
}

namespace {
struct git_app_registrar {
  git_app_registrar() {
    register_app({
        .name = "git",
        .organization = WISH_MODULE_BDG_DESKTOP_GIT_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_GIT_COLLECTION,
        .description = "SourceTree-style git GUI frontend for a local repository "
                        "(wish client --run=git -- /path/to/repo)",
        .params = {},
        .run = run_git,
    });
  }
};
const git_app_registrar git_app_registrar_instance;
} // namespace

} // namespace bdg::wish
