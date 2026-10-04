// MIT License © 2026 Binary Dice Games
/// @file helm_source.hpp
/// @brief Client-side `helm` command orchestration for the helm module.
///
/// Runs every `helm` command through common::tool_source (see
/// common/tool_source.hpp), parses the tab-separated table output (see
/// helm_table_parser.hpp), and pushes structured snapshots to the server-side
/// HelmFrontend form via its update_* RMI methods. Also reacts to the form's
/// `*_requested` events (see server/helm.hpp) by running the corresponding
/// `helm` command and refreshing.
///
/// Mirrors kubectl_source: the cluster and chart repositories a user wants to
/// manage are the ones their own `helm` CLI is configured for, reachable only
/// from the client -- the server never touches `helm` directly.
#pragma once

#include "modules/bdg/common/tool_source.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::helm {

using common::process_result;

class helm_source : public common::tool_source {
 public:
  helm_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker);

  /// @brief Pushes the releases and repositories snapshots, in that order.
  /// Called once on startup, on "refresh_requested", and after every
  /// mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────

  /// @brief `uninstall` -> `helm uninstall <name> -n <ns>`; `rollback` ->
  /// `helm rollback <name> [<revision>] -n <ns>` (an empty @p revision rolls
  /// back to the previous one), then re-pushes that release's history.
  void on_release_action(
      const std::string& name, const std::string& ns, const std::string& action, const std::string& revision);

  /// @brief `update` -> `helm repo update [<name>]` (an empty @p name updates
  /// every repository); `remove` -> `helm repo remove <name>`.
  void on_repo_action(const std::string& name, const std::string& action);

  /// @brief `helm repo add <name> <url>`.
  void on_repo_add(const std::string& name, const std::string& url);

  /// @brief Pushes one `helm search repo [<query>]` snapshot to update_charts
  /// (an empty @p query lists every chart of every repository). The query is
  /// remembered and re-run whenever the repositories change.
  void on_search_requested(const std::string& query);

  /// @brief Parameters of one `helm install` / `helm upgrade` run (the
  /// `install_requested` event payload).
  struct install_request {
    bool upgrade{false};
    std::string chart;
    std::string version; // "" = latest
    std::string release;
    std::string ns;
    std::string values; // YAML text passed as `-f <temp file>`; "" = none
    bool create_namespace{false};
    bool wait{false};
  };

  /// @brief `helm install|upgrade <release> <chart> -n <ns> [--version
  /// <version>] [-f <values file>] [--create-namespace] [--wait]`, reported
  /// with command_result scope "install". Non-empty values are written to an
  /// owner-only temp file for the duration of the command.
  void on_install_requested(const install_request& req);

  /// @brief Answers `install_values_requested` via set_install_values:
  /// @p source `chart` -> `helm show values <chart> [--version]`, `release`
  /// -> `helm get values <name> -n <ns> -o yaml` (the release's current
  /// user-supplied values). @p token is echoed back.
  void on_install_values_requested(
      int32_t token, const std::string& source, const std::string& chart, const std::string& version,
      const std::string& name, const std::string& ns);

  /// @brief Pushes one `helm history <name> -n <ns>` snapshot to
  /// update_history.
  void on_history_requested(const std::string& name, const std::string& ns);

  /// @brief Pushes the verbatim output of one read-only command to
  /// update_details. @p kind selects it: `status` / `values` / `manifest` /
  /// `notes` run `helm status` / `helm get <kind>` on release @p name in
  /// @p ns; `chart_values` / `chart_readme` run `helm show values|readme` on
  /// chart @p name (optionally pinned to @p version).
  void on_details_requested(
      const std::string& kind, const std::string& name, const std::string& ns, const std::string& version);

 private:
  void push_releases();
  void push_repos();

  /// @brief helm's `Error:` message from stderr (see error_summary()), or
  /// stdout when stderr is empty.
  std::string error_text(const process_result& r) const override;

  /// @brief Runs a mutating `helm` command, reports its result and calls
  /// refresh_all(). @return whether the command succeeded.
  bool run_and_refresh(const std::string& label, const std::string& scope, const std::vector<std::string>& args);

  /// @brief Whether every value in @p values passes is_safe_arg(); reports a
  /// failure to @p scope's status label otherwise.
  bool check_args(const std::string& label, const std::string& scope, const std::vector<std::string>& values);

  std::string last_query_; // the Charts window's current search
};

} // namespace bdg::wish::helm
