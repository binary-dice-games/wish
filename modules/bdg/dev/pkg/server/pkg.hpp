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
/// Destructive actions (remove / reinstall / upgrade all) are gated behind
/// the built-in MessageBox form (form::instantiate_child_form(), "yes_no"
/// preset) -- see show_confirm() below. Installs and single upgrades fire
/// directly. Progress, live output and Cancel for a long command are shown by
/// the shared modal `ProgressBox` form, which the client drives itself
/// (modules/common/command_worker.hpp).
///
/// Owns four independently dockable Windows -- Packages (the main root),
/// Search, Details, and Console (a FIFO-capped trace of every command the
/// client ran, fed by append_command_log).
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
class pkg_frontend : public form {
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
  // ── Generic list-window plumbing ──────────────────────────────────────
  //
  // Packages / Search are two near-identical toolbar + Table windows. One
  // `list_window` bundles the per-window widgets; one `list_row` type + one
  // dispatch map (`menu_action_targets_`) serve both (pip.hpp's pattern).

  struct list_window {
    std::string root_key;
    bison::key_t window_id;
    ui_element_ptr status_label;
    ui_element_ptr table;
  };

  struct list_row {
    ui_element_ptr row;
    std::string scope; // "package" / "result"
    std::string name;  // package name
    size_t child_key{0};
    std::vector<bison::key_t> object_ids; // erased together on rebuild
  };

  struct row_action {
    std::string scope;
    std::string name;
    std::string action;
  };

  struct menu_spec {
    std::string label; // empty -> a Separator
    std::string action;
    bool confirm{false}; // adds a "..." suffix; run_row_action() opens the MessageBox
  };

  /// @brief One package of a snapshot (see do_update_packages / _search).
  struct entry {
    std::string name;
    std::string version;
    std::string latest;
    std::string description;
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

  // ── Per-window rebuild ───────────────────────────────────────────────
  /// @brief Rebuild the Packages table from installed_: the entries passing
  /// the name filter and the All / Upgradable Combo, up to kMaxRows.
  void rebuild_package_rows();
  /// @brief Rebuild the Search table from results_, up to kMaxRows, marking
  /// the packages the installed snapshot holds.
  void rebuild_search_rows();
  /// @brief Installed version of @p name per the installed snapshot ("" if
  /// not installed). Compared without dpkg's `:arch` qualifier.
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
  /// @brief Point the Search window at @p query and emit `search_requested`.
  void open_search(const std::string& query);
  void emit_search_request();
  /// @brief Emit `install_requested` for @p names.
  void emit_install(const std::string& names);
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
  list_window search_;

  // The last snapshots. A system has thousands of packages; turning each
  // into a table row (a dozen UI elements, walked every frame) would make
  // the whole UI crawl, so rows are only built for the first kMaxRows
  // entries that pass the filter -- the status line says when more exist.
  std::vector<entry> installed_;
  std::vector<entry> results_;
  bool outdated_checked_{false};
  static constexpr size_t kMaxRows = 300;

  std::vector<list_row> package_rows_;
  std::vector<list_row> search_rows_;
  size_t next_package_key_{0};
  size_t next_search_key_{0};

  std::shared_ptr<message_box> confirm_dialog_;

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

  // ── Console window (client subprocess trace) ────────────────────────
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

/// @brief Register PkgFrontend in the "wish" bison namespace.
void register_pkg();

} // namespace bdg::wish
