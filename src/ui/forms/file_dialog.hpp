// MIT License © 2025 Binary Dice Games
/// @file file_dialog.hpp
/// @brief Server-side FileDialog form class.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <regex>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief High-level file-picker dialog form.
///
/// Usable as an Open File or Save As dialog depending on the `confirm_label`
/// field supplied by the client. The server manages all selection,
/// row-navigation, and button logic. The client supplies the file list via the
/// `files` field and reacts to three high-level events: `on_open`,
/// `on_navigate`, and `on_cancel`.
///
/// The path bar is an editable InputText: pressing Enter emits `on_navigate`
/// with `type = "path"` so the client can jump directly to a typed directory.
///
/// File paths produced by this form are always relative to the session sandbox
/// unless `allow_absolute_paths` is enabled server-side.
class file_dialog : public form {
 public:
  explicit file_dialog(bison::dynamic&& base);

  /// @brief Called from the `__setter` prototype method for every set() call.
  ///
  /// Intercepts `files` (rebuilds Table rows), `filters` (populates Combo and
  /// toggles row visibility), `confirm_label` (updates btn_open label), and
  /// `path` (updates path_input value).
  /// Returns the patch unchanged so the RMI layer still applies field values.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// @brief Rebuild TableRow children from a new files dynamic.
  void rebuild_file_rows(const bison::dynamic& files);

  /// @brief Populate filter_combo items and toggle filter_row visibility.
  void rebuild_filter_combo(const bison::dynamic& filters);

  /// @brief Update filename field and filename_input widget from a row click.
  void on_row_selected(const bison::dynamic& payload);

  /// @brief Copy filename_input's changed value into the form's filename field.
  void on_filename_input_changed(const bison::dynamic& payload);

  /// @brief Emit on_navigate with type="path" when user commits a typed path.
  void on_path_input_changed(const bison::dynamic& payload);

  /// @brief Store the newly selected filter index and re-apply filtering.
  void on_filter_combo_changed(const bison::dynamic& payload);

  /// @brief Validate filename, then emit on_open if the path is safe.
  void on_btn_open_clicked();

  /// @brief Emit on_cancel and remove the internal UI tree from context.ui_objects.
  void on_btn_cancel_clicked();

  /// @brief Handle a double-click on a table row: emit on_navigate or on_open.
  void on_row_activated(const bison::dynamic& payload);

  bison::dynamic_ptr cached_files_; // last files list received from client
  std::vector<std::string> filter_regexes_; // one per filter; empty = match all
  std::vector<size_t> row_to_file_idx_; // maps visible row index → cached_files_ index

  ui_element_ptr file_table_ptr_;
  ui_element_ptr path_input_ptr_;
  ui_element_ptr filename_input_ptr_;
  ui_element_ptr filter_combo_ptr_;
  ui_element_ptr filter_row_ptr_;
  ui_element_ptr btn_open_ptr_;
  bison::key_t file_table_id_;
  bison::key_t path_input_id_;
  bison::key_t filename_input_id_;
  bison::key_t btn_open_id_;
  bison::key_t btn_cancel_id_;
  bison::key_t filter_combo_id_;
  int32_t selected_filter_idx_{0};
};

/// @brief Register FileDialog in the "wish" bison namespace.
void register_file_dialog();

} // namespace bdg::wish
