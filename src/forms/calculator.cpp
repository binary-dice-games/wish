// MIT License © 2025 Binary Dice Games
/// @file calculator.cpp
/// @brief Implementation of the Calculator form.
#include "calculator.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui_importer.hpp>

#include <cmath>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {
template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}
} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// ImGuiWindowFlags_NoResize = 1<<1 = 2

static constexpr const char* kLayout = R"({
  "type": "Window",
  "title": "Calculator",
  "width": 328,
  "height": 420,
  "flags": "NoResize",
  "closable": true,
  "children": {
    "display": { "type": "Label", "text": "0", "font_path": "fonts/default.ttf", "font_size": 32 },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",   "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "div": { "type": "Button", "label": "/",   "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "mul": { "type": "Button", "label": "*",   "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "bsp": { "type": "Button", "label": "<-",  "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 }
      }
    },
    "row1": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n7":  { "type": "Button", "label": "7", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "n8":  { "type": "Button", "label": "8", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "n9":  { "type": "Button", "label": "9", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "sub": { "type": "Button", "label": "-", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 }
      }
    },
    "row2": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n4":  { "type": "Button", "label": "4", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "n5":  { "type": "Button", "label": "5", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "n6":  { "type": "Button", "label": "6", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "add": { "type": "Button", "label": "+", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 }
      }
    },
    "row3": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n1": { "type": "Button", "label": "1", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "n2": { "type": "Button", "label": "2", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "n3": { "type": "Button", "label": "3", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "eq": { "type": "Button", "label": "=", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 }
      }
    },
    "row4": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n0":  { "type": "Button", "label": "0",   "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "dot": { "type": "Button", "label": ".",   "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "pm":  { "type": "Button", "label": "+/-", "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 },
        "pct": { "type": "Button", "label": "%",   "width": 72, "height": 52, "font_path": "fonts/default.ttf", "font_size": 24 }
      }
    }
  }
})";

// ── calculator ────────────────────────────────────────────────────────────────

calculator::calculator(dynamic&& base) : form(std::move(base)) {}

void calculator::on_init() {
  internal_root_key_ = "__calc_" + std::to_string(reinterpret_cast<uintptr_t>(this));

  auto tree = import_json(kLayout);

  // Assign every element a bison RMI ID.
  auto& objects = ctx().objects;
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    objects[id.id] = elem;
    elem["__wish_id"_key] = id;
  }

  // Cache IDs for interactive widgets.
  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();

  tree.with("display", [&](const auto& e) {
    display_id_ = wish_id_of(e);
    display_ptr_ = e;
  });

  tree.with("row0.c", [&](const auto& e) { btn_c_ = wish_id_of(e); });
  tree.with("row0.div", [&](const auto& e) { btn_div_ = wish_id_of(e); });
  tree.with("row0.mul", [&](const auto& e) { btn_mul_ = wish_id_of(e); });
  tree.with("row0.bsp", [&](const auto& e) { btn_bsp_ = wish_id_of(e); });

  tree.with("row1.n7", [&](const auto& e) { btn_n7_ = wish_id_of(e); });
  tree.with("row1.n8", [&](const auto& e) { btn_n8_ = wish_id_of(e); });
  tree.with("row1.n9", [&](const auto& e) { btn_n9_ = wish_id_of(e); });
  tree.with("row1.sub", [&](const auto& e) { btn_sub_ = wish_id_of(e); });

  tree.with("row2.n4", [&](const auto& e) { btn_n4_ = wish_id_of(e); });
  tree.with("row2.n5", [&](const auto& e) { btn_n5_ = wish_id_of(e); });
  tree.with("row2.n6", [&](const auto& e) { btn_n6_ = wish_id_of(e); });
  tree.with("row2.add", [&](const auto& e) { btn_add_ = wish_id_of(e); });

  tree.with("row3.n1", [&](const auto& e) { btn_n1_ = wish_id_of(e); });
  tree.with("row3.n2", [&](const auto& e) { btn_n2_ = wish_id_of(e); });
  tree.with("row3.n3", [&](const auto& e) { btn_n3_ = wish_id_of(e); });
  tree.with("row3.eq", [&](const auto& e) { btn_eq_ = wish_id_of(e); });

  tree.with("row4.n0", [&](const auto& e) { btn_n0_ = wish_id_of(e); });
  tree.with("row4.dot", [&](const auto& e) { btn_dot_ = wish_id_of(e); });
  tree.with("row4.pm", [&](const auto& e) { btn_pm_ = wish_id_of(e); });
  tree.with("row4.pct", [&](const auto& e) { btn_pct_ = wish_id_of(e); });

  sess().objects.merge(std::move(tree), internal_root_key_);
}

// ── Event routing ─────────────────────────────────────────────────────────────

void calculator::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  // Window X button — forward to the client and clean up.
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (event != "clicked"_key)
    return;

  if (id == btn_c_) {
    handle_clear();
    return;
  }
  if (id == btn_div_) {
    handle_operator('/');
    return;
  }
  if (id == btn_mul_) {
    handle_operator('*');
    return;
  }
  if (id == btn_bsp_) {
    handle_backspace();
    return;
  }
  if (id == btn_n7_) {
    handle_digit("7");
    return;
  }
  if (id == btn_n8_) {
    handle_digit("8");
    return;
  }
  if (id == btn_n9_) {
    handle_digit("9");
    return;
  }
  if (id == btn_sub_) {
    handle_operator('-');
    return;
  }
  if (id == btn_n4_) {
    handle_digit("4");
    return;
  }
  if (id == btn_n5_) {
    handle_digit("5");
    return;
  }
  if (id == btn_n6_) {
    handle_digit("6");
    return;
  }
  if (id == btn_add_) {
    handle_operator('+');
    return;
  }
  if (id == btn_n1_) {
    handle_digit("1");
    return;
  }
  if (id == btn_n2_) {
    handle_digit("2");
    return;
  }
  if (id == btn_n3_) {
    handle_digit("3");
    return;
  }
  if (id == btn_eq_) {
    handle_equals();
    return;
  }
  if (id == btn_n0_) {
    handle_digit("0");
    return;
  }
  if (id == btn_dot_) {
    handle_dot();
    return;
  }
  if (id == btn_pm_) {
    handle_negate();
    return;
  }
  if (id == btn_pct_) {
    handle_percent();
    return;
  }
}

// ── Calculator logic ──────────────────────────────────────────────────────────

void calculator::handle_digit(const std::string& ch) {
  if (fresh_) {
    display_ = ch;
    fresh_ = false;
  } else if (display_.size() < 15) {
    display_ += ch;
  }
  update_display();
}

void calculator::handle_dot() {
  if (fresh_) {
    display_ = "0.";
    fresh_ = false;
  } else if (display_.find('.') == std::string::npos) {
    display_ += '.';
  }
  update_display();
}

void calculator::handle_operator(char op) {
  if (pending_op_ && !fresh_) {
    handle_equals();
  }
  operand_ = std::stod(display_);
  pending_op_ = op;
  fresh_ = true;
}

void calculator::handle_equals() {
  if (!pending_op_)
    return;
  double rhs = std::stod(display_);
  double result = operand_;
  switch (pending_op_) {
    case '+':
      result += rhs;
      break;
    case '-':
      result -= rhs;
      break;
    case '*':
      result *= rhs;
      break;
    case '/':
      result = (rhs != 0.0) ? result / rhs : 0.0;
      break;
  }
  pending_op_ = 0;
  fresh_ = true;

  if (result == std::floor(result) && std::abs(result) < 1e12) {
    display_ = std::to_string(static_cast<long long>(result));
  } else {
    std::ostringstream oss;
    oss << result;
    display_ = oss.str();
    if (display_.size() > 15)
      display_.resize(15);
  }
  update_display();
}

void calculator::handle_clear() {
  display_ = "0";
  operand_ = 0.0;
  pending_op_ = 0;
  fresh_ = true;
  update_display();
}

void calculator::handle_negate() {
  if (display_ == "0")
    return;
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
  if (display_.size() > 15)
    display_.resize(15);
  update_display();
}

void calculator::handle_backspace() {
  if (fresh_) {
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
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Self-contained four-function calculator. "
                        "All arithmetic is server-side. "
                        "Listen for the 'closed' event to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<calculator>("wish"_key, "Calculator"_key));
}

} // namespace bdg::wish
