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
    "display": { "type": "Label", "text": "0", "font_size": 32 },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",   "width": 72, "height": 52, "font_size": 24 },
        "div": { "type": "Button", "label": "/",   "width": 72, "height": 52, "font_size": 24 },
        "mul": { "type": "Button", "label": "*",   "width": 72, "height": 52, "font_size": 24 },
        "bsp": { "type": "Button", "label": "<-",  "width": 72, "height": 52, "font_size": 24 }
      }
    },
    "row1": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n7":  { "type": "Button", "label": "7", "width": 72, "height": 52, "font_size": 24 },
        "n8":  { "type": "Button", "label": "8", "width": 72, "height": 52, "font_size": 24 },
        "n9":  { "type": "Button", "label": "9", "width": 72, "height": 52, "font_size": 24 },
        "sub": { "type": "Button", "label": "-", "width": 72, "height": 52, "font_size": 24 }
      }
    },
    "row2": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n4":  { "type": "Button", "label": "4", "width": 72, "height": 52, "font_size": 24 },
        "n5":  { "type": "Button", "label": "5", "width": 72, "height": 52, "font_size": 24 },
        "n6":  { "type": "Button", "label": "6", "width": 72, "height": 52, "font_size": 24 },
        "add": { "type": "Button", "label": "+", "width": 72, "height": 52, "font_size": 24 }
      }
    },
    "row3": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n1": { "type": "Button", "label": "1", "width": 72, "height": 52, "font_size": 24 },
        "n2": { "type": "Button", "label": "2", "width": 72, "height": 52, "font_size": 24 },
        "n3": { "type": "Button", "label": "3", "width": 72, "height": 52, "font_size": 24 },
        "eq": { "type": "Button", "label": "=", "width": 72, "height": 52, "font_size": 24 }
      }
    },
    "row4": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n0":  { "type": "Button", "label": "0",   "width": 72, "height": 52, "font_size": 24 },
        "dot": { "type": "Button", "label": ".",   "width": 72, "height": 52, "font_size": 24 },
        "pm":  { "type": "Button", "label": "+/-", "width": 72, "height": 52, "font_size": 24 },
        "pct": { "type": "Button", "label": "%",   "width": 72, "height": 52, "font_size": 24 }
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

  // Look up a button by its dot-path, cache its ID in id_out, and register
  // its click handler in button_handlers_.
  auto bind_button = [&](const std::string& path, key_t& id_out, std::function<void()> handler) {
    tree.with(path, [&](const auto& e) { id_out = wish_id_of(e); });
    button_handlers_[id_out] = std::move(handler);
  };

  bind_button("row0.c", btn_c_, [this] { handle_clear(); });
  bind_button("row0.div", btn_div_, [this] { handle_operator('/'); });
  bind_button("row0.mul", btn_mul_, [this] { handle_operator('*'); });
  bind_button("row0.bsp", btn_bsp_, [this] { handle_backspace(); });

  bind_button("row1.n7", btn_n7_, [this] { handle_digit("7"); });
  bind_button("row1.n8", btn_n8_, [this] { handle_digit("8"); });
  bind_button("row1.n9", btn_n9_, [this] { handle_digit("9"); });
  bind_button("row1.sub", btn_sub_, [this] { handle_operator('-'); });

  bind_button("row2.n4", btn_n4_, [this] { handle_digit("4"); });
  bind_button("row2.n5", btn_n5_, [this] { handle_digit("5"); });
  bind_button("row2.n6", btn_n6_, [this] { handle_digit("6"); });
  bind_button("row2.add", btn_add_, [this] { handle_operator('+'); });

  bind_button("row3.n1", btn_n1_, [this] { handle_digit("1"); });
  bind_button("row3.n2", btn_n2_, [this] { handle_digit("2"); });
  bind_button("row3.n3", btn_n3_, [this] { handle_digit("3"); });
  bind_button("row3.eq", btn_eq_, [this] { handle_equals(); });

  bind_button("row4.n0", btn_n0_, [this] { handle_digit("0"); });
  bind_button("row4.dot", btn_dot_, [this] { handle_dot(); });
  bind_button("row4.pm", btn_pm_, [this] { handle_negate(); });
  bind_button("row4.pct", btn_pct_, [this] { handle_percent(); });

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

  auto it = button_handlers_.find(id);
  if (it != button_handlers_.end())
    it->second();
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
