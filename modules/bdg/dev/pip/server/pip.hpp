// MIT License © 2026 Binary Dice Games
/// @file pip.hpp
/// @brief Server-side PipFrontend form -- a GUI over the local `pip` CLI.
///
/// All `pip` invocation and output parsing happens client-side (see
/// client/pip_source.hpp) -- the Python environment that matters is the one
/// on the user's own machine, reachable only from the client. This form only
/// renders whatever snapshot it was last given via its update_* RMI methods,
/// and emits high-level *_requested events the client reacts to by running
/// the corresponding `pip` command and pushing a fresh snapshot. Mirrors the
/// `helm` module's client/server split (modules/bdg/dev/helm/server/helm.hpp).
///
/// Destructive actions (uninstall / reinstall) are gated behind the built-in
/// MessageBox form (form::instantiate_child_form(), "yes_no" preset) -- see
/// show_confirm() below. Installs and upgrades fire directly.
///
/// The client runs `pip` on a worker thread; progress, live output and
/// Cancel for a long command are shown by the shared modal `ProgressBox`
/// form, which the client drives itself (common/command_worker.hpp) -- this
/// form has no part in it.
///
/// Owns four independently dockable Windows -- Packages (the main root),
/// Versions, Details, and Console (a FIFO-capped trace of every `pip` command
/// the client ran, fed by append_command_log).
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

/// @brief GUI form for the local `pip` CLI.
///
/// Emitted events:
///   - `"closed"` -- any window was closed; all four are torn down.
///   - `"refresh_requested"` -- no payload; the Packages Refresh button.
///   - `"outdated_requested"` -- no payload; "Check for updates" (the client
///     runs `pip list --outdated` and re-pushes the packages with `latest`).
///   - `"install_requested"` -- `{ spec, upgrade (bool), user (bool), pre
///     (bool) }`. `spec` is one or more whitespace-separated requirements
///     (`requests`, `requests==2.31.0`, `git+https://...`, a path on the
///     client machine).
///   - `"requirements_requested"` -- `{ path, upgrade, user, pre }`: install
///     from a requirements file on the client machine.
///   - `"package_action_requested"` -- `{ name, action }` where `action` is
///     `upgrade`, `reinstall` or `uninstall`.
///   - `"versions_requested"` -- `{ name, pre (bool) }`: list the versions
///     the package index offers. Answer with update_versions.
///   - `"details_requested"` -- `{ kind, name }` where `kind` is `show` /
///     `files` (package `name`) or `freeze` / `check` (`name == ""`). Answer
///     with update_details.
class pip_frontend : public form {
 public:
  explicit pip_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the Packages table. @p args.packages is a
  /// dynamic array, each `{ name, version, latest ("" = not known to be
  /// outdated), location ("" unless an editable install) }` (all strings).
  /// @p args.outdated_checked (bool) says whether `latest` was looked up at
  /// all -- the status line only counts outdated packages when it was.
  bison::dynamic do_update_packages(const bison::dynamic& args);

  /// @brief RMI method: replace the Versions table. @p args holds `name`
  /// (echoed from `versions_requested` -- the call is discarded if it no
  /// longer matches the window's open package), `latest` (string) and
  /// `versions` (a dynamic array of strings, newest first).
  bison::dynamic do_update_versions(const bison::dynamic& args);

  /// @brief RMI method: fill the Details window's text. @p args holds `kind`
  /// and `name` (echoed from `details_requested`; the call is discarded if
  /// they no longer match the window's open target), `title` and `text`.
  bison::dynamic do_update_details(const bison::dynamic& args);

  /// @brief RMI method: report the result of a client-run `pip` command,
  /// shown in the relevant window's status label. @p args holds `command`
  /// (string), `ok` (bool), `output` (string, shown on failure), and an
  /// optional `scope` ("packages"/"versions", default "packages") selecting
  /// which window's status label to write.
  bison::dynamic do_command_result(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Console window's `pip`
  /// subprocess trace. @p args holds `command` (string, e.g. `"pip list
  /// --format=json"`), `exit_code` (int32), `ok` (bool) and `output`
  /// (string, a single-line preview). Color-coded green/red by `ok`; the
  /// table is FIFO-capped at kMaxConsoleRows.
  bison::dynamic do_append_command_log(const bison::dynamic& args);

  /// @brief RMI method: set the Packages window's environment line.
  /// @p args holds `text` (`pip --version` output) and `interpreter` (the
  /// Python interpreter pip is run with).
  bison::dynamic do_set_environment(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── Generic list-window plumbing ──────────────────────────────────────
  //
  // Packages / Versions are two near-identical toolbar + Table windows. One
  // `list_window` bundles the per-window widgets; one `list_row` type + one
  // dispatch map (`menu_action_targets_`) serve both (helm.hpp's pattern).

  struct list_window {
    std::string root_key;
    bison::key_t window_id;
    ui_element_ptr status_label;
    ui_element_ptr table;
  };

  struct list_row {
    ui_element_ptr row;
    std::string scope;    // "package" / "version"
    std::string name;     // package name
    std::string version;  // package: installed version; version row: that version
    bool outdated{false}; // package rows: a newer version is known
    bool editable{false}; // package rows: an editable install
    size_t child_key{0};
    std::vector<bison::key_t> object_ids; // erased together on rebuild
  };

  struct row_action {
    std::string scope;
    std::string name;
    std::string version;
    std::string action;
  };

  struct menu_spec {
    std::string label; // empty -> a Separator
    std::string action;
    bool confirm{false}; // adds a "..." suffix; run_row_action() opens the MessageBox
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

  /// @brief Re-apply the Packages name filter and All / Outdated / Editable
  /// Combo to each row's `visible` field.
  void apply_package_filter();

  // ── Per-window rebuild ───────────────────────────────────────────────
  void rebuild_packages(const bison::dynamic& args);
  /// @brief Rebuild the Versions table from versions_ / versions_latest_,
  /// marking the version the Packages snapshot says is installed. Also run
  /// after every Packages rebuild so that mark follows an install.
  void rebuild_versions();
  /// @brief Installed version of @p name per the Packages snapshot ("" if
  /// not installed). Names compare PEP 503-normalized.
  std::string installed_version(const std::string& name) const;

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
  void open_details(const std::string& kind, const std::string& name);
  void emit_details_request();
  /// @brief Point the Versions window at @p name and emit
  /// `versions_requested` for it.
  void open_versions(const std::string& name);
  void emit_versions_request();
  /// @brief Emit `install_requested` / `requirements_requested` for
  /// @p value with the toolbar's current option checkboxes.
  void emit_install(bison::key_t event, bison::key_t field, const std::string& value);
  /// @brief Menu-action dispatch for one row (see on_event()).
  void run_row_action(const row_action& target);

  // ── Confirmation modal ──────────────────────────────────────────────
  void show_confirm(const std::string& message, std::function<void()> on_confirm);

  // ── Small builders ──────────────────────────────────────────────────
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);

  // ── State ──────────────────────────────────────────────────────────
  list_window packages_;
  list_window versions_window_;

  std::vector<list_row> package_rows_;
  std::vector<list_row> version_rows_;
  size_t next_package_key_{0};
  size_t next_version_key_{0};

  std::shared_ptr<message_box> confirm_dialog_;

  // Packages toolbar.
  ui_element_ptr env_label_;
  std::string packages_summary_; // the status line's last "N packages ..." text
  bison::key_t name_filter_id_;
  bison::key_t state_combo_id_;
  std::string name_filter_;
  int32_t state_filter_{0}; // 0 All, 1 Outdated, 2 Editable.

  // Packages install bars.
  ui_element_ptr spec_input_;
  bison::key_t spec_input_id_;
  bison::key_t req_input_id_;
  bison::key_t upgrade_id_, user_id_, pre_id_;
  std::string spec_text_;
  std::string req_text_;
  bool opt_upgrade_{false};
  bool opt_user_{false};
  bool opt_pre_{false};

  // Versions window: the package whose index versions are listed.
  ui_element_ptr versions_target_label_;
  std::string open_versions_name_;
  std::vector<std::string> versions_; // newest first, as last pushed
  std::string versions_latest_;

  // More index versions than this are not turned into table rows (some
  // packages have many hundreds of releases); the status line says so.
  static constexpr size_t kMaxVersionRows = 200;

  // Details window.
  std::string details_root_key_;
  bison::key_t details_window_id_;
  ui_element_ptr details_editor_;
  ui_element_ptr details_target_label_;
  std::string details_file_; // sandbox-relative file details_editor_ shows
  std::string open_details_kind_;
  std::string open_details_name_;

  std::filesystem::path resource_dir_; // session sandbox root (set in on_init())
  size_t next_details_file_seq_{0};    // makes each set_details_text() file name unique

  // ── Console window (client `pip` subprocess trace) ──────────────────
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

/// @brief Register PipFrontend in the "wish" bison namespace.
void register_pip();

} // namespace bdg::wish
