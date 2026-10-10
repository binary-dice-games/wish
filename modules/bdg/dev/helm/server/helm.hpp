// MIT License © 2026 Binary Dice Games
/// @file helm.hpp
/// @brief Server-side HelmFrontend form -- a GUI over the local `helm` CLI.
///
/// All `helm` invocation and output parsing happens client-side (see
/// client/helm_source.hpp) -- the cluster and chart repositories that matter
/// are the ones the user's own `helm` is configured for, reachable only from
/// the client. This form only renders whatever snapshot it was last given via
/// its update_* RMI methods, and emits high-level *_requested events the
/// client reacts to by running the corresponding `helm` command and pushing a
/// fresh snapshot. Mirrors the `kubectl` module's client/server split
/// (modules/bdg/dev/kubectl/server/kubectl.hpp).
///
/// Built on the panels shared by the bdg tool forms
/// (modules/bdg/common/server): destructive actions (uninstall / rollback /
/// repo remove) are gated behind tool_form::show_confirm(); Releases (the
/// main root), Repositories, Charts and History are common::list_panel;
/// Details is common::text_viewer_panel; Console (a trace of every `helm`
/// command the client ran) is common::console_panel. Install / upgrade go
/// through their own floating dialog instead, created on demand, whose
/// submit button is the confirmation.
#pragma once

#include "modules/bdg/common/server/console_panel.hpp"
#include "modules/bdg/common/server/list_panel.hpp"
#include "modules/bdg/common/server/text_viewer_panel.hpp"
#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief GUI form for the local `helm` CLI.
///
/// Emitted events:
///   - `"closed"` -- any window was closed; all six are torn down.
///   - `"refresh_requested"` -- no payload; the Releases / Repositories
///     Refresh button.
///   - `"release_action_requested"` -- `{ name, namespace, action,
///     revision }` where `action` is `uninstall` or `rollback`. `revision`
///     is only meaningful for `rollback`: the revision to roll back to, or
///     `""` for the previous one.
///   - `"repo_action_requested"` -- `{ name, action }` where `action` is
///     `update` (`name == ""` means every repository) or `remove`.
///   - `"repo_add_requested"` -- `{ name, url }`.
///   - `"search_requested"` -- `{ query }` (`""` lists every chart).
///   - `"install_requested"` -- `{ mode ("install"/"upgrade"), chart,
///     version ("" = latest), release, namespace, values (YAML text, "" =
///     none), create_namespace (bool), wait (bool) }`. Answer with
///     command_result scope "install".
///   - `"install_values_requested"` -- `{ token (int32), source, chart,
///     version, name, namespace }`: the dialog wants its Values box filled
///     with a chart's default values (`source == "chart"`) or a release's
///     current user-supplied values (`source == "release"`). Answer with
///     set_install_values, echoing `token`.
///   - `"history_requested"` -- `{ name, namespace }`.
///   - `"details_requested"` -- `{ kind, name, namespace, version }` where
///     `kind` is `status` / `values` / `manifest` / `notes` (release `name`
///     in `namespace`) or `chart_values` / `chart_readme` (chart `name` at
///     `version`, `namespace == ""`).
class helm_frontend : public common::tool_form {
 public:
  explicit helm_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the Releases table. @p args.releases is a
  /// dynamic array, each `{ namespace, name, revision, updated, status
  /// ("deployed"/"failed"/"pending-install"/...), chart, app_version }` (all
  /// strings).
  bison::dynamic do_update_releases(const bison::dynamic& args);

  /// @brief RMI method: replace the Repositories table. @p args.repos --
  /// each `{ name, url }`.
  bison::dynamic do_update_repos(const bison::dynamic& args);

  /// @brief RMI method: replace the Charts table with a search result.
  /// @p args holds `query` (string, echoed in the status label) and `charts`
  /// -- each `{ name ("repo/chart"), version, app_version, description }`.
  bison::dynamic do_update_charts(const bison::dynamic& args);

  /// @brief RMI method: replace the History table. @p args holds `name` and
  /// `namespace` (echoed from `history_requested` -- the call is discarded if
  /// they no longer match the window's open release) and `revisions` -- each
  /// `{ revision, updated, status, chart, description }`.
  bison::dynamic do_update_history(const bison::dynamic& args);

  /// @brief RMI method: fill the Details window's text. @p args holds `kind`,
  /// `name`, `namespace` (echoed from `details_requested`; the call is
  /// discarded if they no longer match the window's open target), `title`
  /// and `text`.
  bison::dynamic do_update_details(const bison::dynamic& args);

  /// @brief RMI method: report the result of a client-run `helm` command,
  /// shown in the relevant window's status label. @p args holds `command`
  /// (string), `ok` (bool), `output` (string, shown on failure), and an
  /// optional `scope` ("releases"/"repos"/"charts"/"history"/"install",
  /// default "releases") selecting which window's status label to write.
  /// Scope "install" answers `install_requested`: success closes the
  /// Install / Upgrade dialog, failure is shown inside it so the user can
  /// correct the input and retry.
  bison::dynamic do_command_result(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Console window's `helm`
  /// subprocess trace. @p args holds `command` (string, e.g. `"helm repo
  /// list"`), `exit_code` (int32), `ok` (bool) and `output` (string, a
  /// single-line preview). Color-coded green/red by `ok`; the table is
  /// FIFO-capped at kMaxConsoleRows.
  bison::dynamic do_append_command_log(const bison::dynamic& args);

  /// @brief RMI method: fill the Install / Upgrade dialog's Values box.
  /// @p args holds `token` (int32, echoed from `install_values_requested` --
  /// the call is discarded if the dialog was closed or reopened since),
  /// `text` (YAML) and an optional `error` (string, shown in the dialog).
  bison::dynamic do_set_install_values(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── List windows ─────────────────────────────────────────────────────

  /// One row of a list window: what the filters and row actions work on.
  struct entry {
    std::string scope; // "release" / "repo" / "chart" / "revision"
    std::string name; // release / repo / chart name
    std::string ns; // release namespace ("" for repos and charts)
    std::string state; // releases: status (the status Combo filters on it)
    std::string extra; // chart: version; revision: revision number
  };

  /// A list window plus its toolbar filters (not every window has every
  /// filter; an unset id never matches an event).
  struct list_window {
    common::list_panel<entry> panel;
    bison::key_t name_filter_id;
    bison::key_t ns_filter_id;
    bison::key_t status_combo_id; // Releases only.
    std::string name_filter;
    std::string ns_filter;
    int32_t status_filter{0}; // 0 All, 1 Deployed, 2 Failed, 3 Pending.
  };

  /// @brief Builds @p lw from @p layout_json at @p root_key, binding its
  /// filter widgets; @p wire binds the rest of its toolbar.
  void build_list_window(
      list_window& lw, const char* layout_json, const std::string& root_key,
      const std::function<void(ui_tree&)>& wire);

  /// @brief Re-applies @p lw's name / namespace / status filters.
  void apply_list_filter(list_window& lw);

  void rebuild_releases(const bison::dynamic& args);
  void rebuild_repos(const bison::dynamic& args);
  void rebuild_charts(const bison::dynamic& args);
  void rebuild_history(const bison::dynamic& args);

  /// @brief Points the Details window at a new target and emits
  /// `details_requested` for it.
  void open_details(
      const std::string& kind, const std::string& name, const std::string& ns, const std::string& version);
  void emit_details_request();
  void emit_history_request();

  /// @brief A row-menu item running @p action on @p e (see run_row_action()).
  common::menu_item action_item(const std::string& label, const entry& e, const std::string& action, bool confirm);
  /// @brief Menu-action dispatch for one row.
  void run_row_action(const entry& e, const std::string& action);

  // ── State ──────────────────────────────────────────────────────────
  list_window releases_;
  list_window repos_;
  list_window charts_;
  list_window history_;

  // Repositories "add" row.
  ui_element_ptr repo_name_input_;
  ui_element_ptr repo_url_input_;
  bison::key_t repo_name_input_id_;
  bison::key_t repo_url_input_id_;
  std::string repo_name_text_;
  std::string repo_url_text_;

  // Charts search box.
  bison::key_t chart_query_id_;
  std::string chart_query_text_;

  // More search results than this are not turned into table rows (a large
  // repository lists thousands of charts); the status line says so.
  static constexpr size_t kMaxChartRows = 500;

  // ── Install / Upgrade dialog ────────────────────────────────────────
  //
  // A floating (non-docked) Window built on demand by open_install_dialog()
  // and torn down by close_install_dialog(): unlike the dockable windows it
  // must appear in front when asked for, which a tab in a dock group cannot
  // do. At most one exists at a time.
  struct install_dialog {
    bool open{false};
    bool upgrade{false}; // false: `helm install`, true: `helm upgrade`
    bool busy{false};    // install_requested emitted, command_result pending
    bison::key_t window_id;
    bison::key_t chart_id, version_id, release_id, ns_id, values_id, create_ns_id, wait_id;
    ui_element_ptr values_input;
    ui_element_ptr status_label;
    std::string chart, version, release, ns, values;
    bool create_ns{true};
    bool wait{false};
    std::vector<bison::key_t> object_ids; // every element, erased on close
  };
  install_dialog install_;
  std::string install_root_key_;
  int32_t install_token_{0}; // bumped per open; guards set_install_values

  /// @brief (Re)open the dialog. @p upgrade selects the mode; the other
  /// arguments prefill its fields (an upgrade's release / namespace are
  /// read-only). Callable from on_event() (outside dispatch).
  void open_install_dialog(
      bool upgrade, const std::string& chart, const std::string& version, const std::string& release,
      const std::string& ns);
  void close_install_dialog();
  void set_install_status(const std::string& text, bool ok);
  /// @brief Validate the dialog's fields and emit `install_requested`.
  void submit_install();
  /// @brief Emit `install_values_requested` for @p source ("chart"/"release").
  void request_install_values(const std::string& source);

  // History window: the release whose revisions are listed.
  ui_element_ptr history_target_label_;
  std::string open_history_name_;
  std::string open_history_ns_;

  common::text_viewer_panel details_;
  std::string open_details_kind_;
  std::string open_details_name_;
  std::string open_details_ns_;
  std::string open_details_version_;

  common::console_panel console_;
};

/// @brief Register HelmFrontend in the "wish" bison namespace.
void register_helm();

} // namespace bdg::wish
