// MIT License © 2025 Binary Dice Games
/// @file open_file_dialog.hpp
/// @brief Server-side OpenFileDialog form class.
#pragma once

#include <wish/form.hpp>

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

 protected:
  void on_init() override;
};

/// @brief Register OpenFileDialog in the "wish" bison namespace.
void register_open_file_dialog();

}  // namespace bdg::wish
