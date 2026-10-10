// MIT License © 2026 Binary Dice Games
/// @file kubectl.hpp
/// @brief Server-side KubectlFrontend form -- a Kubernetes-dashboard-style
///        GUI over the local `kubectl` CLI.
///
/// All `kubectl` invocation and output parsing happens client-side (see
/// client/kubectl_source.hpp) -- the cluster whose state matters is the one
/// the user's own kubeconfig / current-context points at, reachable only
/// from the client. This form only renders whatever snapshot it was last
/// given via its update_* RMI methods, and emits high-level *_requested
/// events the client reacts to by running the corresponding `kubectl`
/// command and pushing a fresh snapshot. Mirrors the `docker` module's
/// client/server split (modules/bdg/dev/docker/server/docker.hpp), which in
/// turn mirrors `git`.
///
/// Built on the panels shared by the bdg tool forms
/// (modules/bdg/common/server): destructive actions (delete / drain) are
/// gated behind tool_form::show_confirm(); the Pods / Deployments /
/// Services / Nodes windows are common::list_panel; Logs / Describe are
/// common::text_viewer_panel; Console (a trace of every `kubectl` command
/// the client ran) is common::console_panel; the Top window's graphs are
/// common::rolling_plot. Pods is the main root; every other window docks
/// independently.
#pragma once

#include "modules/bdg/common/server/console_panel.hpp"
#include "modules/bdg/common/server/list_panel.hpp"
#include "modules/bdg/common/server/rolling_plot.hpp"
#include "modules/bdg/common/server/text_viewer_panel.hpp"
#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Kubernetes-dashboard-style GUI form for the local `kubectl` CLI.
///
/// Emitted events (see DESIGN.md "5. Public API Contract"):
///   - `"closed"` -- the main (Pods) window was closed.
///   - `"refresh_requested"` -- no payload; any window's Refresh button.
///   - `"pod_action_requested"` -- `{ name, namespace, action }` where
///     `action` is `delete`.
///   - `"deployment_action_requested"` -- `{ name, namespace, action }`
///     (`restart`, `delete`).
///   - `"service_action_requested"` -- `{ name, namespace, action }`
///     (`delete`).
///   - `"node_action_requested"` -- `{ name, action }` (`cordon`,
///     `uncordon`, `drain`).
///   - `"logs_requested"` -- `{ name, namespace, follow, lines }`.
///   - `"describe_requested"` -- `{ kind, name, namespace }` where `kind` is
///     `pod`, `deployment`, `service` or `node`.
class kubectl_frontend : public common::tool_form {
 public:
  explicit kubectl_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the Pods table. @p args.pods is a dynamic
  /// array, each `{ namespace, name, ready ("1/1"), phase
  /// ("Running"/"Pending"/"Succeeded"/"Failed"/...), reason (container
  /// waiting reason such as "CrashLoopBackOff", or ""), restarts (string),
  /// age (string) }`.
  bison::dynamic do_update_pods(const bison::dynamic& args);

  /// @brief RMI method: replace the Deployments table. @p args.deployments
  /// -- each `{ namespace, name, ready ("2/3"), uptodate, available, age }`.
  bison::dynamic do_update_deployments(const bison::dynamic& args);

  /// @brief RMI method: replace the Services table. @p args.services -- each
  /// `{ namespace, name, type, cluster_ip, ports, age }`.
  bison::dynamic do_update_services(const bison::dynamic& args);

  /// @brief RMI method: replace the Nodes table. @p args.nodes -- each
  /// `{ name, status ("Ready"/"NotReady"[,SchedulingDisabled]), schedulable
  /// ("true"/"false"), version, age }`.
  bison::dynamic do_update_nodes(const bison::dynamic& args);

  /// @brief RMI method: fill the Logs window's text. @p args holds `name`
  /// and `namespace` (echoed from `logs_requested` -- the call is discarded
  /// if it no longer matches the window's open target, docker's
  /// do_update_logs staleness guard), `title` (string) and `text` (string;
  /// split on `\n` into one row per line).
  bison::dynamic do_update_logs(const bison::dynamic& args);

  /// @brief RMI method: fill the Describe window's text. @p args holds
  /// `kind`, `name`, `namespace` (staleness guard), `title` and `text`.
  bison::dynamic do_update_describe(const bison::dynamic& args);

  /// @brief RMI method: report the result of a client-run `kubectl` command,
  /// shown in the relevant window's status label. @p args holds `command`
  /// (string), `ok` (bool), `output` (string, shown on failure), and an
  /// optional `scope` ("pods"/"deployments"/"services"/"nodes", default
  /// "pods") selecting which window's status label to write.
  bison::dynamic do_command_result(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Console window's `kubectl`
  /// subprocess trace. @p args holds `command` (string, e.g. `"kubectl get
  /// pods -A"`), `exit_code` (int32), `ok` (bool) and `output` (string, a
  /// single-line preview). See common::console_panel.
  bison::dynamic do_append_command_log(const bison::dynamic& args);

  /// @brief RMI method: push one live `kubectl top` sample to the Top
  /// window. @p args holds `pods` -- each `{ namespace, name, cpu (string
  /// "5m"), cpu_m (float millicores), mem (string "12Mi"), mem_mib (float
  /// MiB) }` -- `nodes` -- each `{ name, cpu (string), cpu_m (float),
  /// cpu_pct (float), mem (string), mem_mib (float), mem_pct (float) }` --
  /// and an optional `error` (string; shown in the status label when
  /// `kubectl top` could not run, e.g. metrics-server missing). Appends one
  /// point to every per-pod / per-node line plus each plot's aggregate line,
  /// and rebuilds the two current-values tables. Fed by kubectl_source's
  /// background poll thread -- deliberately NOT traced in the Console window.
  bison::dynamic do_update_stats(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── List windows ─────────────────────────────────────────────────────

  /// One resource row: what the filters and row actions work on.
  struct resource {
    std::string scope; // "pod" / "deployment" / "service" / "node"
    std::string name;
    std::string ns; // "" for cluster-scoped nodes
    std::string state; // pods: phase (the state Combo filters on)
  };

  /// A list window plus its toolbar filters (not every window has every
  /// filter; an unset id never matches an event).
  struct list_window {
    common::list_panel<resource> panel;
    bison::key_t name_filter_id;
    bison::key_t ns_filter_id;
    bison::key_t phase_combo_id; // Pods only.
    std::string name_filter;
    std::string ns_filter;
    int32_t phase_filter{0}; // 0 All, 1 Running, 2 Pending, 3 Succeeded, 4 Failed.
  };

  /// @brief Builds @p lw from @p layout_json at @p root_key and binds its
  /// Refresh button and filter widgets.
  void build_list_window(list_window& lw, const char* layout_json, const std::string& root_key);

  void rebuild_pods(const bison::dynamic& args);
  void rebuild_deployments(const bison::dynamic& args);
  void rebuild_services(const bison::dynamic& args);
  void rebuild_nodes(const bison::dynamic& args);

  /// @brief Re-applies @p lw's name / namespace / phase filters.
  void apply_list_filter(list_window& lw);

  /// @brief A row-menu item running @p action on @p r (see run_row_action()).
  common::menu_item action_item(const std::string& label, const resource& r, const std::string& action, bool confirm);
  /// @brief Opens Logs / Describe for @p r, or emits
  /// `<scope>_action_requested` -- after a confirmation for delete / drain.
  void run_row_action(const resource& r, const std::string& action);

  void emit_logs_request();
  void emit_describe_request();

  // ── State ──────────────────────────────────────────────────────────
  std::string title_;

  list_window pods_;
  list_window deployments_;
  list_window services_;
  list_window nodes_;

  common::text_viewer_panel logs_;
  bison::key_t logs_follow_id_;
  bison::key_t logs_lines_id_;
  std::string open_logs_name_;
  std::string open_logs_ns_;
  bool logs_follow_{false};
  int32_t logs_lines_{500};

  common::text_viewer_panel describe_;
  std::string open_describe_name_;
  std::string open_describe_ns_;
  std::string open_describe_kind_;

  common::console_panel console_;

  // ── Top window (live `kubectl top` CPU/memory graphs) ───────────────
  //
  // Four rolling plots -- pod CPU (millicores), pod memory (MiB), node
  // CPU %, node memory % -- with an aggregate line ("Total" for the
  // additive pod plots, "Cluster avg" for the node % plots), and two
  // current-values tables.
  std::string top_root_key_;
  bison::key_t top_window_id_;
  ui_element_ptr top_status_label_;
  common::table_rows<> top_pods_;
  common::table_rows<> top_nodes_;
  common::rolling_plot pods_cpu_plot_;
  common::rolling_plot pods_mem_plot_;
  common::rolling_plot nodes_cpu_plot_;
  common::rolling_plot nodes_mem_plot_;

  void build_top_window();
  /// @brief Replaces every row of @p rows with one per entry of
  /// @p args.<array_key>, built by @p make_cells.
  void rebuild_top_table(
      common::table_rows<>& rows,
      const bison::dynamic& args,
      bison::key_t array_key,
      const std::function<std::vector<ui_element_ptr>(const bison::dynamic&)>& make_cells);
};

/// @brief Register KubectlFrontend in the "wish" bison namespace.
void register_kubectl();

} // namespace bdg::wish
