// MIT License © 2026 Binary Dice Games
/// @file pkg.hpp
/// @brief Server-side PkgFrontend form -- one GUI over the system package
///        managers (apt, dnf, pacman, brew).
///
/// The form knows nothing about any particular package manager: which one is
/// in use, what each operation runs and how its output is read all live
/// client-side (see client/pkg_backend.hpp) -- the packages that matter are
/// the ones on the user's own machine, reachable only from the client. This
/// form only renders whatever snapshot it was last given via its update_* RMI
/// methods, and emits high-level *_requested events the client reacts to by
/// running the corresponding command and pushing a fresh snapshot. Mirrors
/// the `pip` module's client/server split
/// (modules/bdg/dev/pip/server/pip.hpp).
///
/// Built on the panels shared by the bdg tool forms
/// (modules/bdg/common/server): destructive actions (remove / reinstall /
/// upgrade all) are gated behind tool_form::show_confirm() -- installs and
/// single upgrades fire directly; Packages and Search are
/// common::list_panel; Details is common::text_viewer_panel; Console is
/// common::console_panel. Progress, live output and Cancel for a long
/// command are shown by the shared modal `ProgressBox` form, which the
/// client drives itself (modules/bdg/common/command_worker.hpp).
///
/// Owns four independently dockable Windows -- Packages (the main root),
/// Search, Details, and Console (a FIFO-capped trace of every command the
/// client ran, fed by append_command_log).
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

/// @brief GUI form for a system package manager.
///
/// Emitted events:
///   - `"closed"` -- any window was closed; all four are torn down.
///   - `"refresh_requested"` -- no payload; the Packages Refresh button.
///   - `"outdated_requested"` -- no payload; "Check for updates" (re-push the
///     packages with `latest` filled in, from the index as last refreshed).
///   - `"index_requested"` -- no payload; "Update index" (refresh the package
///     index from the repositories, then the outdated list).
///   - `"upgrade_all_requested"` -- no payload; "Upgrade all", confirmed.
///   - `"search_requested"` -- `{ query }`: search the package index. Answer
///     with update_search.
///   - `"install_requested"` -- `{ names }`: one or more whitespace-separated
///     package names.
///   - `"package_action_requested"` -- `{ name, action }` where `action` is
///     `upgrade`, `reinstall` or `remove`.
///   - `"details_requested"` -- `{ kind, name }` where `kind` is `show` (the
///     package's description) or `files` (the files it installed). Answer
///     with update_details.
class pkg_frontend : public common::tool_form {
 public:
  explicit pkg_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the installed-packages snapshot.
  /// @p args.packages is a dynamic array, each `{ name, version, latest ("" =
  /// not known to be outdated), description }` (all strings).
  /// @p args.outdated_checked (bool) says whether `latest` was looked up at
  /// all -- the status line only counts upgradable packages when it was.
  bison::dynamic do_update_packages(const bison::dynamic& args);

  /// @brief RMI method: replace the Search table. @p args holds `query`
  /// (echoed from `search_requested` -- the call is discarded if it no
  /// longer matches the window's open search) and `results` -- each `{ name,
  /// version ("" if the manager's search reports none), description }`.
  bison::dynamic do_update_search(const bison::dynamic& args);

  /// @brief RMI method: fill the Details window's text. @p args holds `kind`
  /// and `name` (echoed from `details_requested`; the call is discarded if
  /// they no longer match the window's open target), `title` and `text`.
  bison::dynamic do_update_details(const bison::dynamic& args);

  /// @brief RMI method: report the result of a client-run command, shown in
  /// the relevant window's status label. @p args holds `command` (string),
  /// `ok` (bool), `output` (string, shown on failure), and an optional
  /// `scope` ("packages"/"search", default "packages") selecting which
  /// window's status label to write.
  bison::dynamic do_command_result(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Console window's subprocess
  /// trace. @p args holds `command` (string), `exit_code` (int32), `ok`
  /// (bool) and `output` (string, a single-line preview). Color-coded
  /// green/red by `ok`; the table is FIFO-capped at kMaxConsoleRows.
  bison::dynamic do_append_command_log(const bison::dynamic& args);

  /// @brief RMI method: set the Packages window's environment line.
  /// @p args holds `manager` (e.g. `"apt"`), `text` (its version line) and
  /// `elevation` (`"none"` / `"sudo"` / `"pkexec"`).
  bison::dynamic do_set_environment(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── List windows ─────────────────────────────────────────────────────

  /// One row of Packages / Search: what the row actions work on.
  struct package_row {
    std::string scope; // "package" / "result"
    std::string name; // package name
  };
  using list_window = common::list_panel<package_row>;

  /// @brief One package of a snapshot (see do_update_packages / _search).
  struct entry {
    std::string name;
    std::string version;
    std::string latest;
    std::string description;
  };

  /// @brief Writes @p lw's status label, one line only: a multi-line error
  /// would grow the label and shift the table below it. The whole message is
  /// in the progress dialog and Console.
  static void set_status(list_window& lw, const std::string& text, bool ok) {
    lw.set_status(text, ok, /*first_line_only=*/true);
  }

  /// @brief Rebuild the Packages table from installed_: the entries passing
  /// the name filter and the All / Upgradable Combo, up to kMaxRows.
  void rebuild_package_rows();
  /// @brief Rebuild the Search table from results_, up to kMaxRows, marking
  /// the packages the installed snapshot holds.
  void rebuild_search_rows();
  /// @brief Installed version of @p name per the installed snapshot ("" if
  /// not installed). Compared without dpkg's `:arch` qualifier.
  std::string installed_version(const std::string& name) const;

  /// @brief Point the Details window at a new target and emit
  /// `details_requested` for it.
  void open_details(const std::string& kind, const std::string& name);
  void emit_details_request();
  /// @brief Point the Search window at @p query and emit `search_requested`.
  void open_search(const std::string& query);
  void emit_search_request();
  /// @brief Emit `install_requested` for @p names.
  void emit_install(const std::string& names);
  /// @brief A row-menu item running @p action on @p r (see run_row_action()).
  common::menu_item
  action_item(const std::string& label, const package_row& r, const std::string& action, bool confirm);
  /// @brief Menu-action dispatch for one row.
  void run_row_action(const package_row& r, const std::string& action);

  // ── State ──────────────────────────────────────────────────────────
  list_window packages_;
  list_window search_;

  // The last snapshots. A system has thousands of packages; turning each
  // into a table row (a dozen UI elements, walked every frame) would make
  // the whole UI crawl, so rows are only built for the first kMaxRows
  // entries that pass the filter -- the status line says when more exist.
  std::vector<entry> installed_;
  std::vector<entry> results_;
  bool outdated_checked_{false};
  static constexpr size_t kMaxRows = 300;

  // Packages toolbar.
  ui_element_ptr env_label_;
  std::string packages_summary_; // the status line's last "N packages ..." text
  bison::key_t name_filter_id_;
  bison::key_t state_combo_id_;
  bison::key_t query_input_id_;
  std::string name_filter_;
  int32_t state_filter_{0}; // 0 All, 1 Upgradable.
  std::string query_text_;  // the "Find new packages" box

  // Search window: the query whose results are listed.
  ui_element_ptr search_target_label_;
  std::string open_search_query_;

  common::text_viewer_panel details_;
  std::string open_details_kind_;
  std::string open_details_name_;

  common::console_panel console_;
};

/// @brief Register PkgFrontend in the "wish" bison namespace.
void register_pkg();

} // namespace bdg::wish
