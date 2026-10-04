// MIT License © 2026 Binary Dice Games
/// @file docker_source.hpp
/// @brief Client-side `docker` command orchestration for the docker module.
///
/// Runs every `docker` command through common::tool_source (see
/// common/tool_source.hpp), parses the tab-delimited `--format`
/// output, and pushes structured snapshots to the server-side
/// DockerFrontend form via its update_* RMI methods. Also reacts to the
/// form's `*_requested` events (see server/docker.hpp) by running the
/// corresponding mutating `docker` command and refreshing.
///
/// Mirrors git_repo_source: "the repo a user wants to operate on is on
/// their own machine, reachable only from the client" there becomes "the
/// Docker daemon a user wants to manage is the one their `docker` CLI
/// talks to" here -- the server never touches `docker` directly.
#pragma once

#include "modules/bdg/common/tool_source.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::docker {

using common::process_result;

class docker_source : public common::tool_source {
 public:
  docker_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker);
  ~docker_source() override;

  /// @brief Pushes the containers / images / volumes / networks snapshots,
  /// in that order. Called once on startup (in response to
  /// "refresh_requested") and after every mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────
  void on_container_action(const std::string& id, const std::string& action);
  void on_image_action(const std::string& id, const std::string& action);
  void on_volume_action(const std::string& name, const std::string& action);
  void on_network_action(const std::string& id, const std::string& action);
  void on_prune(const std::string& scope);
  void on_pull_image(const std::string& ref);
  void on_create_volume(const std::string& name);

  /// @brief Pushes one `docker logs --tail <lines> --timestamps <id>`
  /// snapshot to update_logs. When @p follow is true, also starts a
  /// background thread re-running that snapshot every ~2 s until the next
  /// on_logs_requested() call (or destruction) -- the `top` client's
  /// sampling-thread pattern; NOT `docker logs -f` streaming.
  void on_logs_requested(const std::string& id, bool follow, int32_t lines);

  /// @brief Pushes one `docker <kind> inspect <id>` snapshot to
  /// update_inspect (verbatim JSON text, never parsed).
  void on_inspect_requested(const std::string& kind, const std::string& id);

  /// @brief Starts a background thread that runs `docker stats --no-stream`
  /// every ~3 s and pushes each sample to the form's update_stats RMI method
  /// until destruction. The `top` client's sampling-thread pattern; runs
  /// `run_process()` directly (never run_logged()) so these frequent
  /// polls never reach the Console window. Idempotent -- a second call is a
  /// no-op while a thread is already running.
  void start_stats_polling();
  /// @brief Signals the stats poll thread to stop and forgets it.
  void stop_stats_polling();

 private:
  void push_containers();
  void push_images();
  void push_volumes();
  void push_networks();

  // Every one-shot `docker` invocation goes through run_logged() so the
  // Console window is a complete trace -- the exception is the Logs "Follow"
  // re-poll thread, which would flood it every ~2 s (git's Log-window
  // lesson).

  /// @brief Runs a mutating `docker` command, reports its result via
  /// DockerFrontend's command_result RMI method (tagged with @p scope so
  /// the right window's status label is written), and calls refresh_all().
  void run_and_refresh(
      const std::string& label, const std::string& scope, const std::vector<std::string>& args);

  /// @brief Runs `docker logs --tail <lines> --timestamps <id>` once and
  /// pushes update_logs (shared by on_logs_requested() and the follow
  /// thread).
  void push_logs_snapshot(const std::string& id, int32_t lines);

  /// @brief Signals any running follow thread to stop and forgets it.
  void stop_follow();

  // Logs "Follow" background thread (top's sampling-thread pattern).
  std::shared_ptr<std::atomic<bool>> follow_stop_;

  // `docker stats` background poll thread (started once, runs for the life
  // of the form). Separate stop flag from follow_stop_ -- the two threads
  // have independent lifetimes.
  std::shared_ptr<std::atomic<bool>> stats_stop_;
};

} // namespace bdg::wish::docker
