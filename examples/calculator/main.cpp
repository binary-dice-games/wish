// MIT License © 2025 Binary Dice Games
//
// Calculator example: single-binary wish demo using an in-memory transport.
// The server renders a real SDL3 window; the client (running on the same
// thread as main, after server.start()) drives the UI via the wish RPC layer.
//
// Usage: just run the binary. Close the window to exit.

#include <wish/client.hpp>
#include <wish/server.hpp>
#include <wish/sdl3_renderer.hpp>

#include "src/rmi/rmi.hpp"  // memory_server_transport / memory_client_transport

#include <cmath>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Calculator UI descriptor ──────────────────────────────────────────────────

static constexpr const char* kCalcDesc = R"({
  "type": "Window",
  "title": "Calculator",
  "width": 300,
  "height": 420,
  "children": {
    "display": { "type": "Label", "text": "0" },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout",
      "children": {
        "c":   { "type": "Button", "label": "C"   },
        "div": { "type": "Button", "label": "/"   },
        "mul": { "type": "Button", "label": "*"   },
        "bsp": { "type": "Button", "label": "<-"  }
      }
    },
    "row1": {
      "type": "HorizontalLayout",
      "children": {
        "n7":  { "type": "Button", "label": "7" },
        "n8":  { "type": "Button", "label": "8" },
        "n9":  { "type": "Button", "label": "9" },
        "sub": { "type": "Button", "label": "-" }
      }
    },
    "row2": {
      "type": "HorizontalLayout",
      "children": {
        "n4":  { "type": "Button", "label": "4" },
        "n5":  { "type": "Button", "label": "5" },
        "n6":  { "type": "Button", "label": "6" },
        "add": { "type": "Button", "label": "+" }
      }
    },
    "row3": {
      "type": "HorizontalLayout",
      "children": {
        "n1": { "type": "Button", "label": "1" },
        "n2": { "type": "Button", "label": "2" },
        "n3": { "type": "Button", "label": "3" },
        "eq": { "type": "Button", "label": "=" }
      }
    },
    "row4": {
      "type": "HorizontalLayout",
      "children": {
        "n0":  { "type": "Button", "label": "0"   },
        "dot": { "type": "Button", "label": "."   },
        "pm":  { "type": "Button", "label": "+/-" },
        "pct": { "type": "Button", "label": "%"   }
      }
    }
  }
})";

// ── Calculator client ─────────────────────────────────────────────────────────

class calc_client : public wish::client {
 public:
  calc_client(
      std::unique_ptr<bison::rmi::transport::client_transport_iface> t,
      wish::sdl3_renderer* renderer)
      : wish::client(std::move(t)), renderer_(renderer) {}

 protected:
  void on_session() override {
    register_template("calc"_key, kCalcDesc).get();
    auto pm = instantiate_template("calc"_key).get();

    // Capture display proxy for updates.
    auto& disp = pm.at("display");

    // ── Button handlers ───────────────────────────────────────────────────

    // Helper: push display string to the label proxy.
    auto update_display = [&]() {
      dynamic f;
      f["text"_key] = display_;
      disp.set(std::move(f));
    };

    // Digit button: append character to the current entry.
    auto digit_handler = [&, update_display](const std::string& ch) {
      return [&, ch, update_display](dynamic) {
        if (fresh_) { display_ = ch; fresh_ = false; }
        else        { display_ += ch; }
        update_display();
      };
    };

    // Operator button: store pending operand and op.
    auto op_handler = [&, update_display](char op) {
      return [&, op, update_display](dynamic) {
        operand_    = std::stod(display_);
        pending_op_ = op;
        fresh_      = true;
        update_display();
      };
    };

    pm.at("row0.c").onEvent("clicked"_key, [&, update_display](dynamic) {
      display_    = "0";
      operand_    = 0.0;
      pending_op_ = 0;
      fresh_      = true;
      update_display();
    });

    pm.at("row0.div").onEvent("clicked"_key, op_handler('/'));
    pm.at("row0.mul").onEvent("clicked"_key, op_handler('*'));

    pm.at("row0.bsp").onEvent("clicked"_key, [&, update_display](dynamic) {
      if (display_.size() > 1) display_.pop_back();
      else                     display_ = "0";
      update_display();
    });

    pm.at("row1.n7").onEvent("clicked"_key, digit_handler("7"));
    pm.at("row1.n8").onEvent("clicked"_key, digit_handler("8"));
    pm.at("row1.n9").onEvent("clicked"_key, digit_handler("9"));
    pm.at("row1.sub").onEvent("clicked"_key, op_handler('-'));

    pm.at("row2.n4").onEvent("clicked"_key, digit_handler("4"));
    pm.at("row2.n5").onEvent("clicked"_key, digit_handler("5"));
    pm.at("row2.n6").onEvent("clicked"_key, digit_handler("6"));
    pm.at("row2.add").onEvent("clicked"_key, op_handler('+'));

    pm.at("row3.n1").onEvent("clicked"_key, digit_handler("1"));
    pm.at("row3.n2").onEvent("clicked"_key, digit_handler("2"));
    pm.at("row3.n3").onEvent("clicked"_key, digit_handler("3"));

    pm.at("row3.eq").onEvent("clicked"_key, [&, update_display](dynamic) {
      double rhs = std::stod(display_);
      double result = 0.0;
      switch (pending_op_) {
        case '+': result = operand_ + rhs; break;
        case '-': result = operand_ - rhs; break;
        case '*': result = operand_ * rhs; break;
        case '/': result = (rhs != 0.0) ? operand_ / rhs : 0.0; break;
        default:  result = rhs; break;
      }
      // Format: drop trailing .0 for whole numbers.
      if (result == std::floor(result) &&
          std::abs(result) < 1e12) {
        display_ = std::to_string(static_cast<long long>(result));
      } else {
        std::ostringstream oss;
        oss << result;
        display_ = oss.str();
      }
      pending_op_ = 0;
      fresh_      = true;
      update_display();
    });

    pm.at("row4.n0").onEvent("clicked"_key, digit_handler("0"));

    pm.at("row4.dot").onEvent("clicked"_key, [&, update_display](dynamic) {
      if (display_.find('.') == std::string::npos)
        display_ += '.';
      fresh_ = false;
      update_display();
    });

    pm.at("row4.pm").onEvent("clicked"_key, [&, update_display](dynamic) {
      if (!display_.empty() && display_ != "0") {
        if (display_[0] == '-') display_.erase(0, 1);
        else                    display_.insert(0, "-");
      }
      update_display();
    });

    pm.at("row4.pct").onEvent("clicked"_key, [&, update_display](dynamic) {
      double v = std::stod(display_) / 100.0;
      std::ostringstream oss;
      oss << v;
      display_ = oss.str();
      update_display();
    });

    // ── Wait until the window is closed ──────────────────────────────────
    while (!renderer_->should_quit())
      std::this_thread::sleep_for(std::chrono::milliseconds{16});
  }

 private:
  wish::sdl3_renderer* renderer_;

  // Calculator state
  std::string display_    = "0";
  double      operand_    = 0.0;
  char        pending_op_ = 0;
  bool        fresh_      = true;
};

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
  memory_server_transport transport;

  auto r    = std::make_unique<wish::sdl3_renderer>("Calculator", 300, 420);
  auto rptr = r.get();

  wish::server server{transport, std::move(r)};
  server.start();  // spawns render thread (SDL lives there) + bison listen thread

  // run() blocks in on_session() until should_quit() goes true (window closed).
  calc_client client{transport.connect(), rptr};
  client.run();

  server.stop();
  return 0;
}
