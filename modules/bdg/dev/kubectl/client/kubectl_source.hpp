// MIT License © 2026 Binary Dice Games
/// @file kubectl_source.hpp
/// @brief Client-side `kubectl` command orchestration for the kubectl module.
///
/// Owns the proxy, runs every `kubectl` command via
/// kubectl_process::run_kubectl_cli(), parses the tab-delimited `-o jsonpath`
/// output, and pushes structured snapshots to the server-side
/// KubectlFrontend form via its update_* RMI methods. Also reacts to the
/// form's `*_requested` events (see server/kubectl.hpp) by running the
/// corresponding mutating `kubectl` command and refreshing.
///
/// Mirrors docker_source: "the Docker daemon a user wants to manage is the
/// one their `docker` CLI talks to" there becomes "the cluster a user wants
/// to manage is the one their `kubectl` current-context points at" here --
/// the server never touches `kubectl` directly.
#pragma once

#include "kubectl_process.hpp"
#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::kubectl {

class kubectl_source {
 public:
  explicit kubectl_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy);
  ~kubectl_source();

  /// @brief Pushes the pods / deployments / services / nodes snapshots, in
  /// that order. Called once on startup (in response to "refresh_requested")
  /// and after every mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────
  void on_pod_action(const std::string& name, const std::string& ns, const std::string& action);
  void on_deployment_action(const std::string& name, const std::string& ns, const std::string& action);
  void on_service_action(const std::string& name, const std::string& ns, const std::string& action);
  void on_node_action(const std::string& name, const std::string& action);

  /// @brief Pushes one `kubectl logs <name> -n <ns> --tail <lines>
  /// --timestamps` snapshot to update_logs. When @p follow is true, also
  /// starts a background thread re-running that snapshot every ~2 s until the
  /// next on_logs_requested() call (or destruction) -- the `top` client's
  /// sampling-thread pattern; NOT `kubectl logs -f` streaming.
  void on_logs_requested(const std::string& name, const std::string& ns, bool follow, int32_t lines);

  /// @brief Pushes one `kubectl describe <kind> <name> [-n <ns>]` snapshot
  /// to update_describe (verbatim text, never parsed).
  void on_describe_requested(const std::string& kind, const std::string& name, const std::string& ns);

 private:
  void push_pods();
  void push_deployments();
  void push_services();
  void push_nodes();

  /// @brief Runs `kubectl <args>` via run_kubectl_cli() and pushes a trace
  /// row to the Console window (KubectlFrontend's append_command_log RMI
  /// method). Every one-shot `kubectl` invocation goes through here so the
  /// Console window is a complete trace -- the exception is the Logs
  /// "Follow" re-poll thread, which would flood it every ~2 s (git's
  /// Log-window lesson). Mirrors docker_source::run_logged().
  process_result run_logged(const std::vector<std::string>& args);

  /// @brief Runs a mutating `kubectl` command, reports its result via
  /// KubectlFrontend's command_result RMI method (tagged with @p scope so
  /// the right window's status label is written), and calls refresh_all().
  void run_and_refresh(const std::string& label, const std::string& scope, const std::vector<std::string>& args);

  /// @brief Runs `kubectl logs ...` once and pushes update_logs (shared by
  /// on_logs_requested() and the follow thread).
  void push_logs_snapshot(const std::string& name, const std::string& ns, int32_t lines, bool following);

  /// @brief Signals any running follow thread to stop and forgets it.
  void stop_follow();

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;

  // Logs "Follow" background thread (top's sampling-thread pattern).
  std::shared_ptr<std::atomic<bool>> follow_stop_;
};

} // namespace bdg::wish::kubectl
