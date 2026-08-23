// MIT License © 2025 Binary Dice Games
//
// Calculator example using the header-only C++ wish client binding.
//
// Port of examples/calculator/main.cpp (the native, single-binary demo)
// minus the in-memory server/renderer wiring: this is a *client only*.
// Start a wish server first (it owns the window/renderer), then point this
// program at it -- matching whichever transport the server was started
// with:
//
//   # --transport=tcp (explicit; the server's default is --transport=term):
//   build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
//   calculator_cpp --transport=tcp --host=127.0.0.1 --port=7070
//
//   # --transport=term (the server's default): the server spawns its own
//   # terminal and expects the client to run *inside* it, wrapping that
//   # process's own inherited stdio:
//   build/app/wish server --renderer=sdl3
//   -- inside the terminal the server just spawned --
//   calculator_cpp --transport=term
//
// Usage: calculator_cpp [--transport=tcp|pipe|term] [--host=HOST]
//                        [--port=PORT] [--name=PATH] [--theme=dark|light|classic] [--verbose]

#include <wish_cpp/wish.hpp>

#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace wish = bdg::wish::binding;
using namespace bdg::wish::binding;  // for the "_key" literal operator

namespace {

constexpr const char* kCalcDesc = R"({
  "type": "Window",
  "title": "Calculator",
  "width": 328,
  "height": 420,
  "closable": true,
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

// ── Calculator session ───────────────────────────────────────────────────

class calculator {
 public:
  calculator(bool verbose, std::string theme) : verbose_(verbose), theme_(std::move(theme)) {}

  void run_session(wish::client& client) {
    vlog("applying " + theme_ + " theme");
    client.set_style_preset(theme_);

    vlog("registering template 'calc'");
    client.register_template("calc", kCalcDesc);

    vlog("instantiating template 'calc'");
    auto root = client.instantiate_template("calc", "calc");
    root.on_event("closed"_key, [&client, this](wish::value) {
      vlog("window closed -- quitting");
      client.quit();
    });

    auto disp = client.proxy_get("calc.display");

    auto update_display = [&] {
      vlog("update_display -> \"" + display_ + "\"");
      wish::value f;
      f["text"_key] = display_;
      disp.set(f);
    };

    auto digit_handler = [&](std::string ch) {
      return [&, ch, update_display](wish::value) {
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

    auto op_handler = [&](char op) {
      return [&, op, update_display](wish::value) {
        vlog(std::string("op '") + op + "' clicked");
        operand_ = std::stod(display_);
        pending_op_ = op;
        fresh_ = true;
        update_display();
      };
    };

    vlog("registering button handlers");

    auto button = [&](const std::string& path) { return client.proxy_get("calc." + path); };

    buttons_.push_back(button("row0.c"));
    buttons_.back().on_event("clicked"_key, [&, update_display](wish::value) {
      vlog("C (clear) clicked");
      display_ = "0";
      operand_ = 0.0;
      pending_op_ = 0;
      fresh_ = true;
      update_display();
    });

    buttons_.push_back(button("row0.div"));
    buttons_.back().on_event("clicked"_key, op_handler('/'));
    buttons_.push_back(button("row0.mul"));
    buttons_.back().on_event("clicked"_key, op_handler('*'));

    buttons_.push_back(button("row0.bsp"));
    buttons_.back().on_event("clicked"_key, [&, update_display](wish::value) {
      vlog("<- (backspace) clicked");
      if (display_.size() > 1)
        display_.pop_back();
      else
        display_ = "0";
      update_display();
    });

    buttons_.push_back(button("row1.n7"));
    buttons_.back().on_event("clicked"_key, digit_handler("7"));
    buttons_.push_back(button("row1.n8"));
    buttons_.back().on_event("clicked"_key, digit_handler("8"));
    buttons_.push_back(button("row1.n9"));
    buttons_.back().on_event("clicked"_key, digit_handler("9"));
    buttons_.push_back(button("row1.sub"));
    buttons_.back().on_event("clicked"_key, op_handler('-'));

    buttons_.push_back(button("row2.n4"));
    buttons_.back().on_event("clicked"_key, digit_handler("4"));
    buttons_.push_back(button("row2.n5"));
    buttons_.back().on_event("clicked"_key, digit_handler("5"));
    buttons_.push_back(button("row2.n6"));
    buttons_.back().on_event("clicked"_key, digit_handler("6"));
    buttons_.push_back(button("row2.add"));
    buttons_.back().on_event("clicked"_key, op_handler('+'));

    buttons_.push_back(button("row3.n1"));
    buttons_.back().on_event("clicked"_key, digit_handler("1"));
    buttons_.push_back(button("row3.n2"));
    buttons_.back().on_event("clicked"_key, digit_handler("2"));
    buttons_.push_back(button("row3.n3"));
    buttons_.back().on_event("clicked"_key, digit_handler("3"));

    buttons_.push_back(button("row3.eq"));
    buttons_.back().on_event("clicked"_key, [&, update_display](wish::value) {
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

    buttons_.push_back(button("row4.n0"));
    buttons_.back().on_event("clicked"_key, digit_handler("0"));

    buttons_.push_back(button("row4.dot"));
    buttons_.back().on_event("clicked"_key, [&, update_display](wish::value) {
      vlog(". (dot) clicked");
      if (display_.find('.') == std::string::npos) display_ += '.';
      fresh_ = false;
      update_display();
    });

    buttons_.push_back(button("row4.pm"));
    buttons_.back().on_event("clicked"_key, [&, update_display](wish::value) {
      vlog("+/- clicked");
      if (!display_.empty() && display_ != "0") {
        if (display_[0] == '-')
          display_.erase(0, 1);
        else
          display_.insert(0, "-");
      }
      update_display();
    });

    buttons_.push_back(button("row4.pct"));
    buttons_.back().on_event("clicked"_key, [&, update_display](wish::value) {
      vlog("% clicked");
      double v = std::stod(display_) / 100.0;
      std::ostringstream oss;
      oss << v;
      display_ = oss.str();
      update_display();
    });

    vlog("ready -- waiting for quit()");
    client.wait();
    vlog("session ending");

    buttons_.clear();
    client.release("calc");
  }

 private:
  void vlog(const std::string& msg) const {
    if (verbose_) std::clog << "[calc] " << msg << "\n";
  }

  bool verbose_;
  std::string theme_;
  std::string display_ = "0";
  double operand_ = 0.0;
  char pending_op_ = 0;
  bool fresh_ = true;
  std::vector<wish::proxy> buttons_;
};

// ── Ctrl+C handling ──────────────────────────────────────────────────────
//
// wish::client::run() blocks on the RMI worker thread for the whole
// session; quit() is documented safe to call from any thread, including a
// signal handler, so a global pointer + a plain SIGINT handler is enough
// (no extra thread needed, unlike the Python port -- that one has to work
// around the GIL's signal-delivery constraints).

wish::client* g_client_for_signal = nullptr;

void handle_sigint(int) {
  if (g_client_for_signal) g_client_for_signal->quit();
}

struct cli_args {
  std::string transport = "tcp";
  std::string host = "127.0.0.1";
  uint16_t port = 7070;
  std::string name;
  std::string theme = "wish";
  bool verbose = false;
};

cli_args parse_args(int argc, char* argv[]) {
  cli_args args;
  auto value_of = [&](const std::string& arg, const std::string& flag) -> std::optional<std::string> {
    std::string prefix = flag + "=";
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    return std::nullopt;
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (auto v = value_of(arg, "--transport")) args.transport = *v;
    else if (auto v = value_of(arg, "--host")) args.host = *v;
    else if (auto v = value_of(arg, "--port")) args.port = static_cast<uint16_t>(std::stoi(*v));
    else if (auto v = value_of(arg, "--name")) args.name = *v;
    else if (auto v = value_of(arg, "--theme")) args.theme = *v;
    else if (arg == "--verbose") args.verbose = true;
  }
  return args;
}

}  // namespace

int main(int argc, char* argv[]) {
  cli_args args = parse_args(argc, argv);

  wish::client client = [&] {
    if (args.transport == "tcp") {
      std::cout << "[Client] connecting to " << args.host << ":" << args.port << " ...\n";
      return wish::client::tcp(args.host, args.port);
    }
    if (args.transport == "pipe") {
      std::cout << "[Client] connecting to pipe " << args.name << " ...\n";
      return wish::client::pipe(args.name);
    }
    std::cout << "[Client] connecting via inherited stdio (--transport=term) ...\n";
    return wish::client::term();
  }();

  g_client_for_signal = &client;
  std::signal(SIGINT, handle_sigint);

  calculator calc(args.verbose, args.theme);
  try {
    client.run([&](wish::client& c) { calc.run_session(c); });
  } catch (const std::exception& e) {
    std::cerr << "[Client] error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "[Client] done.\n";
  return EXIT_SUCCESS;
}
