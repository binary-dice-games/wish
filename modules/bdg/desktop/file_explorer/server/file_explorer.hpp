// MIT License © 2025 Binary Dice Games
/// @file file_explorer.hpp
/// @brief Server-side FileExplorer form: two-panel file browser + transfer UI.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <filesystem>
#include <string>
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
/// no direct access to. It follows the same handshake as Notepad's
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
class file_explorer : public form {
 public:
  explicit file_explorer(bison::dynamic&& base);

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

  void fill_table(const ui_element_ptr& table, const std::vector<file_row>& entries, int32_t selected_index = -1);
  void set_status(const std::string& message);

  /// @brief What the "Overwrite?" confirmation, once accepted, should do.
  enum class pending_transfer { none, upload, download };

  /// @brief Builds (or rebuilds) the "<name> already exists ... overwrite?"
  /// modal as a second top-level internal Window (mirrors message_box.cpp's
  /// rebuild()/register_root(), inlined here rather than instantiating a
  /// separate MessageBox object -- this form already owns the on_event
  /// routing and selection state the confirmation needs to act on).
  void show_overwrite_confirm(pending_transfer kind, const std::string& name);

  /// @brief Thin wrapper over form::request_close_at(confirm_root_key_) --
  /// see that method's doc comment for why this can't just erase the tree
  /// directly from a button-click handler.
  void request_close_confirm();

  /// @brief Thin wrapper over form::remove_objects_at(confirm_root_key_),
  /// scoped to the confirm dialog's own root instead of internal_root_key_
  /// (a form only tracks automatic removal for the latter).
  void remove_confirm_objects();

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
  ui_element_ptr right_path_ptr_;
  ui_element_ptr right_table_ptr_;
  ui_element_ptr right_selected_ptr_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr transfer_progress_ptr_;

  std::string local_path_;
  std::string sandbox_path_; ///< Relative to sandbox root; "" == root.
  std::vector<file_row> local_entries_;
  std::vector<file_row> sandbox_entries_;

  std::string selected_local_name_;
  bool selected_local_is_dir_{false};
  std::string selected_sandbox_name_;
  bool selected_sandbox_is_dir_{false};

  std::string confirm_root_key_; ///< Empty when no confirm dialog is open.
  bison::key_t confirm_window_id_;
  bison::key_t confirm_yes_id_;
  bison::key_t confirm_no_id_;
  pending_transfer pending_transfer_{pending_transfer::none};
};

/// @brief Register FileExplorer in the "wish" bison namespace.
void register_file_explorer();

} // namespace bdg::wish
