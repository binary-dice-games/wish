// MIT License © 2026 Binary Dice Games
/// @file calc_engine.hpp
/// @brief UI-independent calculator engine behind the bc form: entry editing,
///        operator precedence, parentheses, memory, history, and the
///        standard / scientific / programmer modes.
///
/// The engine models a key-driven calculator (Windows Calculator, GNOME
/// Calculator, macOS Calculator): every method is one key press, and
/// display() / expression() are what the two display lines show afterwards.
/// It has no wish dependency, so it is unit-tested directly.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace bdg::wish::calc {

/// @brief Calculator layout / arithmetic mode.
///
/// `standard` evaluates left to right as each operator is pressed
/// (2 + 3 x 4 = 20); `scientific` and `programmer` honor operator precedence
/// and parentheses (2 + 3 x 4 = 14). `programmer` works on signed two's
/// complement integers of the current word_size.
enum class mode { standard, scientific, programmer };

/// @brief Unit of the trigonometric functions' angles (scientific mode).
enum class angle_unit { degrees, radians, gradians };

/// @brief Integer width in programmer mode (bits).
enum class word_size : int { byte = 8, word = 16, dword = 32, qword = 64 };

/// @brief Infix operators, applied with precedence on equals()/close_paren().
enum class binary_op {
  add,
  subtract,
  multiply,
  divide,
  modulo,
  power, ///< x^y
  root, ///< y-th root of x
  log_base, ///< logarithm of x in base y
  shift_left,
  shift_right, ///< arithmetic (sign-filling) shift
  bit_and,
  bit_or,
  bit_xor,
  bit_nand,
  bit_nor,
};

/// @brief Prefix functions, applied immediately to the displayed value.
enum class unary_op {
  negate,
  reciprocal,
  square,
  cube,
  sqrt,
  cbrt,
  pow10,
  pow2,
  exp,
  ln,
  log10,
  log2,
  abs,
  factorial, ///< n!, Gamma(x + 1) for non-integers
  floor,
  ceil,
  sin,
  cos,
  tan,
  asin,
  acos,
  atan,
  sinh,
  cosh,
  tanh,
  asinh,
  acosh,
  atanh,
  bit_not,
  rotate_left, ///< programmer: rotate one bit left within the word size
  rotate_right, ///< programmer: rotate one bit right within the word size
};

/// @brief A value as the engine stores it: a double in the standard and
/// scientific modes, a word-size-truncated integer in programmer mode.
/// Conversion between the two happens lazily when a value crosses modes
/// (memory, history, mode switches).
struct value {
  bool integral{false};
  double real{0.0};
  int64_t integer{0};
};

/// @brief One completed calculation, newest first in engine::history().
struct history_entry {
  std::string expression; ///< e.g. "2 + 3 × 4 ="
  std::string result; ///< display text of the result
  value result_value;
};

/// @brief Key-driven calculator state machine.
///
/// Errors (division by zero, domain errors, overflow) put the engine in an
/// error state: display() shows the message, operators are ignored, and the
/// next digit, constant, clear() or clear_entry() starts over.
class engine {
 public:
  static constexpr std::size_t kMaxHistory = 50;
  static constexpr std::size_t kMaxDigits = 16;

  // ── Entry ─────────────────────────────────────────────────────────────────

  /// @brief Types digit @p d (0-15). Ignored if not valid in the current
  /// radix, or (programmer) if the entry would no longer fit the word size.
  void digit(int d);
  /// @brief Types the decimal point. Ignored in programmer mode.
  void point();
  /// @brief Starts the exponent of a scientific-notation entry ("Exp" key):
  /// 1.5 Exp 3 enters 1.5e+3. Ignored in programmer mode.
  void exponent();
  /// @brief Deletes the last typed character.
  void backspace();
  /// @brief Toggles the sign of the entry (or of its exponent while typing
  /// one), or negates a computed value.
  void negate();
  /// @brief "CE": resets the current entry to 0, keeping the expression.
  void clear_entry();
  /// @brief "C": resets the entry and the expression (not memory/history).
  void clear();
  /// @brief Replaces the entry by a constant such as pi, labelled @p label
  /// in the expression line.
  void constant(double v, const std::string& label);
  /// @brief Replaces the entry by a uniformly random number in [0, 1).
  void random();

  // ── Operators ─────────────────────────────────────────────────────────────

  /// @brief Appends @p op after the current operand. Pressed right after
  /// another operator, it replaces that operator instead.
  void binary(binary_op op);
  /// @brief Applies @p op to the displayed value.
  void unary(unary_op op);
  /// @brief "%": after + or - turns x into lhs * x / 100, otherwise x / 100.
  void percent();
  /// @brief Opens a parenthesis; after an operand it implies multiplication.
  void open_paren();
  /// @brief Closes the innermost parenthesis. Ignored when none is open.
  void close_paren();
  /// @brief Evaluates the expression (closing open parentheses) and records
  /// it in history(). Pressed again, repeats the last operation (5 + 3 = =).
  void equals();

  // ── Memory ────────────────────────────────────────────────────────────────

  void memory_clear();
  void memory_recall();
  void memory_store();
  void memory_add();
  void memory_subtract();
  bool has_memory() const {
    return memory_.has_value();
  }

  // ── History ───────────────────────────────────────────────────────────────

  const std::deque<history_entry>& history() const {
    return history_;
  }
  /// @brief Puts history()[@p index]'s result back on the display.
  void recall_history(std::size_t index);
  void clear_history() {
    history_.clear();
  }

  // ── Settings ──────────────────────────────────────────────────────────────

  /// @brief Switches mode, keeping the displayed value (truncated to an
  /// integer when entering programmer mode) and clearing the expression.
  void set_mode(mode m);
  mode get_mode() const {
    return mode_;
  }
  void set_angle_unit(angle_unit u) {
    angle_ = u;
  }
  angle_unit get_angle_unit() const {
    return angle_;
  }
  /// @brief Sets the programmer-mode input/display radix (2, 8, 10 or 16).
  void set_radix(int radix);
  int radix() const {
    return mode_ == mode::programmer ? radix_ : 10;
  }
  /// @brief Sets the programmer-mode word size, truncating the current value.
  void set_word_size(word_size w);
  word_size get_word_size() const {
    return word_;
  }
  /// @brief Forces scientific notation ("F-E") in the decimal modes.
  void set_scientific_notation(bool on) {
    sci_notation_ = on;
  }
  bool scientific_notation() const {
    return sci_notation_;
  }
  /// @brief Thousands separators (decimal) / nibble spacing (hex, binary).
  void set_digit_grouping(bool on) {
    grouping_ = on;
  }
  bool digit_grouping() const {
    return grouping_;
  }

  // ── Output ────────────────────────────────────────────────────────────────

  /// @brief Main display line: the entry being typed, the latest result, or
  /// an error message.
  std::string display() const;
  /// @brief Secondary line showing the pending expression, e.g. "2 + (3 ×".
  std::string expression() const;
  /// @brief The displayed value in @p radix (programmer mode's HEX/DEC/OCT/
  /// BIN rows), grouped per digit_grouping().
  std::string display_in(int radix) const;
  /// @brief The displayed value as plain text (no grouping), for copying.
  std::string display_plain() const;
  /// @brief Number of currently unclosed parentheses.
  int open_parens() const {
    return static_cast<int>(expr_levels_.size()) - 1;
  }
  bool has_error() const {
    return error_.has_value();
  }
  /// @brief The displayed value (the entry is parsed if one is being typed).
  value current() const;

  // ── Formatting helpers (public for tests) ─────────────────────────────────

  /// @brief Formats @p v with up to 15 significant digits, switching to
  /// scientific notation for very large / small magnitudes or when
  /// @p force_sci is set ("1.5e+20").
  static std::string format_real(double v, bool force_sci = false);

 private:
  struct pending_op {
    bool paren{false};
    binary_op op{binary_op::add};
  };

  bool programmer() const {
    return mode_ == mode::programmer;
  }
  int bits() const {
    return static_cast<int>(word_);
  }
  int64_t truncate(uint64_t v) const;
  double as_real(const value& v) const;
  int64_t as_int(const value& v) const;
  value make_real(double d) const;
  value make_int(int64_t i) const;
  value coerce(const value& v) const;

  std::string format(const value& v, int radix, bool grouped) const;
  std::string group(const std::string& text, int radix) const;
  std::string label_of_current() const;

  void begin_input(); // clears an error / a finished result before new input
  void sync_entry(); // re-parses entry_ into current_
  value zero() const;
  void record(const std::string& expr, const value& result);
  void commit_entry();
  void set_current(const value& v, std::string label);
  void fail(const std::string& message);

  int precedence(binary_op op) const;
  bool reduce_one(); // pops one operator; false on error
  bool reduce_while(int min_precedence, bool right_assoc);
  bool reduce_to_paren();
  std::optional<value> apply(binary_op op, const value& a, const value& b);
  std::optional<value> apply(unary_op op, const value& a);
  double to_radians(double x) const;
  double from_radians(double x) const;
  std::optional<binary_op> top_binary() const;

  // Settings.
  mode mode_{mode::standard};
  angle_unit angle_{angle_unit::degrees};
  word_size word_{word_size::qword};
  int radix_{10};
  bool sci_notation_{false};
  bool grouping_{true};

  // Current operand.
  std::optional<std::string> entry_; ///< text being typed, in radix()
  value current_;
  std::string current_label_; ///< function label, e.g. "sqr(3)"; empty for plain values
  bool operand_entered_{false}; ///< an operand was typed/computed since the last operator
  bool last_was_op_{false}; ///< the last key was a binary operator
  bool just_evaluated_{false}; ///< the last key was equals()

  // Pending expression (shunting-yard stacks) and its display text, one
  // string per parenthesis nesting level.
  std::vector<value> values_;
  std::vector<pending_op> ops_;
  std::vector<std::string> expr_levels_{std::string{}};
  std::string finished_expr_; ///< expression line after equals()
  std::size_t last_op_text_pos_{0}; ///< where the last operator's text starts in expr_levels_.back()

  // Repeat-equals state.
  std::optional<binary_op> last_op_;
  value last_rhs_;
  std::string last_rhs_label_;

  std::optional<std::string> error_;
  std::optional<value> memory_;
  std::deque<history_entry> history_;
  std::mt19937_64 rng_{std::random_device{}()};
};

} // namespace bdg::wish::calc
