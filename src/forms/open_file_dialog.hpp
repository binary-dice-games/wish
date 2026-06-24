// MIT License © 2025 Binary Dice Games
/// @file open_file_dialog.hpp
/// @brief Server-side OpenFileDialog form class.
#pragma once

#include <wish/form.hpp>
#include <wish/ui_element.hpp>

namespace bdg::wish {

/// @brief High-level file-picker dialog form.
///
/// The server manages all selection, row-navigation, and button logic. The
/// client supplies the file list via the `files` field and reacts to the three
/// high-level events: `on_open`, `on_navigate`, and `on_cancel`.
///
/// File paths produced by this form are always relative to the session sandbox
/// unless `allow_absolute_paths` is enabled server-side.
class open_file_dialog : public form {
 public:
  explicit open_file_dialog(bison::dynamic&& base);

  /// @brief Called from the `__setter` prototype method for every set() call.
  ///
  /// Intercepts `files` (rebuilds Table rows), `filters` (populates Combo and
  /// toggles row visibility), and `confirm_label` (updates btn_open label).
  /// Returns the patch unchanged so the RMI layer still applies field values.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;

 private:
  /// @brief Rebuild TableRow children from a new files dynamic.
  void rebuild_file_rows(const bison::dynamic& files);

  /// @brief Populate filter_combo items and toggle filter_row visibility.
  void rebuild_filter_combo(const bison::dynamic& filters);

  /// @brief Update filename field and filename_input widget from a row click.
  void on_row_selected(const bison::dynamic& payload);

  /// @brief Copy filename_input's changed value into the form's filename field.
  void on_filename_input_changed(const bison::dynamic& payload);

  /// @brief Store the newly selected filter index.
  void on_filter_combo_changed(const bison::dynamic& payload);

  /// @brief Validate filename, then emit on_open if the path is safe.
  void on_btn_open_clicked();

  /// @brief Emit on_cancel and remove the internal UI tree from session.objects.
  void on_btn_cancel_clicked();

  /// @brief Handle a double-click on a table row: emit on_navigate or on_open.
  void on_row_activated(const bison::dynamic& payload);

  ui_element_ptr file_table_ptr_;
  ui_element_ptr filename_input_ptr_;
  ui_element_ptr filter_combo_ptr_;
  ui_element_ptr filter_row_ptr_;
  ui_element_ptr btn_open_ptr_;
  bison::key_t   file_table_id_;
  bison::key_t   filename_input_id_;
  bison::key_t   btn_open_id_;
  bison::key_t   btn_cancel_id_;
  bison::key_t   filter_combo_id_;
  int32_t        selected_filter_idx_{0};
};

/// @brief Register OpenFileDialog in the "wish" bison namespace.
void register_open_file_dialog();

}  // namespace bdg::wish
