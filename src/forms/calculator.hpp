// MIT License © 2025 Binary Dice Games
/// @file calculator.hpp
/// @brief Server-side Calculator form.
#pragma once

#include <wish/form.hpp>
#include <wish/ui_element.hpp>

namespace bdg::wish {

/// @brief Self-contained four-function calculator form.
///
/// Manages its own Window + button grid + display label internally.
/// All arithmetic logic lives server-side; the client only needs to instantiate
/// the form and listen for the `"closed"` event to know when the user is done.
///
/// Emitted events:
///   - `"closed"` — user clicked the window X button; internal UI is removed.
class calculator : public form {
 public:
  explicit calculator(bison::dynamic&& base);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id,
                bison::key_t event_name,
                const bison::dynamic& payload) override;

 private:
  // ── Calculator state ──────────────────────────────────────────────────────

  void handle_digit(const std::string& ch);
  void handle_operator(char op);
  void handle_equals();
  void handle_clear();
  void handle_negate();
  void handle_percent();
  void handle_backspace();
  void handle_dot();
  void update_display();

  std::string display_{"0"};
  double      operand_{0.0};
  char        pending_op_{0};
  bool        fresh_{true};

  // ── Widget ID cache ───────────────────────────────────────────────────────

  bison::key_t window_id_;
  bison::key_t display_id_;

  bison::key_t btn_c_, btn_div_, btn_mul_, btn_bsp_;
  bison::key_t btn_n7_, btn_n8_, btn_n9_, btn_sub_;
  bison::key_t btn_n4_, btn_n5_, btn_n6_, btn_add_;
  bison::key_t btn_n1_, btn_n2_, btn_n3_, btn_eq_;
  bison::key_t btn_n0_, btn_dot_, btn_pm_,  btn_pct_;

  ui_element_ptr display_ptr_;
};

/// @brief Register Calculator in the "wish" bison namespace.
void register_calculator();

} // namespace bdg::wish
