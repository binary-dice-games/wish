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
/// Destructive actions (uninstall / rollback / repo remove) are gated behind
/// the built-in MessageBox form (form::instantiate_child_form(), "yes_no"
/// preset) -- see show_confirm() below. Install / upgrade go through their
/// own dialog instead, whose submit button is the confirmation.
///
/// Owns six independently dockable Windows -- Releases (the main root),
/// Repositories, Charts, History, Details, and Console (a FIFO-capped trace
/// of every `helm` command the client ran, fed by append_command_log) --
/// plus a floating Install / Upgrade dialog created on demand.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class message_box;

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
class helm_frontend : public form {
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
  // ── Generic list-window plumbing ──────────────────────────────────────
  //
  // Releases / Repositories / Charts / History are four near-identical
  // toolbar + Table windows. One `list_window` bundles the per-window
  // widgets; one `list_row` type + one dispatch map (`menu_action_targets_`)
  // serve all four (kubectl.hpp's pattern).

  struct list_window {
    std::string root_key;
    bison::key_t window_id;
    ui_element_ptr status_label;
    ui_element_ptr table;

    // Toolbar filter widgets (not every window has every one).
    bison::key_t name_filter_id;
    bison::key_t ns_filter_id;
    bison::key_t status_combo_id; // Releases only.

    std::string name_filter;
    std::string ns_filter;
    int32_t status_filter{0}; // 0 All, 1 Deployed, 2 Failed, 3 Pending.
  };

  struct list_row {
    ui_element_ptr row;
    std::string scope; // "release" / "repo" / "chart" / "revision"
    std::string name;  // release / repo / chart name
    std::string ns;    // release namespace ("" for repos and charts)
    std::string state; // releases: status (the status Combo filters on it)
    std::string extra; // chart: version; revision: revision number
    size_t child_key{0};
    std::vector<bison::key_t> object_ids; // erased together on rebuild
  };

  struct row_action {
    std::string scope;
    std::string name;
    std::string ns;
    std::string extra;
    std::string action;
  };

  struct menu_spec {
    std::string label; // empty -> a Separator
    std::string action;
    bool confirm{false}; // adds a "..." suffix; on_event() opens the MessageBox
  };

  /// @brief Import @p layout_json, register every node, cache the status /
  /// table widgets, and register the tree as its own dockable top-level root
  /// at @p root_key. @p wire binds that window's own toolbar widgets.
  void build_list_window(
      list_window& lw, const char* layout_json, const std::string& root_key,
      const std::function<void(ui_tree&)>& wire);

  void clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key);

  void add_list_row(
      list_window& lw, std::vector<list_row>& rows, size_t& next_key, list_row&& meta,
      const std::vector<ui_element_ptr>& cells, const std::vector<menu_spec>& items);

  void set_status(list_window& lw, const std::string& text, bool ok);

  /// @brief Re-apply a list window's name / namespace / status filters to
  /// each row's `visible` field.
  void apply_list_filter(list_window& lw, std::vector<list_row>& rows);

  // ── Per-window rebuild ───────────────────────────────────────────────
  void rebuild_releases(const bison::dynamic& args);
  void rebuild_repos(const bison::dynamic& args);
  void rebuild_charts(const bison::dynamic& args);
  void rebuild_history(const bison::dynamic& args);

  // ── Details / Console windows ───────────────────────────────────────
  /// @brief Build a toolbar + body window (Details, Console) from
  /// @p layout_json and register it as a top-level object under @p root_key.
  void build_text_window(
      const std::string& root_key, const char* layout_json, bison::key_t& window_id_out,
      const std::function<void(ui_tree&)>& wire);
  /// @brief Show @p text in the Details `TextEditor`: write it to a new file
  /// `private/<root>_details_<n>.txt` in the session sandbox, point the
  /// editor's `file_path` at it and delete the file it replaced. A write
  /// failure leaves the editor unchanged.
  void set_details_text(const std::string& text);
  /// @brief Delete the sandbox file the Details editor shows (if any).
  void remove_details_file();

  /// @brief Point the Details window at a new target and emit
  /// `details_requested` for it.
  void open_details(
      const std::string& kind, const std::string& name, const std::string& ns, const std::string& version);
  void emit_details_request();
  void emit_history_request();
  /// @brief Menu-action dispatch for one row (see on_event()).
  void run_row_action(const row_action& target);

  // ── Confirmation modal ──────────────────────────────────────────────
  void show_confirm(const std::string& message, std::function<void()> on_confirm);

  // ── Small builders ──────────────────────────────────────────────────
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);

  // ── State ──────────────────────────────────────────────────────────
  list_window releases_;
  list_window repos_;
  list_window charts_;
  list_window history_;

  std::vector<list_row> release_rows_;
  std::vector<list_row> repo_rows_;
  std::vector<list_row> chart_rows_;
  std::vector<list_row> history_rows_;
  size_t next_release_key_{0};
  size_t next_repo_key_{0};
  size_t next_chart_key_{0};
  size_t next_history_key_{0};

  std::shared_ptr<message_box> confirm_dialog_;

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

  // Details window.
  std::string details_root_key_;
  bison::key_t details_window_id_;
  ui_element_ptr details_editor_;
  ui_element_ptr details_target_label_;
  std::string details_file_; // sandbox-relative file details_editor_ shows
  std::string open_details_kind_;
  std::string open_details_name_;
  std::string open_details_ns_;
  std::string open_details_version_;

  std::filesystem::path resource_dir_; // session sandbox root (set in on_init())
  size_t next_details_file_seq_{0};    // makes each set_details_text() file name unique

  // ── Console window (client `helm` subprocess trace) ─────────────────
  //
  // A FIFO-capped `Table` (# / Command / Exit / Output) so a long session
  // stays bounded rather than growing without limit.
  std::string console_root_key_;
  bison::key_t console_window_id_;
  ui_element_ptr console_table_;

  static constexpr size_t kMaxConsoleRows = 500;

  struct console_row_entry {
    size_t child_key;
    std::vector<bison::key_t> object_ids;
  };
  size_t console_seq_{0};
  size_t next_console_child_key_{0};
  std::deque<console_row_entry> console_rows_; // oldest first

  /// @brief Append one trace row (sequence #, command, exit code, output
  /// preview), green/red by @p ok; evict the oldest first past kMaxConsoleRows.
  void append_console_row(const std::string& command, int32_t exit_code, bool ok, const std::string& output);
  /// @brief Erase every Console row (+ their ctx().objects / click_handlers_
  /// entries); reset the sequence counter. From any row's "Clear Console".
  void clear_console_rows();
  void erase_console_row_objects(const console_row_entry& entry);

  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
  std::unordered_map<bison::key_t, row_action, bison::key_t, bison::key_t> menu_action_targets_;
};

/// @brief Register HelmFrontend in the "wish" bison namespace.
void register_helm();

} // namespace bdg::wish
