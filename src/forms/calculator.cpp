// MIT License © 2025 Binary Dice Games
/// @file calculator.cpp
/// @brief Implementation of the Calculator form.
#include "calculator.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <wish/ui_importer.hpp>

#include <cmath>
#include <sstream>

namespace bdg::wish {

using namespace bison;

// ── UI layout ─────────────────────────────────────────────────────────────────

static constexpr const char* kLayout = R"({
  "type": "Window",
  "title": "Calculator",
  "width": 265,
  "height": 385,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "display": { "type": "Label", "text": "0" },
        "row_fn": {
          "type": "HorizontalLayout",
          "children": {
            "btn_c":   { "type": "Button", "label": "C",   "width": 60 },
            "btn_pm":  { "type": "Button", "label": "+/-", "width": 60 },
            "btn_pct": { "type": "Button", "label": "%",   "width": 60 },
            "btn_bs":  { "type": "Button", "label": "<",   "width": 60 }
          }
        },
        "row1": {
          "type": "HorizontalLayout",
          "children": {
            "btn7":    { "type": "Button", "label": "7", "width": 60 },
            "btn8":    { "type": "Button", "label": "8", "width": 60 },
            "btn9":    { "type": "Button", "label": "9", "width": 60 },
            "btn_div": { "type": "Button", "label": "/", "width": 60 }
          }
        },
        "row2": {
          "type": "HorizontalLayout",
          "children": {
            "btn4":    { "type": "Button", "label": "4", "width": 60 },
            "btn5":    { "type": "Button", "label": "5", "width": 60 },
            "btn6":    { "type": "Button", "label": "6", "width": 60 },
            "btn_mul": { "type": "Button", "label": "*", "width": 60 }
          }
        },
        "row3": {
          "type": "HorizontalLayout",
          "children": {
            "btn1":    { "type": "Button", "label": "1", "width": 60 },
            "btn2":    { "type": "Button", "label": "2", "width": 60 },
            "btn3":    { "type": "Button", "label": "3", "width": 60 },
            "btn_sub": { "type": "Button", "label": "-", "width": 60 }
          }
        },
        "row4": {
          "type": "HorizontalLayout",
          "children": {
            "btn0":    { "type": "Button", "label": "0", "width": 60 },
            "btn_dot": { "type": "Button", "label": ".", "width": 60 },
            "btn_eq":  { "type": "Button", "label": "=", "width": 60 },
            "btn_add": { "type": "Button", "label": "+", "width": 60 }
          }
        },
        "row_close": {
          "type": "HorizontalLayout",
          "children": {
            "btn_close": { "type": "Button", "label": "Close", "width": 245 }
          }
        }
      }
    }
  }
})";

// ── calculator ────────────────────────────────────────────────────────────────

calculator::calculator(dynamic&& base) : form(std::move(base)) {}

void calculator::on_init() {
  internal_root_key_ =
      "__calc_" + std::to_string(reinterpret_cast<uintptr_t>(this));

  auto tree = import_json(kLayout);

  auto& objects = ctx().objects;
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    objects[id.id] = elem;
    elem["__wish_id"_key] = id;
  }

  // Cache IDs for every interactive widget.
  auto cache = [&](const char* path, key_t& out_id,
                    ui_element_ptr* out_ptr = nullptr) {
    tree.with(path, [&](const auto& e) {
      out_id = e->as<key_t>("__wish_id"_key);
      if (out_ptr) *out_ptr = e;
    });
  };

  cache("vbox.display",           display_id_,  &display_ptr_);
  cache("vbox.row_close.btn_close", btn_close_id_);
  cache("vbox.row_fn.btn_c",      btn_c_);
  cache("vbox.row_fn.btn_pm",     btn_pm_);
  cache("vbox.row_fn.btn_pct",    btn_pct_);
  cache("vbox.row_fn.btn_bs",     btn_bs_);
  cache("vbox.row1.btn7",         btn_digit_[7]);
  cache("vbox.row1.btn8",         btn_digit_[8]);
  cache("vbox.row1.btn9",         btn_digit_[9]);
  cache("vbox.row1.btn_div",      btn_div_);
  cache("vbox.row2.btn4",         btn_digit_[4]);
  cache("vbox.row2.btn5",         btn_digit_[5]);
  cache("vbox.row2.btn6",         btn_digit_[6]);
  cache("vbox.row2.btn_mul",      btn_mul_);
  cache("vbox.row3.btn1",         btn_digit_[1]);
  cache("vbox.row3.btn2",         btn_digit_[2]);
  cache("vbox.row3.btn3",         btn_digit_[3]);
  cache("vbox.row3.btn_sub",      btn_sub_);
  cache("vbox.row4.btn0",         btn_digit_[0]);
  cache("vbox.row4.btn_dot",      btn_dot_);
  cache("vbox.row4.btn_eq",       btn_eq_);
  cache("vbox.row4.btn_add",      btn_add_);

  sess().objects.merge(std::move(tree), internal_root_key_);
}

// ── Event routing ─────────────────────────────────────────────────────────────

void calculator::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (event != "clicked"_key) return;

  if (id == btn_close_id_) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }
  if (id == btn_c_)   { handle_clear();    return; }
  if (id == btn_pm_)  { handle_negate();   return; }
  if (id == btn_pct_) { handle_percent();  return; }
  if (id == btn_bs_)  { handle_backspace(); return; }
  if (id == btn_dot_) { handle_dot();      return; }
  if (id == btn_eq_)  { handle_equals();   return; }
  if (id == btn_add_) { handle_operator('+'); return; }
  if (id == btn_sub_) { handle_operator('-'); return; }
  if (id == btn_mul_) { handle_operator('*'); return; }
  if (id == btn_div_) { handle_operator('/'); return; }
  for (int d = 0; d <= 9; ++d) {
    if (id == btn_digit_[d]) { handle_digit(static_cast<char>('0' + d)); return; }
  }
}

// ── Calculator logic ──────────────────────────────────────────────────────────

void calculator::handle_digit(char d) {
  if (has_result_) {
    display_ = std::string{d};
    has_result_ = false;
  } else if (display_ == "0") {
    display_ = std::string{d};
  } else if (display_.size() < 15) {
    display_ += d;
  }
  update_display();
}

void calculator::handle_dot() {
  if (has_result_) {
    display_ = "0.";
    has_result_ = false;
  } else if (display_.find('.') == std::string::npos) {
    display_ += '.';
  }
  update_display();
}

void calculator::handle_operator(char op) {
  if (pending_op_ && !has_result_) {
    // Chain: apply pending operation first.
    handle_equals();
  }
  operand_ = std::stod(display_);
  pending_op_ = op;
  has_result_ = true;
}

void calculator::handle_equals() {
  if (!pending_op_) return;
  double rhs = std::stod(display_);
  double result = operand_;
  switch (pending_op_) {
    case '+': result += rhs; break;
    case '-': result -= rhs; break;
    case '*': result *= rhs; break;
    case '/': result = (rhs != 0.0) ? result / rhs : 0.0; break;
  }
  pending_op_ = 0;
  has_result_ = true;

  // Format: prefer integer string when result is whole.
  if (result == std::floor(result) && std::abs(result) < 1e15) {
    display_ = std::to_string(static_cast<long long>(result));
  } else {
    std::ostringstream oss;
    oss << result;
    display_ = oss.str();
    if (display_.size() > 15) display_.resize(15);
  }
  update_display();
}

void calculator::handle_clear() {
  display_ = "0";
  operand_ = 0.0;
  pending_op_ = 0;
  has_result_ = false;
  update_display();
}

void calculator::handle_negate() {
  if (display_ == "0") return;
  if (display_[0] == '-')
    display_.erase(0, 1);
  else
    display_.insert(0, "-");
  update_display();
}

void calculator::handle_percent() {
  double v = std::stod(display_) / 100.0;
  std::ostringstream oss;
  oss << v;
  display_ = oss.str();
  if (display_.size() > 15) display_.resize(15);
  update_display();
}

void calculator::handle_backspace() {
  if (has_result_) {
    handle_clear();
    return;
  }
  if (display_.size() > 1)
    display_.pop_back();
  else
    display_ = "0";
  update_display();
}

void calculator::update_display() {
  if (display_ptr_)
    (*display_ptr_)["text"_key] = display_;
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_calculator() {
  auto proto = dynamic_ptr{"Calculator"_key, {}};

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Calculator"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Self-contained four-function calculator. "
      "All arithmetic is server-side. "
      "Listen for the 'closed' event to detect when the user is done."));

  dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      dynamic::make_factory<calculator>("wish"_key, "Calculator"_key));
}

} // namespace bdg::wish
