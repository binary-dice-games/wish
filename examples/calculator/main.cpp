// MIT License © 2025 Binary Dice Games
//
// Calculator example: single-binary wish demo using an in-memory transport.
// The server renders a real SDL3 window; the client (running on the same
// thread as main, after server.start()) drives the UI via the wish RPC layer.
//
// Usage: calculator [--verbose] [--theme dark|light|classic]

#include <client.hpp>
#include <sdl3_renderer.hpp>
#include <server.hpp>
#include <web/web_renderer.hpp>

#include "src/rmi/rmi.hpp" // memory_server_transport / memory_client_transport

#include <gflags/gflags.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

DEFINE_bool(verbose, false, "Print verbose trace to stderr.");
DEFINE_string(theme, "dark", "UI theme preset: dark, light, or classic.");
DEFINE_int32(font_size, 16, "Font size in pixels");
DEFINE_string(renderer, "web", "Rendering backend: sdl3 or web");
DEFINE_int32(web_port, 8080, "HTTP/WebSocket port for --renderer web");
DEFINE_string(web_bind, "127.0.0.1", "Bind address for --renderer web (localhost-only by default)");

static bool ValidateTheme(const char* /*flag*/, const std::string& value) {
  return value == "dark" || value == "light" || value == "classic";
}
DEFINE_validator(theme, &ValidateTheme);

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Calculator UI descriptor ──────────────────────────────────────────────────

static constexpr const char* kCalcDesc = R"({
  "type": "Window",
  "title": "Calculator",
  "width": 328,
  "height": 420,
  "children": {
    "display": { "type": "Label", "text": "0" },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",   "width": 72, "height": 52 },
        "div": { "type": "Button", "label": "/",   "width": 72, "height": 52 },
        "mul": { "type": "Button", "label": "*",   "width": 72, "height": 52 },
        "bsp": { "type": "Button", "label": "<-",  "width": 72, "height": 52 }
      }
    },
    "row1": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n7":  { "type": "Button", "label": "7", "width": 72, "height": 52 },
        "n8":  { "type": "Button", "label": "8", "width": 72, "height": 52 },
        "n9":  { "type": "Button", "label": "9", "width": 72, "height": 52 },
        "sub": { "type": "Button", "label": "-", "width": 72, "height": 52 }
      }
    },
    "row2": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n4":  { "type": "Button", "label": "4", "width": 72, "height": 52 },
        "n5":  { "type": "Button", "label": "5", "width": 72, "height": 52 },
        "n6":  { "type": "Button", "label": "6", "width": 72, "height": 52 },
        "add": { "type": "Button", "label": "+", "width": 72, "height": 52 }
      }
    },
    "row3": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n1": { "type": "Button", "label": "1", "width": 72, "height": 52 },
        "n2": { "type": "Button", "label": "2", "width": 72, "height": 52 },
        "n3": { "type": "Button", "label": "3", "width": 72, "height": 52 },
        "eq": { "type": "Button", "label": "=", "width": 72, "height": 52 }
      }
    },
    "row4": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n0":  { "type": "Button", "label": "0",   "width": 72, "height": 52 },
        "dot": { "type": "Button", "label": ".",   "width": 72, "height": 52 },
        "pm":  { "type": "Button", "label": "+/-", "width": 72, "height": 52 },
        "pct": { "type": "Button", "label": "%",   "width": 72, "height": 52 }
      }
    }
  }
})";

// ── Calculator client ─────────────────────────────────────────────────────────

class calc_client : public wish::client {
 public:
  calc_client(memory_client_transport t, wish::renderer* renderer, bool verbose = false, std::string theme = "dark")
      : wish::client(std::move(t)), renderer_(renderer), verbose_(verbose), theme_(std::move(theme)) {}

 protected:
  void on_session() override {
    vlog("applying " + theme_ + " theme");
    set_style_preset(theme_).get();
    {
      dynamic overrides;
      overrides["window_rounding"_key] = 6.0f;
      overrides["frame_rounding"_key] = 4.0f;
      overrides["grab_rounding"_key] = 4.0f;
      set_style(std::move(overrides)).get();
    }

    vlog("registering template 'calc'");
    register_template_from_json("calc"_key, kCalcDesc).get();

    vlog("instantiating template 'calc'");
    auto pm = instantiate_template("calc"_key).get();

    if (verbose_) {
      std::clog << "[calc] proxy map (" << pm.size() << " entries):\n";
      for (auto& [name, proxy] : pm)
        std::clog << "  \"" << name << "\"\n";
    }

    // Capture display proxy for updates.
    auto& disp = pm.at("display");

    // ── Button handlers ───────────────────────────────────────────────────

    // Helper: push display string to the label proxy.
    auto update_display = [&]() {
      vlog("update_display -> \"" + display_ + "\"");
      dynamic f;
      f["text"_key] = display_;
      disp.set(std::move(f));
    };

    // Digit button: append character to the current entry.
    auto digit_handler = [&, update_display](const std::string& ch) {
      return [&, ch, update_display](dynamic) {
        vlog("digit '" + ch + "' clicked");
        if (fresh_) {
          display_ = ch;
          fresh_ = false;
        } else {
          display_ += ch;
        }
        update_display();
      };
    };

    // Operator button: store pending operand and op.
    auto op_handler = [&, update_display](char op) {
      return [&, op, update_display](dynamic) {
        vlog(std::string("op '") + op + "' clicked");
        operand_ = std::stod(display_);
        pending_op_ = op;
        fresh_ = true;
        update_display();
      };
    };

    vlog("registering button handlers");

    pm.at("row0.c").onEvent("clicked"_key, [&, update_display](dynamic) {
      vlog("C (clear) clicked");
      display_ = "0";
      operand_ = 0.0;
      pending_op_ = 0;
      fresh_ = true;
      update_display();
    });

    pm.at("row0.div").onEvent("clicked"_key, op_handler('/'));
    pm.at("row0.mul").onEvent("clicked"_key, op_handler('*'));

    pm.at("row0.bsp").onEvent("clicked"_key, [&, update_display](dynamic) {
      vlog("<- (backspace) clicked");
      if (display_.size() > 1)
        display_.pop_back();
      else
        display_ = "0";
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
      vlog("= (equals) clicked");
      double rhs = std::stod(display_);
      double result = 0.0;
      switch (pending_op_) {
        case '+':
          result = operand_ + rhs;
          break;
        case '-':
          result = operand_ - rhs;
          break;
        case '*':
          result = operand_ * rhs;
          break;
        case '/':
          result = (rhs != 0.0) ? operand_ / rhs : 0.0;
          break;
        default:
          result = rhs;
          break;
      }
      // Format: drop trailing .0 for whole numbers.
      if (result == std::floor(result) && std::abs(result) < 1e12) {
        display_ = std::to_string(static_cast<long long>(result));
      } else {
        std::ostringstream oss;
        oss << result;
        display_ = oss.str();
      }
      pending_op_ = 0;
      fresh_ = true;
      vlog("result: \"" + display_ + "\"");
      update_display();
    });

    pm.at("row4.n0").onEvent("clicked"_key, digit_handler("0"));

    pm.at("row4.dot").onEvent("clicked"_key, [&, update_display](dynamic) {
      vlog(". (dot) clicked");
      if (display_.find('.') == std::string::npos)
        display_ += '.';
      fresh_ = false;
      update_display();
    });

    pm.at("row4.pm").onEvent("clicked"_key, [&, update_display](dynamic) {
      vlog("+/- clicked");
      if (!display_.empty() && display_ != "0") {
        if (display_[0] == '-')
          display_.erase(0, 1);
        else
          display_.insert(0, "-");
      }
      update_display();
    });

    pm.at("row4.pct").onEvent("clicked"_key, [&, update_display](dynamic) {
      vlog("% clicked");
      double v = std::stod(display_) / 100.0;
      std::ostringstream oss;
      oss << v;
      display_ = oss.str();
      update_display();
    });

    vlog("ready — waiting for window close");

    // ── Wait until the window is closed ──────────────────────────────────
    while (!renderer_->should_quit())
      std::this_thread::sleep_for(std::chrono::milliseconds{16});

    vlog("window closed — exiting on_session");
  }

 private:
  void vlog(const std::string& msg) const {
    if (verbose_)
      std::clog << "[calc] " << msg << "\n";
  }

  wish::renderer* renderer_;
  bool verbose_;
  std::string theme_;

  // Calculator state
  std::string display_ = "0";
  double operand_ = 0.0;
  char pending_op_ = 0;
  bool fresh_ = true;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::unique_ptr<wish::renderer> make_renderer() {
  if (FLAGS_renderer == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    return std::make_unique<wish::sdl3_renderer>("Calculator", 340, 440, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=sdl3 requested but this binary was built with WISH_ENABLE_SDL3=OFF");
#endif
  }
  if (FLAGS_renderer == "web") {
#ifdef WISH_WEB_ENABLED
    std::cout << "[wish] open http://" << FLAGS_web_bind << ':' << FLAGS_web_port << " in a browser\n"
              << "Press Ctrl+C to stop\n"
              << std::flush;
    return std::make_unique<wish::web_renderer>(FLAGS_web_bind, FLAGS_web_port, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=web requested but this binary was built with WISH_ENABLE_WEB=OFF");
#endif
  }
  throw std::runtime_error("unknown --renderer value '" + FLAGS_renderer + "' (expected sdl3 or web)");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_verbose)
    std::clog << "[calc] starting\n";

  memory_server_transport transport;

  auto r = make_renderer();
  auto rptr = r.get();

  wish::server server{transport, std::move(r)};
  server.start(); // spawns render thread (SDL lives there) + bison listen thread

  if (FLAGS_verbose)
    std::clog << "[calc] server started — connecting client\n";

  // run() blocks in on_session() until should_quit() goes true (window closed).
  calc_client client{transport.connect(), rptr, FLAGS_verbose, FLAGS_theme};
  client.run();

  if (FLAGS_verbose)
    std::clog << "[calc] client done — stopping server\n";

  server.stop();
  return 0;
}
