// MIT License © 2025 Binary Dice Games
/// @file bc.hpp
/// @brief Server-side form for bc, a multi-mode calculator (standard,
///        scientific, programmer).
#pragma once

#include "calc_engine.hpp"
#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish {

/// @brief Self-contained calculator form with switchable layouts.
///
/// Modeled on the Windows / GNOME / macOS calculators:
///   - **Standard**: the four operations, %, 1/x, x², √x, sign, CE/C/⌫.
///   - **Scientific**: operator precedence and parentheses, trigonometric
///     (DEG/RAD/GRAD, inverse via 2nd, hyperbolic via hyp), powers, roots,
///     logarithms, n!, mod, |x|, floor/ceil, π, e, rand, Exp entry and a
///     forced scientific-notation (F-E) display.
///   - **Programmer**: 64/32/16/8-bit two's complement integers shown at once
///     in HEX/DEC/OCT/BIN (click a row to type in that radix), A-F digits,
///     AND/OR/XOR/NAND/NOR/NOT, shifts and one-bit rotates.
///
/// Every mode has memory keys (MC/MR/M+/M-/MS) and an optional history panel
/// (View > History) whose entries can be clicked to recall a result. The
/// layout is switched from the mode tabs or the View menu; the window resizes
/// to the active keypad. All arithmetic lives in calc::engine.
///
/// Emitted events:
///   - `"closed"` — user clicked the window X button; internal UI is removed.
class bc : public common::tool_form {
 public:
  explicit bc(bison::dynamic&& base);

  /// Number of history entries the history panel shows.
  static constexpr std::size_t kHistorySlots = 10;

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// One calculator key (a Button). A key with `label_fn` is relabelled on
  /// every refresh: 2nd / hyp alternates and the toggle keys' "on" marker.
  struct key_def {
    std::string id;
    std::string label;
    std::function<void()> action;
    std::function<std::string()> label_fn{};
  };
  using key_row = std::vector<key_def>;
  struct keypad_def {
    std::string id;
    int key_width;
    int key_height;
    std::vector<key_row> rows;
  };

  static int keypad_height(const keypad_def& pad);
  std::vector<keypad_def> make_keypads();
  std::string make_layout(const std::vector<keypad_def>& pads) const;
  void set_mode(calc::mode m);
  void refresh();
  /// Sizes the window to the active keypad (plus the history panel).
  void fit_window();

  calc::engine engine_;
  bool second_{false}; ///< "2nd": inverse functions in scientific mode
  bool hyp_{false}; ///< "hyp": hyperbolic trig in scientific mode
  bool show_history_{false};

  // ── Widgets ───────────────────────────────────────────────────────────────

  bison::key_t window_id_;
  ui_element_ptr window_;
  std::array<std::pair<int, int>, 3> keypad_sizes_{}; ///< width, height per mode
  ui_element_ptr display_;
  ui_element_ptr expression_;
  ui_element_ptr status_;
  ui_element_ptr radix_panel_;
  ui_element_ptr history_panel_;
  ui_element_ptr history_empty_;
  ui_element_ptr copy_item_;
  ui_element_ptr history_item_;
  ui_element_ptr grouping_item_;
  std::array<ui_element_ptr, 3> keypads_{};
  std::array<ui_element_ptr, 3> mode_tabs_{};
  std::array<ui_element_ptr, 3> mode_items_{};
  std::array<ui_element_ptr, 3> angle_items_{};
  std::array<ui_element_ptr, 4> radix_rows_{};
  std::vector<ui_element_ptr> history_slots_;
  std::vector<std::pair<ui_element_ptr, std::function<std::string()>>> dynamic_labels_;
  std::vector<std::pair<ui_element_ptr, std::function<bool()>>> toggles_;
};

/// @brief Register bc (RMI class "Bc") in the "wish" bison namespace.
void register_bc();

} // namespace bdg::wish
