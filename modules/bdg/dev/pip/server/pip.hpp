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
/// Built on the panels shared by the bdg tool forms
/// (modules/bdg/common/server): destructive actions (uninstall / reinstall)
/// are gated behind tool_form::show_confirm() -- installs and upgrades fire
/// directly; Packages and Versions are common::list_panel; Details is
/// common::text_viewer_panel; Console is common::console_panel.
///
/// The client runs `pip` on a worker thread; progress, live output and
/// Cancel for a long command are shown by the shared modal `ProgressBox`
/// form, which the client drives itself (modules/bdg/common/command_worker.hpp) -- this
/// form has no part in it.
///
/// Owns four independently dockable Windows -- Packages (the main root),
/// Versions, Details, and Console (a FIFO-capped trace of every `pip` command
/// the client ran, fed by append_command_log).
#pragma once

#include "modules/bdg/common/server/console_panel.hpp"
#include "modules/bdg/common/server/list_panel.hpp"
#include "modules/bdg/common/server/text_viewer_panel.hpp"
#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish {

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
class pip_frontend : public common::tool_form {
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
  // ── List windows ─────────────────────────────────────────────────────

  /// One row of Packages / Versions: what the filter and row actions work on.
  struct package_row {
    std::string scope;    // "package" / "version"
    std::string name;     // package name
    std::string version;  // package: installed version; version row: that version
    bool outdated{false}; // package rows: a newer version is known
    bool editable{false}; // package rows: an editable install
  };
  using list_window = common::list_panel<package_row>;

  /// @brief Writes @p lw's status label, one line only: a multi-line pip
  /// error would grow the label and shift the table below it. The whole
  /// message is in the progress dialog and Console.
  static void set_status(list_window& lw, const std::string& text, bool ok) {
    lw.set_status(text, ok, /*first_line_only=*/true);
  }

  /// @brief Re-apply the Packages name filter and All / Outdated / Editable
  /// Combo to each row's `visible` field.
  void apply_package_filter();

  void rebuild_packages(const bison::dynamic& args);
  /// @brief Rebuild the Versions table from versions_ / versions_latest_,
  /// marking the version the Packages snapshot says is installed. Also run
  /// after every Packages rebuild so that mark follows an install.
  void rebuild_versions();
  /// @brief Installed version of @p name per the Packages snapshot ("" if
  /// not installed). Names compare PEP 503-normalized.
  std::string installed_version(const std::string& name) const;

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
  /// @brief A row-menu item running @p action on @p r (see run_row_action()).
  common::menu_item
  action_item(const std::string& label, const package_row& r, const std::string& action, bool confirm);
  /// @brief Menu-action dispatch for one row.
  void run_row_action(const package_row& r, const std::string& action);

  // ── State ──────────────────────────────────────────────────────────
  list_window packages_;
  list_window versions_window_;

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

  // Details window. Its files go under "private/": `pip freeze` can list
  // private index / VCS URLs.
  common::text_viewer_panel details_;
  std::string open_details_kind_;
  std::string open_details_name_;

  common::console_panel console_;
};

/// @brief Register PipFrontend in the "wish" bison namespace.
void register_pip();

} // namespace bdg::wish
