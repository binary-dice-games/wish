// MIT License © 2026 Binary Dice Games
/// @file sq.hpp
/// @brief Server-side SqFrontend form -- a DBeaver-style, query-only GUI
///        over the local `sq` (https://github.com/neilotoole/sq) CLI.
///
/// All `sq` invocation and output parsing happens client-side (see
/// client/sq_source.hpp). This form only renders whatever snapshot it was
/// last given via its update_* RMI methods and emits high-level
/// *_requested events the client reacts to -- the same split as the
/// docker/kubectl/curl modules.
///
/// Owns six independently dockable Windows: SQL Editor (the main root:
/// active-database picker, a SQL-highlighting TextEditor, Run), Results (Export CSV
/// plus a dynamic-column grid), Connections (sq sources + an inline "add" form),
/// Navigator (a tree of the active database's tables/views/columns),
/// Structure (the selected table's columns, keys and references) and
/// Console (a trace of every `sq` command run).
///
/// The module is query-only: no event or RMI method here can request a
/// data change (the client additionally rejects non-SELECT SQL, see
/// client/sq_query_guard.hpp).
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

/// @brief DBeaver-style query GUI form for the local `sq` CLI.
///
/// Emitted events:
///   - `"closed"` -- any window was closed; everything is torn down.
///   - `"refresh_requested"` -- no payload; re-list connections and
///     re-inspect the active database.
///   - `"activate_requested"` -- `{ handle }` make @p handle the active
///     database (Connections row action or the editor's picker).
///   - `"ping_requested"` -- `{ handle }`.
///   - `"remove_connection_requested"` -- `{ handle }` (confirmed first;
///     only unregisters the source from sq, the database is untouched).
///   - `"add_connection_requested"` -- `{ handle, location, driver,
///     password }` -- all strings, any may be empty (`driver` "" = auto).
///   - `"query_requested"` -- `{ sql, max_rows }` run @p sql, display at
///     most @p max_rows rows.
///   - `"export_requested"` -- `{ path, overwrite }` re-run the last
///     executed query in full and write it to @p path as CSV.
class sq_frontend : public form {
 public:
  explicit sq_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the Connections table and the editor's
  /// active-database picker. @p args holds `active` (handle, may be empty)
  /// and `entries` -- an array of `{ handle, driver, location }` (the
  /// location already has any password masked).
  bison::dynamic do_update_connections(const bison::dynamic& args);

  /// @brief RMI method: set the driver choices of the "add connection"
  /// form. @p args.drivers is an array of `{ type, description }`.
  bison::dynamic do_update_drivers(const bison::dynamic& args);

  /// @brief RMI method: replace the Navigator tree and Structure data.
  /// @p args holds `handle`, `driver`, `product`, an optional `error`
  /// (shown instead of the tree), and `tables` -- an array of
  /// `{ name, type ("table"/"view"), rows (int32, -1 = unknown), columns }`
  /// where each column is `{ name, type, pk (bool), nullable (bool), fk }`
  /// (`fk` is "" or e.g. `artist(id)`). An empty `handle` clears both.
  bison::dynamic do_update_schema(const bison::dynamic& args);

  /// @brief RMI method: display a query result. @p args holds `ok`,
  /// `error` (when !ok), `sql`, `elapsed_ms` (int32), `total_rows` (int32),
  /// `truncated` (bool), `columns` (array of `{ name }`) and `rows` (array
  /// of string arrays, one per row, one cell per column). A NULL cell is
  /// sent as the one-character string "\x01" (see kNullCell).
  bison::dynamic do_update_result(const bison::dynamic& args);

  /// @brief RMI method: show a message in a window's status label.
  /// @p args holds `scope` ("editor"/"results"/"connections"), `ok` and `message`.
  bison::dynamic do_command_result(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Console trace. @p args holds
  /// `command`, `exit_code` (int32), `ok` and `output` (single-line).
  bison::dynamic do_append_command_log(const bison::dynamic& args);

  /// @brief RMI method: tell the user `sq` is not installed. @p args holds
  /// `message` and `url`. Shows a dialog plus a persistent banner in the
  /// editor and Connections windows.
  bison::dynamic do_show_unavailable(const bison::dynamic& args);

  /// @brief The wire encoding of a SQL NULL cell in do_update_result.
  static constexpr const char* kNullCell = "\x01";

  /// @brief Quotes @p name as a SQL identifier for @p driver (backticks for
  /// MySQL/MariaDB, brackets for SQL Server, double quotes otherwise).
  static std::string quote_identifier(const std::string& driver, const std::string& name);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct column_info {
    std::string name;
    std::string type;
    bool pk{false};
    bool nullable{true};
    std::string fk;
  };
  struct table_info {
    std::string name;
    std::string type;
    int32_t rows{-1};
    std::vector<column_info> columns;
  };

  // ── Construction helpers ─────────────────────────────────────────────
  /// @brief Import @p layout_json, assign ids, register it as its own
  /// dockable top-level root at @p root_key (the main root is registered by
  /// form::init() itself); @p wire looks widgets up in the imported tree.
  void build_window(
      const char* layout_json, const std::string& root_key, bison::key_t& window_id_out,
      const std::function<void(ui_tree&)>& wire);
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);
  void show_confirm(const std::string& message, std::function<void()> on_confirm);
  void set_status(const ui_element_ptr& label, const std::string& text, bool ok);

  /// @brief Forget @p ids in ctx().objects / click_handlers_ and clear them.
  void release_ids(std::vector<bison::key_t>& ids);
  /// @brief Replace @p table's dynamic TableRow children with @p rows,
  /// keeping its JSON-declared TableColumn children. @p ids are released.
  void replace_rows(const ui_element_ptr& table, std::vector<bison::key_t>& ids, const std::vector<ui_element_ptr>& rows);
  /// @brief Build one TableRow of Label cells, recording every id in @p ids.
  ui_element_ptr make_row(
      std::vector<bison::key_t>& ids, const std::vector<ui_element_ptr>& cells,
      const std::vector<std::pair<std::string, std::function<void()>>>& menu = {});

  // ── Per-window rebuild ───────────────────────────────────────────────
  void rebuild_connections(const bison::dynamic& args);
  void rebuild_navigator(const std::string& heading);
  void show_structure(size_t table_index);
  void append_console_row(const std::string& command, int32_t exit_code, bool ok, const std::string& output);
  void emit_query(const std::string& sql);
  /// @brief The editor's current text (read from its sandbox file).
  std::string read_sql();
  /// @brief Replace the editor's text (writes a new sandbox file and
  /// points the TextEditor at it).
  void set_sql_text(const std::string& sql);

  // ── State ────────────────────────────────────────────────────────────
  bison::key_t editor_window_id_;
  bison::key_t connections_window_id_;
  bison::key_t navigator_window_id_;
  bison::key_t structure_window_id_;
  bison::key_t results_window_id_;
  bison::key_t console_window_id_;
  std::string connections_root_key_;
  std::string navigator_root_key_;
  std::string structure_root_key_;
  std::string results_root_key_;
  std::string console_root_key_;

  // Editor.
  ui_element_ptr sql_input_; ///< the TextEditor
  size_t sql_file_seq_{0};
  std::filesystem::path resource_dir_;  ///< session sandbox, copied in on_init()
  bool allow_absolute_paths_{false};
  ui_element_ptr conn_combo_;
  bison::key_t conn_combo_id_;
  ui_element_ptr rows_combo_;
  ui_element_ptr export_path_input_;
  ui_element_ptr overwrite_checkbox_;
  ui_element_ptr editor_status_;

  // Connections.
  ui_element_ptr connections_status_;
  ui_element_ptr connections_table_;
  ui_element_ptr driver_combo_;
  ui_element_ptr handle_input_;
  ui_element_ptr location_input_;
  ui_element_ptr password_input_;
  std::vector<std::string> driver_types_; ///< parallel to the driver combo (0 = auto)
  std::vector<std::string> handles_;      ///< parallel to the editor's picker
  std::vector<bison::key_t> connection_ids_;
  std::string active_handle_;
  std::string active_driver_;

  // Navigator / Structure.
  ui_element_ptr nav_box_;
  std::vector<bison::key_t> nav_ids_;
  std::vector<table_info> tables_;
  std::string schema_handle_;
  std::string schema_product_;
  ui_element_ptr structure_title_;
  ui_element_ptr structure_table_;
  std::vector<bison::key_t> structure_ids_;

  // Results.
  ui_element_ptr results_status_;
  ui_element_ptr results_table_;
  std::vector<bison::key_t> result_ids_;
  size_t result_seq_{0};

  // Console.
  ui_element_ptr console_table_;
  static constexpr size_t kMaxConsoleRows = 500;
  struct console_row_entry {
    size_t child_key;
    std::vector<bison::key_t> object_ids;
  };
  size_t console_seq_{0};
  size_t next_console_child_key_{0};
  std::deque<console_row_entry> console_rows_;

  std::shared_ptr<message_box> dialog_;

  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
};

/// @brief Register SqFrontend in the "wish" bison namespace.
void register_sq();

} // namespace bdg::wish
