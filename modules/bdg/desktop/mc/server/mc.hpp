// MIT License © 2025 Binary Dice Games
/// @file mc.hpp
/// @brief Server-side form for mc (a two-panel file browser + transfer UI).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class properties_dialog;

/// @brief Two-panel file browser: local machine (left) vs. session sandbox
/// (right), with upload/download transfer buttons and a progress bar.
///
/// The right panel is entirely server-owned: the session sandbox
/// (`context::resource_dir`) lives on the same machine as this form, so
/// navigation, listing, and the "Open in Explorer" button are all handled
/// here directly via `std::filesystem` + `file_service::resolve_path()`.
///
/// The left panel shows the *client's* local machine, which this form has
/// no direct access to. It follows the same handshake as nano's
/// `on_request_open`: the form emits `on_local_navigate` when the user wants
/// to browse a different local directory, and the client responds by
/// calling `update_local_listing()` with the freshly enumerated contents.
/// Selecting a row only tracks state; the actual transfer only happens once
/// the user clicks the upload/download button, which emits
/// `on_upload_requested`/`on_download_requested` for the client to act on.
///
/// Both panels support multi-row selection (see left_table_/right_table_'s
/// row_selected handling in on_event()): a plain click replaces the
/// selection with just that row; Ctrl+click toggles one row without
/// touching the rest; Shift+click (or holding Shift while dragging across
/// rows -- see Table's own doc comment in src/ui/ui_elements/table.cpp)
/// selects the contiguous range between the last plain-clicked row (the
/// "anchor") and the clicked/hovered row. The upload/download buttons act
/// on every selected *file* in the corresponding panel (selected
/// directories, and the ".." pseudo-row, are silently skipped).
///
/// Emitted events:
///   - `"closed"` — window X button; internal UI removed.
///   - `"on_local_navigate"` (`{name, type}`, `type` is `"dir"` or `"path"`)
///     — client should re-list the target local directory and call
///     `update_local_listing()`.
///   - `"on_upload_requested"` (`{names, local_path}`, `names` a plain-string
///     array) — client should read `local_path/<name>` for each `names`
///     entry, `upload_file()` it, then call `refresh_sandbox()` once after
///     the whole batch.
///   - `"on_download_requested"` (`{names}`) — client should `download_file()`
///     each entry, write it under the current local path, then call
///     `update_local_listing()` once after the whole batch to refresh the
///     left panel.
///   - `"on_upload_conflict"` (`{names, local_path}`) — every name in
///     `names` already exists in the sandbox. The client should confirm
///     once with the user (e.g. via an instantiated `MessageBox`,
///     `buttons: "yes_no"`) and, if confirmed, proceed exactly as it would
///     for `on_upload_requested`.
///   - `"on_download_conflict"` (`{names}`) — same as `on_upload_conflict`,
///     but these targets already exist locally.
///   - `"on_local_rename_requested"` (`{old_name, new_name}`) — the user
///     confirmed the local panel's Rename dialog. The client should rename
///     `old_name` to `new_name` inside the currently-shown local directory
///     and then re-report the listing (`update_local_listing()`), mirroring
///     `on_local_navigate`'s handshake.
///
/// Both panels also offer a per-row right-click `ContextMenu` (Properties /
/// Rename / Copy Path). The sandbox panel handles Rename and Properties
/// entirely server-side (direct `std::filesystem` access); the local panel's
/// Rename goes through `on_local_rename_requested` above since only the
/// client can touch its own filesystem, while its Properties uses the
/// name/type/size/modified already reported via `update_local_listing()` —
/// no extra round trip needed. Copy Path never touches the server at all:
/// it rides `MenuItem.copy_text` (src/ui/ui_elements/menu.cpp), which the
/// renderer copies to the OS clipboard directly on click. Rename/Properties
/// act on the single row that was right-clicked, independent of the current
/// multi-selection.
class mc : public form {
 public:
  explicit mc(bison::dynamic&& base);

  /// @brief RMI method: replace the left panel's displayed directory.
  /// @p args holds `path` (string) and `files` (dynamic array of entries,
  /// each `{name, type ("file"/"dir"), size, modified}` — `size`/`modified`
  /// are already client-formatted display strings).
  bison::dynamic do_update_local_listing(const bison::dynamic& args);

  /// @brief RMI method: re-enumerate the current sandbox directory. Called
  /// by the client after an upload completes, so the new file appears
  /// without requiring the user to navigate away and back.
  bison::dynamic do_refresh_sandbox(const bison::dynamic& args);

  /// @brief Called from the `__setter` prototype method for every set() call.
  /// Intercepts `local_path`, `sandbox_path`, `status`, `transfer_progress`,
  /// and `transfer_label` to mirror them into the internal widgets.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct file_row {
    std::string name;
    std::string type; ///< "file" or "dir"
    std::string size;
    std::string modified;
  };

  /// Which row context-menu action a MenuItem's `__wish_id` maps to (see
  /// local_menu_targets_/sandbox_menu_targets_), and which panel/entry it
  /// was built for. Resolved against local_entries_/sandbox_entries_ (by
  /// `name`) at click time, mirroring top.cpp's own pid-indirection pattern:
  /// on_event() looks the current entry up by name rather than this struct
  /// carrying a captured file_row copy, so a Rename/Properties click always
  /// reflects the freshest size/modified data for that name.
  enum class row_menu_action { properties, rename, copy_path };
  struct row_menu_target {
    row_menu_action action{row_menu_action::properties};
    bool is_sandbox{false};
    std::string name;
  };

  /// @brief Rebuilds @p table's rows from @p entries: name/size/modified
  /// cells (as before) plus, for every entry except a leading ".." row, a
  /// per-row ContextMenu (Properties/Rename/Copy Path). @p is_sandbox
  /// selects which panel's semantics the menu items should carry (see
  /// row_menu_target), and @p menu_targets is cleared and repopulated with
  /// this call's fresh set of MenuItem `__wish_id` -> action mappings --
  /// the previous call's entries are being replaced wholesale, the same way
  /// @p table's own row children are. @p selected_names marks every row
  /// whose `name` it contains as `TableRow.selected` (multi-selection
  /// highlight); defaults to none selected.
  void fill_table(
      const ui_element_ptr& table, const std::vector<file_row>& entries, bool is_sandbox,
      std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t>& menu_targets,
      const std::set<std::string>& selected_names = {});
  void set_status(const std::string& message);

  /// @brief Applies one row click's multi-selection semantics to @p
  /// selected/@p anchor, given @p entries' current order and the clicked
  /// row's @p idx plus the Ctrl/Shift modifier state from the click's
  /// payload (see Table's "row_selected" doc comment, table.cpp):
  ///   - Shift (a discrete click, or a frame of a Shift+drag sweep) selects
  ///     the contiguous range between @p anchor and @p idx, replacing the
  ///     previous selection; @p anchor itself does not move, so repeated
  ///     Shift+clicks/a drag sweep keep redefining the range's other end.
  ///     Falls through to plain-click behavior if @p anchor is unset (-1).
  ///   - Ctrl (with no Shift) toggles @p idx alone, leaving the rest of the
  ///     selection untouched, and becomes the new @p anchor.
  ///   - Neither modifier replaces the selection with just @p idx and moves
  ///     @p anchor there.
  static void apply_row_click(
      std::set<std::string>& selected, int32_t& anchor, const std::vector<file_row>& entries, int32_t idx, bool ctrl,
      bool shift);

  /// @brief Formats the "Selected: ..." label text for a panel's current
  /// multi-selection: "(none)", the single name, or "N items".
  static std::string describe_selection(const std::set<std::string>& selected);

  /// @brief Names in @p selected whose entry in @p entries is a file (not a
  /// directory or the ".." pseudo-row), in @p entries' display order --
  /// what the upload/download buttons actually act on.
  static std::vector<std::string> selected_file_names(
      const std::set<std::string>& selected, const std::vector<file_row>& entries);

  /// @brief Builds an `{names: [string...]}` event payload from @p names
  /// (a plain-string bison::dynamic array -- see git.cpp's
  /// read_string_array()/string_array() for the same convention).
  static bison::dynamic make_names_payload(const std::vector<std::string>& names);

  /// @brief Builds one row's ContextMenu element (Properties/Rename/Copy
  /// Path), registering each MenuItem's `__wish_id` in @p menu_targets.
  /// @p path_display is the string Copy Path copies to the clipboard --
  /// the sandbox-relative display path for a sandbox row, or the client's
  /// reported local path for a local row (see fill_table()'s call sites).
  ui_element_ptr build_row_context_menu(
      const file_row& entry, bool is_sandbox, const std::string& path_display,
      std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t>& menu_targets);

  /// @brief Sorts @p entries in place by the given file_table column
  /// (0=Name, 1=Size, 2=Modified -- see kLayout's col_name/col_size/
  /// col_modified `column_id`), leaving a leading ".." entry (see
  /// navigate_sandbox()) pinned first regardless of column/direction, the
  /// way Explorer keeps the parent-directory shortcut from moving under
  /// sort. Size is compared numerically via parse_display_size()
  /// (file_browser_utils.hpp), not lexicographically, since "size" is only
  /// ever a human-formatted string (e.g. "12.3 KB" would otherwise sort
  /// before "2 KB").
  void sort_entries(std::vector<file_row>& entries, int32_t sort_column_id, bool ascending) const;

  /// @brief Handle the left/right file_table's "sorted" event (see
  /// table.cpp's Table.flags doc comment): store the new sort state and
  /// re-sort + rebuild the given entries/table. @p selected_names is
  /// name-keyed (not index-keyed), so it stays valid across the reorder and
  /// is passed straight through to fill_table() -- only the selection
  /// *anchor* (an index) goes stale on sort; callers reset that themselves.
  void on_table_sorted(
      const bison::dynamic& payload, std::vector<file_row>& entries, const ui_element_ptr& table, bool is_sandbox,
      std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t>& menu_targets,
      int32_t& sort_column_id, bool& sort_ascending, const std::set<std::string>& selected_names);

  bool sandbox_has_file(const std::string& name) const;
  bool local_has_file(const std::string& name) const;

  /// @brief Navigate the sandbox panel to @p relative_path ("" = sandbox
  /// root) and re-list it. Rejects paths that escape the sandbox or are not
  /// directories, leaving the current listing untouched and setting status.
  ///
  /// @p resource_dir / @p allow_absolute_paths must be resolved by the
  /// caller (via sess() inside dispatch, via context_rlock outside it) --
  /// this function does not touch sync_ctx_ itself. See the .cpp for why.
  void navigate_sandbox(std::string relative_path, const std::filesystem::path& resource_dir, bool allow_absolute_paths);

  // ── Row context-menu actions ────────────────────────────────────────────
  //
  // Rename and Properties each use one dialog layout shared by both panels;
  // Rename needs to remember which panel/name it's acting on between
  // show_rename_dialog() and apply_rename() (rename_is_sandbox_/
  // rename_old_name_), while Properties is fully populated at show-time and
  // needs no persisted target. Mirrors top.cpp's confirm-kill/properties
  // dialog pattern: a small internal Window merged as its own top-level
  // object (a secondary root distinct from internal_root_key_), closed via
  // the __request_close__/closed handshake (see form.hpp's
  // request_close_at()).

  /// @brief Opens the Rename dialog for @p name in the local (@p is_sandbox
  /// == false) or sandbox (== true) panel, prefilled with its current name.
  void show_rename_dialog(bool is_sandbox, const std::string& name);
  void request_close_rename();
  void remove_rename_objects();
  /// @brief Validates and applies the Rename dialog's current input:
  /// rejects an empty name or one containing a path separator, then either
  /// renames directly (sandbox) or emits on_local_rename_requested (local).
  /// Closes the dialog either way.
  void apply_rename();

  /// @brief Opens the Properties dialog for @p entry from the local (@p
  /// is_sandbox == false) or sandbox (== true) panel. All fields are
  /// already known server-side (no client round trip needed even for the
  /// local panel -- see mc.hpp's class doc comment), so unlike top.cpp's
  /// Properties dialog this never needs to retarget an already-open
  /// instance -- see form::instantiate_child_form().
  void show_properties_dialog(bool is_sandbox, const file_row& entry);

  bison::key_t window_id_;
  bison::key_t left_path_id_;
  bison::key_t left_table_id_;
  bison::key_t right_path_id_;
  bison::key_t right_table_id_;
  bison::key_t open_explorer_id_;
  bison::key_t upload_id_;
  bison::key_t download_id_;

  ui_element_ptr left_path_ptr_;
  ui_element_ptr left_table_ptr_;
  ui_element_ptr left_selected_ptr_;
  ui_element_ptr left_stats_ptr_;
  ui_element_ptr left_disk_ptr_;
  ui_element_ptr right_path_ptr_;
  ui_element_ptr right_table_ptr_;
  ui_element_ptr right_selected_ptr_;
  ui_element_ptr right_stats_ptr_;
  ui_element_ptr right_disk_ptr_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr transfer_progress_ptr_;

  std::string local_path_;
  std::string sandbox_path_; ///< Relative to sandbox root; "" == root.
  std::vector<file_row> local_entries_;
  std::vector<file_row> sandbox_entries_;

  /// MenuItem `__wish_id` -> action, one map per panel since fill_table()
  /// clears and rebuilds only the panel it was called for.
  std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t> local_menu_targets_;
  std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t> sandbox_menu_targets_;

  std::string rename_root_key_;
  bison::key_t rename_window_id_;
  bison::key_t rename_ok_id_;
  bison::key_t rename_cancel_id_;
  ui_element_ptr rename_input_ptr_;
  bool rename_is_sandbox_{false};
  std::string rename_old_name_;

  /// Properties dialog: a privately-instantiated PropertiesDialog (see
  /// form::instantiate_child_form()). Only one may be open at a time; a new
  /// Properties click just overwrites this member -- the stale instance's
  /// destructor tears down its own internal objects, same effect the old
  /// direct remove_objects_at() call had.
  std::shared_ptr<properties_dialog> properties_dialog_;

  // Current per-panel sort state, applied by sort_entries() whenever
  // local_entries_/sandbox_entries_ is (re)built. Defaults to ascending by
  // Name (column_id 0), matching col_name's position in kLayout.
  int32_t local_sort_column_id_{0};
  bool local_sort_ascending_{true};
  int32_t sandbox_sort_column_id_{0};
  bool sandbox_sort_ascending_{true};

  // Multi-selection state per panel: the set of currently-selected entry
  // names (name-keyed, not index-keyed, so it survives a re-sort -- see
  // on_table_sorted()'s doc comment), plus the row index Shift+click/drag
  // range-selects against (-1 == unset). Reset (selection cleared, anchor
  // unset) whenever a panel's entries are wholesale replaced -- a fresh
  // listing (do_update_local_listing()) or sandbox navigation
  // (navigate_sandbox()) -- since old selected names may no longer exist in
  // the new directory.
  std::set<std::string> selected_local_names_;
  int32_t local_selection_anchor_{-1};
  std::set<std::string> selected_sandbox_names_;
  int32_t sandbox_selection_anchor_{-1};
};

/// @brief Register Mc in the "wish" bison namespace.
void register_mc();

} // namespace bdg::wish
