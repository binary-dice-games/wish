// MIT License © 2026 Binary Dice Games
/// @file find.hpp
/// @brief Server-side form for find (UI only, no local file access).
#pragma once

#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Instant file search in the spirit of FSearch
/// (https://github.com/cboxdoerfer/fsearch), over the *client's* files.
///
/// One window, laid out like FSearch's: a menu bar (File, Edit, View,
/// Search, Help), the search field with the filter drop-down on its right,
/// the results table (Name, Path, Type, Size, Date Modified -- sortable,
/// resizable, reorderable; right-click a header to hide a column), and a
/// status bar (match count, selection, the active search modes, the filter
/// and the size of the database).
///
/// Like du, this form has **no filesystem access**: the client indexes its
/// own folders into an in-memory database and runs every query. The form
/// owns the search state (query, filter, modes, sort) and the selection:
///
///   - `"on_search"` (`{query, filter, match_case, search_in_path, regex,
///     sort_column, ascending, generation}`) -- the query, the filter, a
///     search mode or the sort changed (or search_again() was called). The
///     client answers with `show_results()`, echoing `generation`; results
///     for an older generation are dropped.
///   - `"on_update_database"` -- File > Update Database: the client
///     re-indexes, reporting through `set({"indexing", "status",
///     "db_files", "db_folders"})`, then calls search_again().
///   - `"on_open"` / `"on_open_folder"` (`{paths}`: newline-separated full
///     paths) -- double click on a result, File > Open / Open Folder, or a
///     row's context menu. The client opens them with the system's default
///     application (or the folder containing each one).
///   - `"closed"` -- the window's X button or File > Close.
///
/// The search modes and the View toggles are remembered in the user store
/// (find_settings.hpp).
class find_form : public common::tool_form {
 public:
  explicit find_form(bison::dynamic&& base);
  ~find_form() override;

  /// @brief RMI method: display the results of a search.
  ///
  /// @p args holds:
  ///   - `generation` (int32): the `on_search` event answered;
  ///   - `total`, `folders` (int32): the number of matches, and of matching
  ///     folders;
  ///   - `entries`: dynamic array of `{name, path, type, size, modified,
  ///     dir}` -- the first matches in display order (`path` is the
  ///     containing folder; `type` may be a translatable message; `size`
  ///     and `modified` are display strings; `dir` is a bool);
  ///   - `error` (string, optional): why the query is invalid.
  bison::dynamic do_show_results(const bison::dynamic& args);

  /// @brief RMI method: emit `on_search` again for the current state (the
  /// client calls it once its database is ready).
  bison::dynamic do_search_again();

  /// @brief Called from the `__setter` prototype method for every set()
  /// call. Mirrors `indexing`, `status`, `db_files` and `db_folders` into the
  /// status bar.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct result_row {
    std::string name;
    std::string path; ///< Containing folder.
    std::string type;
    std::string size;
    std::string modified;
    bool dir{false};

    std::string full_path() const;
  };

  enum class row_action { open, open_folder, copy };

  /// @brief Emit `on_search` for the current state, with a new generation.
  void request_search();
  /// @brief Rebuild the table from `rows_` (with each row's context menu).
  void fill_table();
  /// @brief Push `selected_` into the rows' `selected` flags, the Edit
  /// menu's copy texts and the status bar.
  void apply_selection();
  /// @brief Rows of the selection, in display order.
  std::vector<const result_row*> selected_rows() const;
  /// @brief Emit `on_open` / `on_open_folder` for @p rows.
  void open_rows(const std::vector<const result_row*>& rows, bool folder);
  /// @brief Refresh the match count / indexing message on the left of the
  /// status bar.
  void update_status();
  /// @brief Refresh the search-mode indicators, the filter label and the
  /// Search / View menus' check marks.
  void update_indicators();
  /// @brief Apply the View toggles.
  void update_view();
  void toggle_mode(bool& mode, const settings::setting_info& info);
  /// @brief Show the "Search Syntax" help dialog.
  void show_syntax_help();

  bison::key_t window_id_;
  bison::key_t query_id_;
  bison::key_t filter_id_;
  bison::key_t table_id_;

  ui_element_ptr query_ptr_;
  ui_element_ptr filter_ptr_;
  ui_element_ptr table_ptr_;
  ui_element_ptr statusbar_ptr_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr selection_label_ptr_;
  ui_element_ptr case_label_ptr_;
  ui_element_ptr path_label_ptr_;
  ui_element_ptr regex_label_ptr_;
  ui_element_ptr filter_label_ptr_;
  ui_element_ptr db_label_ptr_;

  ui_element_ptr menu_match_case_ptr_;
  ui_element_ptr menu_search_in_path_ptr_;
  ui_element_ptr menu_regex_ptr_;
  ui_element_ptr menu_statusbar_ptr_;
  ui_element_ptr menu_filter_ptr_;
  ui_element_ptr menu_copy_name_ptr_;
  ui_element_ptr menu_copy_path_ptr_;
  ui_element_ptr menu_copy_full_path_ptr_;

  // Search state.
  std::string query_;
  std::int32_t filter_{0};
  bool match_case_{false};
  bool search_in_path_{false};
  bool regex_{false};
  std::int32_t sort_column_{0};
  bool ascending_{true};
  std::int32_t generation_{0};

  // View state.
  bool show_statusbar_{true};
  bool show_filter_{true};

  // Results and database state.
  std::vector<result_row> rows_;
  std::int32_t total_{0};
  std::int32_t total_folders_{0};
  std::string error_;
  bool has_results_{false};
  bool indexing_{false};
  std::string index_status_;
  std::int32_t db_files_{-1}; ///< -1 until the first database is ready.
  std::int32_t db_folders_{0};

  // Selection, as indexes into rows_.
  std::set<std::int32_t> selected_;
  std::int32_t anchor_{-1};

  // Row context menu items -> what they do; erased on every rebuild.
  std::unordered_map<bison::key_t, std::pair<row_action, std::int32_t>, bison::key_t, bison::key_t> row_menu_;
  std::vector<bison::key_t> row_object_ids_;
};

/// @brief Register Find in the "wish" bison namespace.
void register_find();

} // namespace bdg::wish
