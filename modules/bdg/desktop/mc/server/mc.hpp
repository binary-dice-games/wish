// MIT License © 2025 Binary Dice Games
/// @file mc.hpp
/// @brief Server-side form for mc (a two-panel file browser + transfer UI).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

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
/// Emitted events:
///   - `"closed"` — window X button; internal UI removed.
///   - `"on_local_navigate"` (`{name, type}`, `type` is `"dir"` or `"path"`)
///     — client should re-list the target local directory and call
///     `update_local_listing()`.
///   - `"on_upload_requested"` (`{name, local_path}`) — client should read
///     `local_path/name`, call `upload_file()`, then `refresh_sandbox()`.
///   - `"on_download_requested"` (`{name}`) — client should call
///     `download_file()`, write it under the current local path, then call
///     `update_local_listing()` to refresh the left panel.
///   - `"on_upload_conflict"` (`{name, local_path}`) — the upload target
///     already exists in the sandbox. The client should confirm with the
///     user (e.g. via an instantiated `MessageBox`, `buttons: "yes_no"`) and,
///     if confirmed, proceed exactly as it would for `on_upload_requested`.
///   - `"on_download_conflict"` (`{name}`) — same as `on_upload_conflict`,
///     but the download target already exists locally.
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
/// renderer copies to the OS clipboard directly on click.
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
  /// @p table's own row children are.
  void fill_table(
      const ui_element_ptr& table, const std::vector<file_row>& entries, bool is_sandbox,
      std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t>& menu_targets,
      int32_t selected_index = -1);
  void set_status(const std::string& message);

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
  /// re-sort + rebuild the given entries/table.
  void on_table_sorted(
      const bison::dynamic& payload, std::vector<file_row>& entries, const ui_element_ptr& table, bool is_sandbox,
      std::unordered_map<bison::key_t, row_menu_target, bison::key_t, bison::key_t>& menu_targets,
      int32_t& sort_column_id, bool& sort_ascending);

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
  /// local panel -- see mc.hpp's class doc comment).
  void show_properties_dialog(bool is_sandbox, const file_row& entry);
  void request_close_properties();
  void remove_properties_objects();

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

  std::string properties_root_key_;
  bison::key_t properties_window_id_;
  bison::key_t properties_close_id_;
  ui_element_ptr properties_name_ptr_;
  ui_element_ptr properties_type_ptr_;
  ui_element_ptr properties_size_ptr_;
  ui_element_ptr properties_modified_ptr_;
  ui_element_ptr properties_path_ptr_;

  // Current per-panel sort state, applied by sort_entries() whenever
  // local_entries_/sandbox_entries_ is (re)built. Defaults to ascending by
  // Name (column_id 0), matching col_name's position in kLayout.
  int32_t local_sort_column_id_{0};
  bool local_sort_ascending_{true};
  int32_t sandbox_sort_column_id_{0};
  bool sandbox_sort_ascending_{true};

  std::string selected_local_name_;
  bool selected_local_is_dir_{false};
  std::string selected_sandbox_name_;
  bool selected_sandbox_is_dir_{false};
};

/// @brief Register Mc in the "wish" bison namespace.
void register_mc();

} // namespace bdg::wish
