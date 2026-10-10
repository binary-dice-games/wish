// MIT License © 2026 Binary Dice Games
/// @file calc_engine.cpp
/// @brief Implementation of the bc calculator engine.
#include "calc_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace bdg::wish::calc {

namespace {

constexpr double kPi = 3.14159265358979323846;

// UTF-8 glyphs, written as escapes so the source stays ASCII.
constexpr const char* kTimes = "\xC3\x97"; // multiplication sign
constexpr const char* kDivide = "\xC3\xB7"; // division sign
constexpr const char* kSqrt = "\xE2\x88\x9A";

const char* const kDivByZero = "Cannot divide by zero";
const char* const kUndefined = "Result is undefined";
const char* const kInvalid = "Invalid input";
const char* const kOverflow = "Overflow";

std::string op_symbol(binary_op op) {
  switch (op) {
    case binary_op::add:
      return "+";
    case binary_op::subtract:
      return "-";
    case binary_op::multiply:
      return kTimes;
    case binary_op::divide:
      return kDivide;
    case binary_op::modulo:
      return "mod";
    case binary_op::power:
      return "^";
    case binary_op::root:
      return "yroot";
    case binary_op::log_base:
      return "log base";
    case binary_op::shift_left:
      return "<<";
    case binary_op::shift_right:
      return ">>";
    case binary_op::bit_and:
      return "AND";
    case binary_op::bit_or:
      return "OR";
    case binary_op::bit_xor:
      return "XOR";
    case binary_op::bit_nand:
      return "NAND";
    case binary_op::bit_nor:
      return "NOR";
  }
  return "?";
}

std::string unary_label(unary_op op, const std::string& x) {
  // A parenthesized group already carries its parentheses: sqr((1 + 2)) reads
  // as sqr(1 + 2).
  bool grouped = x.size() >= 2 && x.front() == '(' && x.back() == ')';
  auto wrap = [&](const char* name) { return std::string{name} + (grouped ? x : "(" + x + ")"); };
  switch (op) {
    case unary_op::negate:
      return wrap("negate");
    case unary_op::reciprocal:
      return wrap("1/");
    case unary_op::square:
      return wrap("sqr");
    case unary_op::cube:
      return wrap("cube");
    case unary_op::sqrt:
      return wrap(kSqrt);
    case unary_op::cbrt:
      return wrap("cuberoot");
    case unary_op::pow10:
      return wrap("10^");
    case unary_op::pow2:
      return wrap("2^");
    case unary_op::exp:
      return wrap("e^");
    case unary_op::ln:
      return wrap("ln");
    case unary_op::log10:
      return wrap("log");
    case unary_op::log2:
      return wrap("log2");
    case unary_op::abs:
      return wrap("abs");
    case unary_op::factorial:
      return wrap("fact");
    case unary_op::floor:
      return wrap("floor");
    case unary_op::ceil:
      return wrap("ceil");
    case unary_op::sin:
      return wrap("sin");
    case unary_op::cos:
      return wrap("cos");
    case unary_op::tan:
      return wrap("tan");
    case unary_op::asin:
      return wrap("asin");
    case unary_op::acos:
      return wrap("acos");
    case unary_op::atan:
      return wrap("atan");
    case unary_op::sinh:
      return wrap("sinh");
    case unary_op::cosh:
      return wrap("cosh");
    case unary_op::tanh:
      return wrap("tanh");
    case unary_op::asinh:
      return wrap("asinh");
    case unary_op::acosh:
      return wrap("acosh");
    case unary_op::atanh:
      return wrap("atanh");
    case unary_op::bit_not:
      return wrap("NOT");
    case unary_op::rotate_left:
      return wrap("RoL");
    case unary_op::rotate_right:
      return wrap("RoR");
  }
  return x;
}

bool is_integer(double x) {
  return std::isfinite(x) && x == std::floor(x);
}

char digit_char(int d) {
  return static_cast<char>(d < 10 ? '0' + d : 'A' + (d - 10));
}

int digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

// Parses unsigned digits in @p radix; nullopt on overflow of 64 bits.
std::optional<uint64_t> parse_unsigned(const std::string& text, int radix) {
  uint64_t acc = 0;
  for (char c : text) {
    int d = digit_value(c);
    if (d < 0 || d >= radix)
      return std::nullopt;
    if (acc > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / static_cast<uint64_t>(radix))
      return std::nullopt;
    acc = acc * static_cast<uint64_t>(radix) + static_cast<uint64_t>(d);
  }
  return acc;
}

} // namespace

// ── Value conversion ──────────────────────────────────────────────────────────

int64_t engine::truncate(uint64_t v) const {
  int b = bits();
  if (b >= 64)
    return static_cast<int64_t>(v);
  uint64_t mask = (uint64_t{1} << b) - 1;
  v &= mask;
  if (v & (uint64_t{1} << (b - 1)))
    v |= ~mask;
  return static_cast<int64_t>(v);
}

double engine::as_real(const value& v) const {
  return v.integral ? static_cast<double>(v.integer) : v.real;
}

int64_t engine::as_int(const value& v) const {
  if (v.integral)
    return truncate(static_cast<uint64_t>(v.integer));
  double d = std::trunc(v.real);
  if (!std::isfinite(d))
    return 0;
  // 2^63 is exactly representable; anything at or beyond it saturates.
  constexpr double kLimit = 9223372036854775808.0;
  int64_t i = d >= kLimit ? std::numeric_limits<int64_t>::max()
      : d < -kLimit       ? std::numeric_limits<int64_t>::min()
                          : static_cast<int64_t>(d);
  return truncate(static_cast<uint64_t>(i));
}

value engine::make_real(double d) const {
  return value{false, d, 0};
}

value engine::make_int(int64_t i) const {
  return value{true, 0.0, truncate(static_cast<uint64_t>(i))};
}

value engine::coerce(const value& v) const {
  return programmer() ? make_int(as_int(v)) : make_real(as_real(v));
}

value engine::zero() const {
  return coerce(value{});
}

value engine::current() const {
  return current_;
}

// ── Formatting ────────────────────────────────────────────────────────────────

std::string engine::format_real(double v, bool force_sci) {
  if (v == 0.0)
    return "0"; // also folds -0
  if (std::isnan(v))
    return "NaN";
  if (std::isinf(v))
    return v > 0 ? "Infinity" : "-Infinity";

  double a = std::fabs(v);
  char buf[64];
  if (!force_sci && a < 1e16 && a >= 1e-9) {
    int magnitude = static_cast<int>(std::floor(std::log10(a)));
    int precision = std::max(0, 14 - magnitude);
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    std::string s{buf};
    if (s.find('.') != std::string::npos) {
      s.erase(s.find_last_not_of('0') + 1);
      if (s.back() == '.')
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
  }

  std::snprintf(buf, sizeof(buf), "%.14e", v);
  std::string s{buf};
  auto e = s.find('e');
  std::string mantissa = s.substr(0, e);
  int exponent = std::atoi(s.c_str() + e + 1);
  if (mantissa.find('.') != std::string::npos) {
    mantissa.erase(mantissa.find_last_not_of('0') + 1);
    if (mantissa.back() == '.')
      mantissa.pop_back();
  }
  return mantissa + "e" + (exponent < 0 ? "-" : "+") + std::to_string(std::abs(exponent));
}

std::string engine::format(const value& v, int radix, bool grouped) const {
  std::string s;
  if (programmer()) {
    int64_t i = as_int(v);
    if (radix == 10) {
      s = std::to_string(i);
    } else {
      uint64_t u = static_cast<uint64_t>(i);
      if (bits() < 64)
        u &= (uint64_t{1} << bits()) - 1;
      do {
        s.insert(s.begin(), digit_char(static_cast<int>(u % static_cast<uint64_t>(radix))));
        u /= static_cast<uint64_t>(radix);
      } while (u != 0);
    }
  } else {
    s = format_real(as_real(v), sci_notation_);
  }
  return grouped ? group(s, radix) : s;
}

std::string engine::group(const std::string& text, int radix) const {
  std::size_t begin = (!text.empty() && text[0] == '-') ? 1 : 0;
  std::size_t end = text.find_first_of(".e");
  if (end == std::string::npos)
    end = text.size();
  std::size_t size = radix == 10 || radix == 8 ? 3 : 4;
  char separator = radix == 10 ? ',' : ' ';

  std::string out = text.substr(end);
  std::size_t count = 0;
  for (std::size_t i = end; i > begin; --i) {
    if (count == size) {
      out.insert(out.begin(), separator);
      count = 0;
    }
    out.insert(out.begin(), text[i - 1]);
    ++count;
  }
  return text.substr(0, begin) + out;
}

std::string engine::display() const {
  if (error_)
    return *error_;
  if (entry_)
    return grouping_ ? group(*entry_, radix()) : *entry_;
  return format(current_, radix(), grouping_);
}

std::string engine::display_in(int radix) const {
  if (error_)
    return "";
  return format(current_, radix, grouping_);
}

std::string engine::display_plain() const {
  if (error_)
    return "";
  if (entry_)
    return *entry_;
  return format(current_, radix(), false);
}

std::string engine::expression() const {
  if (just_evaluated_)
    return finished_expr_;
  std::string s;
  for (std::size_t i = 0; i < expr_levels_.size(); ++i) {
    if (i > 0)
      s += "(";
    s += expr_levels_[i];
  }
  if (operand_entered_ && !current_label_.empty())
    s += current_label_;
  while (!s.empty() && s.back() == ' ')
    s.pop_back();
  return s;
}

std::string engine::label_of_current() const {
  return current_label_.empty() ? format(current_, radix(), false) : current_label_;
}

// ── Entry ─────────────────────────────────────────────────────────────────────

void engine::begin_input() {
  if (error_)
    clear();
  if (just_evaluated_) {
    just_evaluated_ = false;
    finished_expr_.clear();
  }
}

void engine::sync_entry() {
  if (!entry_)
    return;
  if (programmer()) {
    auto u = parse_unsigned(*entry_, radix_);
    current_ = make_int(truncate(u.value_or(0)));
  } else {
    current_ = make_real(std::strtod(entry_->c_str(), nullptr));
  }
}

void engine::commit_entry() {
  entry_.reset();
}

void engine::set_current(const value& v, std::string label) {
  entry_.reset();
  current_ = v;
  current_label_ = std::move(label);
  operand_entered_ = true;
  last_was_op_ = false;
}

void engine::fail(const std::string& message) {
  error_ = message;
  entry_.reset();
  values_.clear();
  ops_.clear();
  expr_levels_ = {std::string{}};
  finished_expr_.clear();
  current_label_.clear();
  last_op_.reset();
  just_evaluated_ = false;
  operand_entered_ = false;
  last_was_op_ = false;
}

void engine::digit(int d) {
  if (d < 0 || d >= radix())
    return;
  begin_input();
  if (!entry_) {
    entry_ = std::string{};
    current_label_.clear();
  }
  std::string& e = *entry_;
  char c = digit_char(d);

  if (programmer()) {
    std::string next = (e.empty() || e == "0") ? std::string(1, c) : e + c;
    auto u = parse_unsigned(next, radix_);
    // Decimal entry is capped at the signed maximum, other radixes at the
    // full unsigned width of the word (two's complement bit patterns).
    uint64_t limit = radix_ == 10
        ? (bits() == 64 ? uint64_t{std::numeric_limits<int64_t>::max()} : (uint64_t{1} << (bits() - 1)) - 1)
        : bits() == 64 ? std::numeric_limits<uint64_t>::max()
                       : (uint64_t{1} << bits()) - 1;
    if (u && *u <= limit)
      e = next;
    else if (e.empty())
      e = "0";
  } else {
    auto epos = e.find('e');
    if (epos != std::string::npos) {
      if (e.size() - epos - 2 < 3)
        e += c;
    } else if (e.empty() || e == "0") {
      e = std::string(1, c);
    } else if (e == "-0") {
      e = std::string{"-"} + c;
    } else {
      auto digits =
          static_cast<std::size_t>(std::count_if(e.begin(), e.end(), [](char ch) { return ch >= '0' && ch <= '9'; }));
      if (digits < kMaxDigits)
        e += c;
    }
  }
  sync_entry();
  operand_entered_ = true;
  last_was_op_ = false;
}

void engine::point() {
  if (programmer())
    return;
  begin_input();
  if (!entry_) {
    entry_ = std::string{"0."};
    current_label_.clear();
  } else if (entry_->find_first_of(".e") == std::string::npos) {
    *entry_ += entry_->empty() ? "0." : ".";
  }
  sync_entry();
  operand_entered_ = true;
  last_was_op_ = false;
}

void engine::exponent() {
  if (programmer())
    return;
  begin_input();
  if (!entry_) {
    std::string base = "1";
    if (operand_entered_) {
      std::string current = format_real(as_real(current_));
      if (current.find('e') == std::string::npos)
        base = current;
    }
    entry_ = base;
    current_label_.clear();
  }
  if (entry_->find('e') == std::string::npos)
    *entry_ += "e+";
  sync_entry();
  operand_entered_ = true;
  last_was_op_ = false;
}

void engine::backspace() {
  if (error_) {
    clear();
    return;
  }
  if (just_evaluated_) {
    finished_expr_.clear();
    return;
  }
  if (!entry_)
    return;
  std::string& e = *entry_;
  if (!e.empty())
    e.pop_back();
  if (e.size() >= 2 && e[e.size() - 2] == 'e' && (e.back() == '+' || e.back() == '-'))
    e.resize(e.size() - 2);
  if (e.empty() || e == "-")
    e = "0";
  sync_entry();
}

void engine::negate() {
  if (error_)
    return;
  if (entry_ && !programmer()) {
    std::string& e = *entry_;
    auto epos = e.find('e');
    if (epos != std::string::npos) {
      e[epos + 1] = e[epos + 1] == '+' ? '-' : '+';
    } else if (e == "0" || e.empty()) {
      return;
    } else if (e[0] == '-') {
      e.erase(0, 1);
    } else {
      e.insert(0, "-");
    }
    sync_entry();
    return;
  }
  unary(unary_op::negate);
}

void engine::clear_entry() {
  if (error_ || just_evaluated_) {
    clear();
    return;
  }
  entry_.reset();
  current_ = zero();
  current_label_.clear();
  operand_entered_ = false;
}

void engine::clear() {
  error_.reset();
  entry_.reset();
  current_ = zero();
  current_label_.clear();
  operand_entered_ = false;
  last_was_op_ = false;
  just_evaluated_ = false;
  values_.clear();
  ops_.clear();
  expr_levels_ = {std::string{}};
  finished_expr_.clear();
  last_op_.reset();
}

void engine::constant(double v, const std::string& label) {
  begin_input();
  set_current(coerce(make_real(v)), programmer() ? std::string{} : label);
}

void engine::random() {
  begin_input();
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  set_current(coerce(make_real(dist(rng_))), std::string{});
}

// ── Operators ─────────────────────────────────────────────────────────────────

int engine::precedence(binary_op op) const {
  if (mode_ == mode::standard)
    return 1; // immediate execution: strictly left to right
  switch (op) {
    case binary_op::power:
    case binary_op::root:
    case binary_op::log_base:
      return 7;
    case binary_op::multiply:
    case binary_op::divide:
    case binary_op::modulo:
      return 6;
    case binary_op::add:
    case binary_op::subtract:
      return 5;
    case binary_op::shift_left:
    case binary_op::shift_right:
      return 4;
    case binary_op::bit_and:
    case binary_op::bit_nand:
      return 3;
    case binary_op::bit_xor:
      return 2;
    case binary_op::bit_or:
    case binary_op::bit_nor:
      return 1;
  }
  return 1;
}

std::optional<binary_op> engine::top_binary() const {
  if (ops_.empty() || ops_.back().paren)
    return std::nullopt;
  return ops_.back().op;
}

bool engine::reduce_one() {
  binary_op op = ops_.back().op;
  ops_.pop_back();
  value rhs = values_.back();
  values_.pop_back();
  value lhs = values_.back();
  values_.pop_back();
  auto result = apply(op, lhs, rhs);
  if (!result)
    return false;
  values_.push_back(*result);
  return true;
}

bool engine::reduce_while(int min_precedence, bool right_assoc) {
  while (auto top = top_binary()) {
    int p = precedence(*top);
    if (p < min_precedence || (p == min_precedence && right_assoc))
      break;
    if (!reduce_one())
      return false;
  }
  return true;
}

bool engine::reduce_to_paren() {
  while (!ops_.empty() && !ops_.back().paren) {
    if (!reduce_one())
      return false;
  }
  if (!ops_.empty())
    ops_.pop_back();
  return true;
}

void engine::binary(binary_op op) {
  if (error_)
    return;
  commit_entry();
  if (just_evaluated_) {
    // Continue from the result: "= + 2".
    just_evaluated_ = false;
    finished_expr_.clear();
  }

  if (last_was_op_ && top_binary()) {
    // A second operator in a row replaces the first.
    ops_.back().op = op;
    expr_levels_.back().resize(last_op_text_pos_);
    expr_levels_.back() += op_symbol(op) + " ";
    return;
  }

  values_.push_back(current_);
  expr_levels_.back() += label_of_current() + " ";
  bool right_assoc = mode_ != mode::standard && (op == binary_op::power || op == binary_op::root);
  if (!reduce_while(precedence(op), right_assoc))
    return;
  ops_.push_back({false, op});
  last_op_text_pos_ = expr_levels_.back().size();
  expr_levels_.back() += op_symbol(op) + " ";

  // Show the running value: what has been reduced so far.
  current_ = values_.back();
  current_label_.clear();
  operand_entered_ = false;
  last_was_op_ = true;
}

void engine::unary(unary_op op) {
  if (error_)
    return;
  commit_entry();
  if (just_evaluated_) {
    just_evaluated_ = false;
    finished_expr_.clear();
  }
  std::string label = unary_label(op, label_of_current());
  auto result = apply(op, current_);
  if (!result)
    return;
  set_current(*result, std::move(label));
}

void engine::percent() {
  if (error_)
    return;
  commit_entry();
  if (just_evaluated_) {
    just_evaluated_ = false;
    finished_expr_.clear();
  }
  double x = as_real(current_);
  auto top = top_binary();
  double r = x / 100.0;
  if (top && (*top == binary_op::add || *top == binary_op::subtract) && !values_.empty())
    r = as_real(values_.back()) * x / 100.0;
  value v = coerce(make_real(r));
  set_current(v, format(v, radix(), false));
}

void engine::open_paren() {
  begin_input();
  if (operand_entered_)
    binary(binary_op::multiply); // implicit multiplication: 2(3) = 6
  if (error_)
    return;
  commit_entry();
  ops_.push_back({true, binary_op::add});
  expr_levels_.emplace_back();
  current_label_.clear();
  operand_entered_ = false;
  last_was_op_ = false;
}

void engine::close_paren() {
  if (error_ || open_parens() == 0)
    return;
  commit_entry();
  values_.push_back(current_);
  std::string inner = expr_levels_.back() + label_of_current();
  if (!reduce_to_paren())
    return;
  expr_levels_.pop_back();
  value v = values_.back();
  values_.pop_back();
  set_current(v, "(" + inner + ")");
}

void engine::equals() {
  if (error_)
    return;
  commit_entry();

  if (just_evaluated_) {
    // Repeat the last operation: 5 + 3 = = gives 11.
    if (!last_op_)
      return;
    std::string expr = label_of_current() + " " + op_symbol(*last_op_) + " " + last_rhs_label_ + " =";
    auto result = apply(*last_op_, current_, last_rhs_);
    if (result)
      record(expr, *result);
    return;
  }

  if (ops_.empty()) {
    std::string expr = label_of_current() + " =";
    last_op_.reset();
    record(expr, current_);
    return;
  }

  std::string rhs_label = label_of_current();
  std::string expr;
  for (std::size_t i = 0; i < expr_levels_.size(); ++i) {
    if (i > 0)
      expr += "(";
    expr += expr_levels_[i];
  }
  expr += rhs_label + std::string(static_cast<std::size_t>(open_parens()), ')') + " =";

  auto top = top_binary();
  value rhs = current_;
  values_.push_back(current_);
  while (!ops_.empty()) {
    if (ops_.back().paren) {
      ops_.pop_back();
      continue;
    }
    if (!reduce_one())
      return;
  }
  value result = values_.back();
  values_.clear();
  expr_levels_ = {std::string{}};

  last_op_ = top;
  last_rhs_ = rhs;
  last_rhs_label_ = rhs_label;
  record(expr, result);
}

void engine::record(const std::string& expr, const value& result) {
  entry_.reset();
  current_ = result;
  current_label_.clear();
  finished_expr_ = expr;
  just_evaluated_ = true;
  operand_entered_ = false;
  last_was_op_ = false;
  history_.push_front({expr, format(result, radix(), grouping_), result});
  while (history_.size() > kMaxHistory)
    history_.pop_back();
}

// ── Arithmetic ────────────────────────────────────────────────────────────────

double engine::to_radians(double x) const {
  switch (angle_) {
    case angle_unit::degrees:
      return x * kPi / 180.0;
    case angle_unit::gradians:
      return x * kPi / 200.0;
    case angle_unit::radians:
      break;
  }
  return x;
}

double engine::from_radians(double x) const {
  switch (angle_) {
    case angle_unit::degrees:
      return x * 180.0 / kPi;
    case angle_unit::gradians:
      return x * 200.0 / kPi;
    case angle_unit::radians:
      break;
  }
  return x;
}

std::optional<value> engine::apply(binary_op op, const value& a, const value& b) {
  bool bitwise = op == binary_op::shift_left || op == binary_op::shift_right || op == binary_op::bit_and ||
      op == binary_op::bit_or || op == binary_op::bit_xor || op == binary_op::bit_nand || op == binary_op::bit_nor;

  if (programmer() || bitwise) {
    // Bitwise operators in a decimal mode work on the 64-bit integer parts.
    auto int_of = [&](const value& v) {
      if (programmer())
        return as_int(v);
      double d = std::trunc(as_real(v));
      return std::isfinite(d) && std::fabs(d) < 9.2e18 ? static_cast<int64_t>(d) : int64_t{0};
    };
    int64_t x = int_of(a);
    int64_t y = int_of(b);
    auto ux = static_cast<uint64_t>(x);
    auto uy = static_cast<uint64_t>(y);
    int width = programmer() ? bits() : 64;
    std::optional<int64_t> r;
    switch (op) {
      case binary_op::add:
        r = static_cast<int64_t>(ux + uy);
        break;
      case binary_op::subtract:
        r = static_cast<int64_t>(ux - uy);
        break;
      case binary_op::multiply:
        r = static_cast<int64_t>(ux * uy);
        break;
      case binary_op::divide:
      case binary_op::modulo:
        if (y == 0) {
          fail(op == binary_op::divide && x == 0 ? kUndefined : kDivByZero);
          return std::nullopt;
        }
        if (y == -1)
          r = op == binary_op::divide ? static_cast<int64_t>(uint64_t{0} - ux) : int64_t{0};
        else
          r = op == binary_op::divide ? x / y : x % y;
        break;
      case binary_op::power: {
        if (y < 0) {
          r = (x == 1) ? 1 : (x == -1) ? ((y % 2) ? -1 : 1) : 0;
          if (x == 0) {
            fail(kDivByZero);
            return std::nullopt;
          }
          break;
        }
        uint64_t base = ux;
        uint64_t acc = 1;
        for (uint64_t e = uy; e != 0; e >>= 1) {
          if (e & 1)
            acc *= base;
          base *= base;
        }
        r = static_cast<int64_t>(acc);
        break;
      }
      case binary_op::root:
      case binary_op::log_base:
        break; // computed in floating point below
      case binary_op::shift_left:
        r = (y < 0 || y >= width) ? int64_t{0} : static_cast<int64_t>(ux << y);
        break;
      case binary_op::shift_right:
        r = (y < 0 || y >= 64) ? (x < 0 ? int64_t{-1} : int64_t{0}) : (x >> y);
        break;
      case binary_op::bit_and:
        r = static_cast<int64_t>(ux & uy);
        break;
      case binary_op::bit_or:
        r = static_cast<int64_t>(ux | uy);
        break;
      case binary_op::bit_xor:
        r = static_cast<int64_t>(ux ^ uy);
        break;
      case binary_op::bit_nand:
        r = static_cast<int64_t>(~(ux & uy));
        break;
      case binary_op::bit_nor:
        r = static_cast<int64_t>(~(ux | uy));
        break;
    }
    if (r)
      return programmer() ? make_int(*r) : make_real(static_cast<double>(*r));
  }

  double x = as_real(a);
  double y = as_real(b);
  double r = 0.0;
  switch (op) {
    case binary_op::add:
      r = x + y;
      break;
    case binary_op::subtract:
      r = x - y;
      break;
    case binary_op::multiply:
      r = x * y;
      break;
    case binary_op::divide:
      if (y == 0.0) {
        fail(x == 0.0 ? kUndefined : kDivByZero);
        return std::nullopt;
      }
      r = x / y;
      break;
    case binary_op::modulo:
      if (y == 0.0) {
        fail(kDivByZero);
        return std::nullopt;
      }
      r = std::fmod(x, y);
      break;
    case binary_op::power:
      if (x == 0.0 && y < 0.0) {
        fail(kDivByZero);
        return std::nullopt;
      }
      r = std::pow(x, y);
      break;
    case binary_op::root:
      if (y == 0.0) {
        fail(kInvalid);
        return std::nullopt;
      }
      if (x < 0.0) {
        // Odd integer roots of negatives are real: yroot(-8, 3) = -2.
        if (!is_integer(y) || std::fmod(std::fabs(y), 2.0) != 1.0) {
          fail(kInvalid);
          return std::nullopt;
        }
        r = -std::pow(-x, 1.0 / y);
      } else {
        r = std::pow(x, 1.0 / y);
      }
      break;
    case binary_op::log_base:
      if (x <= 0.0 || y <= 0.0 || y == 1.0) {
        fail(kInvalid);
        return std::nullopt;
      }
      r = std::log(x) / std::log(y);
      break;
    default:
      break; // bitwise: handled above
  }
  if (std::isnan(r)) {
    fail(kInvalid);
    return std::nullopt;
  }
  if (std::isinf(r)) {
    fail(kOverflow);
    return std::nullopt;
  }
  return coerce(make_real(r));
}

std::optional<value> engine::apply(unary_op op, const value& a) {
  if (programmer()) {
    auto ux = static_cast<uint64_t>(as_int(a));
    uint64_t mask = bits() >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits()) - 1;
    switch (op) {
      case unary_op::negate:
        return make_int(static_cast<int64_t>(uint64_t{0} - ux));
      case unary_op::bit_not:
        return make_int(static_cast<int64_t>(~ux));
      case unary_op::rotate_left: {
        uint64_t u = ux & mask;
        return make_int(static_cast<int64_t>(((u << 1) | (u >> (bits() - 1))) & mask));
      }
      case unary_op::rotate_right: {
        uint64_t u = ux & mask;
        return make_int(static_cast<int64_t>(((u >> 1) | ((u & 1) << (bits() - 1))) & mask));
      }
      case unary_op::square:
        return make_int(static_cast<int64_t>(ux * ux));
      case unary_op::cube:
        return make_int(static_cast<int64_t>(ux * ux * ux));
      case unary_op::abs:
        return make_int(as_int(a) < 0 ? static_cast<int64_t>(uint64_t{0} - ux) : as_int(a));
      default:
        break; // everything else: floating point, then truncated
    }
  } else if (op == unary_op::bit_not || op == unary_op::rotate_left || op == unary_op::rotate_right) {
    double d = std::trunc(as_real(a));
    auto i = std::isfinite(d) && std::fabs(d) < 9.2e18 ? static_cast<int64_t>(d) : int64_t{0};
    auto u = static_cast<uint64_t>(i);
    uint64_t r = op == unary_op::bit_not ? ~u
        : op == unary_op::rotate_left    ? (u << 1) | (u >> 63)
                                         : (u >> 1) | (u << 63);
    return make_real(static_cast<double>(static_cast<int64_t>(r)));
  }

  double x = as_real(a);
  double r = 0.0;
  auto invalid = [&]() -> std::optional<value> {
    fail(kInvalid);
    return std::nullopt;
  };

  // Trig in degrees / gradians: reduce to [0, 360) degrees first so the
  // quadrant angles come out exact (sin 180 = 0, tan 90 is an error).
  auto exact_trig = [&](unary_op f) -> std::optional<double> {
    if (angle_ == angle_unit::radians)
      return std::nullopt;
    double deg = angle_ == angle_unit::degrees ? x : x * 0.9;
    double m = std::fmod(deg, 360.0);
    if (m < 0)
      m += 360.0;
    if (m != 0.0 && m != 90.0 && m != 180.0 && m != 270.0)
      return std::nullopt;
    int q = static_cast<int>(m / 90.0);
    static constexpr double kSin[] = {0.0, 1.0, 0.0, -1.0};
    static constexpr double kCos[] = {1.0, 0.0, -1.0, 0.0};
    if (f == unary_op::sin)
      return kSin[q];
    if (f == unary_op::cos)
      return kCos[q];
    return (q % 2) ? std::numeric_limits<double>::quiet_NaN() : 0.0; // tan
  };

  switch (op) {
    case unary_op::negate:
      r = -x;
      break;
    case unary_op::reciprocal:
      if (x == 0.0) {
        fail(kDivByZero);
        return std::nullopt;
      }
      r = 1.0 / x;
      break;
    case unary_op::square:
      r = x * x;
      break;
    case unary_op::cube:
      r = x * x * x;
      break;
    case unary_op::sqrt:
      if (x < 0.0)
        return invalid();
      r = std::sqrt(x);
      break;
    case unary_op::cbrt:
      r = std::cbrt(x);
      break;
    case unary_op::pow10:
      r = std::pow(10.0, x);
      break;
    case unary_op::pow2:
      r = std::exp2(x);
      break;
    case unary_op::exp:
      r = std::exp(x);
      break;
    case unary_op::ln:
    case unary_op::log10:
    case unary_op::log2:
      if (x <= 0.0)
        return invalid();
      r = op == unary_op::ln ? std::log(x) : op == unary_op::log10 ? std::log10(x) : std::log2(x);
      break;
    case unary_op::abs:
      r = std::fabs(x);
      break;
    case unary_op::factorial:
      if (x < 0.0 && is_integer(x))
        return invalid();
      if (x > 170.0) {
        fail(kOverflow);
        return std::nullopt;
      }
      if (is_integer(x)) {
        r = 1.0;
        for (int i = 2; i <= static_cast<int>(x); ++i)
          r *= i;
      } else {
        r = std::tgamma(x + 1.0);
      }
      break;
    case unary_op::floor:
      r = std::floor(x);
      break;
    case unary_op::ceil:
      r = std::ceil(x);
      break;
    case unary_op::sin:
    case unary_op::cos:
    case unary_op::tan:
      if (auto exact = exact_trig(op)) {
        r = *exact;
      } else {
        double rad = to_radians(x);
        r = op == unary_op::sin ? std::sin(rad) : op == unary_op::cos ? std::cos(rad) : std::tan(rad);
      }
      break;
    case unary_op::asin:
    case unary_op::acos:
      if (x < -1.0 || x > 1.0)
        return invalid();
      r = from_radians(op == unary_op::asin ? std::asin(x) : std::acos(x));
      break;
    case unary_op::atan:
      r = from_radians(std::atan(x));
      break;
    case unary_op::sinh:
      r = std::sinh(x);
      break;
    case unary_op::cosh:
      r = std::cosh(x);
      break;
    case unary_op::tanh:
      r = std::tanh(x);
      break;
    case unary_op::asinh:
      r = std::asinh(x);
      break;
    case unary_op::acosh:
      if (x < 1.0)
        return invalid();
      r = std::acosh(x);
      break;
    case unary_op::atanh:
      if (x <= -1.0 || x >= 1.0)
        return invalid();
      r = std::atanh(x);
      break;
    case unary_op::bit_not:
    case unary_op::rotate_left:
    case unary_op::rotate_right:
      break; // handled above
  }
  if (std::isnan(r))
    return invalid();
  if (std::isinf(r)) {
    fail(kOverflow);
    return std::nullopt;
  }
  return coerce(make_real(r));
}

// ── Memory and history ────────────────────────────────────────────────────────

void engine::memory_clear() {
  memory_.reset();
}

void engine::memory_recall() {
  if (!memory_)
    return;
  begin_input();
  set_current(coerce(*memory_), std::string{});
}

void engine::memory_store() {
  if (error_)
    return;
  commit_entry();
  memory_ = current_;
}

void engine::memory_add() {
  if (error_)
    return;
  commit_entry();
  if (auto r = apply(binary_op::add, memory_ ? coerce(*memory_) : zero(), current_))
    memory_ = *r;
}

void engine::memory_subtract() {
  if (error_)
    return;
  commit_entry();
  if (auto r = apply(binary_op::subtract, memory_ ? coerce(*memory_) : zero(), current_))
    memory_ = *r;
}

void engine::recall_history(std::size_t index) {
  if (index >= history_.size())
    return;
  value v = history_[index].result_value;
  begin_input();
  set_current(coerce(v), std::string{});
}

// ── Settings ──────────────────────────────────────────────────────────────────

void engine::set_mode(mode m) {
  if (m == mode_)
    return;
  value v = error_ ? value{} : current_;
  clear();
  mode_ = m;
  current_ = coerce(v);
}

void engine::set_radix(int radix) {
  if (radix != 2 && radix != 8 && radix != 10 && radix != 16)
    return;
  commit_entry();
  radix_ = radix;
}

void engine::set_word_size(word_size w) {
  commit_entry();
  word_ = w;
  if (programmer() && !error_)
    current_ = make_int(current_.integer);
}

} // namespace bdg::wish::calc
