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
/// Destructive actions (delete / drain) are gated behind the built-in
/// MessageBox form (form::instantiate_child_form(), "yes_no" preset) -- see
/// show_confirm() below, a direct port of docker_frontend::show_confirm().
///
/// Owns six independently dockable Windows -- Pods (the main root),
/// Deployments, Services, Nodes, Logs, Describe -- registered by hand in
/// on_init() exactly as docker.cpp's build_list_window() / build_text_window()
/// do.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class message_box;

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
class kubectl_frontend : public form {
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

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── Generic list-window plumbing ──────────────────────────────────────
  //
  // Pods / Deployments / Services / Nodes are four near-identical toolbar +
  // Table windows. One `list_window` bundles the per-window widgets; one
  // `list_row` type + one dispatch map (`menu_action_targets_`) serve all
  // four.

  struct list_window {
    std::string root_key;
    bison::key_t window_id;
    ui_element_ptr status_label;
    ui_element_ptr table;
    std::string scope; // "pod" / "deployment" / "service" / "node"

    // Toolbar filter widgets (not every window wires every one).
    ui_element_ptr name_filter_input;
    bison::key_t name_filter_id;
    ui_element_ptr ns_filter_input;
    bison::key_t ns_filter_id;
    bison::key_t phase_combo_id; // Pods only.

    std::string name_filter;
    std::string ns_filter;
    int32_t phase_filter{0}; // 0 All, 1 Running, 2 Pending, 3 Succeeded, 4 Failed.
  };

  struct list_row {
    ui_element_ptr row;
    std::string scope;
    std::string name;  // resource name (menu action key + filter + confirm)
    std::string ns;    // namespace ("" for cluster-scoped nodes)
    std::string state; // pods: phase (the state Combo filters on)
    size_t child_key{0};
    std::vector<bison::key_t> object_ids; // erased together on rebuild
  };

  struct row_action {
    std::string scope;
    std::string name;
    std::string ns;
    std::string action;
  };

  struct menu_spec {
    std::string label;
    std::string action;
    bool confirm{false};
  };

  /// @brief Import @p layout_json, register every node, cache the status /
  /// table widgets, and register the tree as its own dockable top-level root
  /// at @p root_key (docker.cpp's build_list_window() pattern). @p
  /// wire_toolbar binds that window's own toolbar buttons / inline fields.
  void build_list_window(
      list_window& lw, const char* layout_json, const std::string& root_key, const std::string& scope,
      const std::function<void(ui_tree&)>& wire_toolbar);

  void clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key);

  void add_list_row(
      list_window& lw, std::vector<list_row>& rows, size_t& next_key, list_row&& meta,
      const std::vector<ui_element_ptr>& cells, const std::vector<menu_spec>& items);

  void set_status(list_window& lw, const std::string& text, bool ok);

  // ── Per-window rebuild ───────────────────────────────────────────────
  void rebuild_pods(const bison::dynamic& args);
  void rebuild_deployments(const bison::dynamic& args);
  void rebuild_services(const bison::dynamic& args);
  void rebuild_nodes(const bison::dynamic& args);

  // ── Logs / Describe text panes ──────────────────────────────────────
  void build_text_window(
      const std::string& root_key, const char* layout_json, bison::key_t& window_id_out,
      ui_element_ptr& table_out, const std::function<void(ui_tree&)>& wire_toolbar);
  void set_text_lines(
      const ui_element_ptr& table, std::vector<bison::key_t>& line_ids, size_t& next_key, const std::string& text);
  void emit_logs_request();
  void emit_describe_request();

  /// @brief Re-apply a list window's name / namespace / phase filters to
  /// each row's `visible` field (docker's retroactive-filter pattern).
  void apply_list_filter(list_window& lw, std::vector<list_row>& rows);

  // ── Confirmation modal (docker_frontend::show_confirm() port) ────────
  void show_confirm(const std::string& message, std::function<void()> on_confirm);

  // ── Small builders ──────────────────────────────────────────────────
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);

  // ── State ──────────────────────────────────────────────────────────
  std::string title_;

  list_window pods_;
  list_window deployments_;
  list_window services_;
  list_window nodes_;

  std::vector<list_row> pod_rows_;
  std::vector<list_row> deployment_rows_;
  std::vector<list_row> service_rows_;
  std::vector<list_row> node_rows_;
  size_t next_pod_key_{0};
  size_t next_deployment_key_{0};
  size_t next_service_key_{0};
  size_t next_node_key_{0};

  std::shared_ptr<message_box> confirm_dialog_;

  // Logs window.
  std::string logs_root_key_;
  bison::key_t logs_window_id_;
  ui_element_ptr logs_table_;
  ui_element_ptr logs_target_label_;
  bison::key_t logs_follow_id_;
  bison::key_t logs_lines_id_;
  std::vector<bison::key_t> logs_line_ids_;
  size_t next_logs_line_key_{0};
  std::string open_logs_name_;
  std::string open_logs_ns_;
  bool logs_follow_{false};
  int32_t logs_lines_{500};

  // Describe window.
  std::string describe_root_key_;
  bison::key_t describe_window_id_;
  ui_element_ptr describe_table_;
  ui_element_ptr describe_target_label_;
  std::vector<bison::key_t> describe_line_ids_;
  size_t next_describe_line_key_{0};
  std::string open_describe_name_;
  std::string open_describe_ns_;
  std::string open_describe_kind_;

  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
  std::unordered_map<bison::key_t, row_action, bison::key_t, bison::key_t> menu_action_targets_;
};

/// @brief Register KubectlFrontend in the "wish" bison namespace.
void register_kubectl();

} // namespace bdg::wish
