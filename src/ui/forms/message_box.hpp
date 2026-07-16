// MIT License © 2025 Binary Dice Games
/// @file message_box.hpp
/// @brief Server-side MessageBox form class.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <string>
#include <unordered_map>

namespace bdg::wish {

/// @brief High-level modal message dialog, modeled on the Win32 MessageBox API.
///
/// Shows a title, a body message, an optional icon (`icon`: "none", "info",
/// "warning", "error", "question"), and a Win32-style button preset (`buttons`:
/// "ok", "ok_cancel", "yes_no", "yes_no_cancel", "retry_cancel", or
/// "abort_retry_ignore"). The internal Window is rendered as a true
/// input-blocking modal popup (Window.modal = true) and has no title-bar close
/// button — the user must click one of the presented buttons.
///
/// Clicking any button emits a single `on_result` event with a `button` field
/// holding one of "ok", "cancel", "yes", "no", "retry", "abort", "ignore" (the
/// name of the clicked button), then removes the internal UI tree.
class message_box : public form {
 public:
  explicit message_box(bison::dynamic&& base);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// @brief Widget ID → result string ("ok", "cancel", "yes", ...) for every
  /// button in the currently selected preset.
  std::unordered_map<bison::key_t, std::string, bison::key_t, bison::key_t> button_results_;
};

/// @brief Register MessageBox in the "wish" bison namespace.
void register_message_box();

} // namespace bdg::wish
