// MIT License © 2025 Binary Dice Games
//
// Demo example: showcases every wish widget and layout type in a single window,
// analogous to ImGui::ShowDemoWindow for the wish framework.
//
// Usage: demo [--verbose | -v] [--theme dark|light|classic | -t <theme>]

#include <wish/client.hpp>
#include <wish/server.hpp>
#include <wish/sdl3_renderer.hpp>

#include "src/rmi/rmi.hpp"  // memory_server_transport / memory_client_transport

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── UI descriptor ─────────────────────────────────────────────────────────────

// One root Window with every element type.  Sections are separated by a
// Label carrying the section name followed by a Separator line.
static constexpr const char* kDemoDesc = R"json({
  "type": "Window", "title": "wish Widget Demo",
  "width": 760, "height": 650,
  "children": {

    "sec_basics":   { "type": "Label",     "text": "Basics"   },
    "sep_basics":   { "type": "Separator"                      },
    "lbl_static":   { "type": "Label",     "text": "A Label displays static or dynamic text." },
    "lbl_clicks":   { "type": "Label",     "text": "Click counter: 0" },

    "sec_buttons":  { "type": "Label",     "text": "Buttons"  },
    "sep_buttons":  { "type": "Separator"                      },
    "btn_row": {
      "type": "HorizontalLayout", "spacing": 8,
      "children": {
        "btn_click": { "type": "Button", "label": "Click me"          },
        "btn_reset": { "type": "Button", "label": "Reset counter"     },
        "btn_wide":  { "type": "Button", "label": "Wide (width=200)", "width": 200 }
      }
    },

    "sec_checks":   { "type": "Label",    "text": "Checkboxes" },
    "sep_checks":   { "type": "Separator"                       },
    "chk_a":        { "type": "Checkbox", "label": "Option A",                          "value": false },
    "chk_b":        { "type": "Checkbox", "label": "Option B",                          "value": true  },
    "chk_vis":      { "type": "Checkbox", "label": "Show hidden label (visible field)", "value": true  },
    "lbl_hidden":   { "type": "Label",    "text": "    This label is toggled by the checkbox above." },

    "sec_sliders":  { "type": "Label",       "text": "Sliders"  },
    "sep_sliders":  { "type": "Separator"                        },
    "sf_opacity":   { "type": "SliderFloat", "label": "Opacity", "value": 1.0,  "min": 0.0,    "max": 1.0,   "format": "%.2f"     },
    "sf_angle":     { "type": "SliderFloat", "label": "Angle",   "value": 0.0,  "min": -180.0, "max": 180.0, "format": "%.0f deg" },
    "si_count":     { "type": "SliderInt",   "label": "Count",   "value": 10,   "min": 0,      "max": 50     },

    "sec_inputs":   { "type": "Label",     "text": "Text Input"   },
    "sep_inputs":   { "type": "Separator"                          },
    "txt_name":     { "type": "InputText", "label": "Name",    "value": "",             "hint": "Type your name..." },
    "txt_msg":      { "type": "InputText", "label": "Message", "value": "Hello, wish!"                             },

    "sec_layouts":  { "type": "Label",    "text": "Layouts"  },
    "sep_layouts":  { "type": "Separator"                     },
    "lbl_hlay":     { "type": "Label",    "text": "HorizontalLayout (spacing 12):" },
    "hlay": {
      "type": "HorizontalLayout", "spacing": 12,
      "children": {
        "hl_a": { "type": "Button", "label": "Alpha" },
        "hl_b": { "type": "Button", "label": "Beta"  },
        "hl_c": { "type": "Button", "label": "Gamma" }
      }
    },
    "lbl_vlay": { "type": "Label", "text": "VerticalLayout (spacing 4) with nested HorizontalLayouts:" },
    "vlay": {
      "type": "VerticalLayout", "spacing": 4,
      "children": {
        "vr1": {
          "type": "HorizontalLayout", "spacing": 6,
          "children": {
            "vr1c1": { "type": "Button", "label": "Row 1  Col 1" },
            "vr1c2": { "type": "Button", "label": "Row 1  Col 2" },
            "vr1c3": { "type": "Button", "label": "Row 1  Col 3" }
          }
        },
        "vr2": {
          "type": "HorizontalLayout", "spacing": 6,
          "children": {
            "vr2c1": { "type": "Button", "label": "Row 2  Col 1" },
            "vr2c2": { "type": "Button", "label": "Row 2  Col 2" }
          }
        }
      }
    },

    "sec_theme":    { "type": "Label",    "text": "Theme"    },
    "sep_theme":    { "type": "Separator"                     },
    "lbl_theme":    { "type": "Label",    "text": "Switch the visual theme at runtime:" },
    "theme_row": {
      "type": "HorizontalLayout", "spacing": 8,
      "children": {
        "theme_dark":    { "type": "Button", "label": "Dark"    },
        "theme_light":   { "type": "Button", "label": "Light"   },
        "theme_classic": { "type": "Button", "label": "Classic" }
      }
    },

    "sep_status":   { "type": "Separator" },
    "lbl_ev_hdr":   { "type": "Label", "text": "Last event:" },
    "lbl_status":   { "type": "Label", "text": "(interact with any widget to see its event here)" }
  }
})json";

// ── Demo client ───────────────────────────────────────────────────────────────

class demo_client : public wish::client {
 public:
  demo_client(memory_client_transport t,
              wish::sdl3_renderer* renderer,
              bool verbose = false,
              std::string theme = "dark")
      : wish::client(std::move(t)),
        renderer_(renderer),
        verbose_(verbose),
        theme_(std::move(theme)) {}

 protected:
  void on_session() override {
    vlog("applying " + theme_ + " theme");
    set_style_preset(theme_).get();

    vlog("registering and instantiating template");
    register_template("demo"_key, kDemoDesc).get();
    auto pm = instantiate_template("demo"_key).get();

    // ── Shared helpers ────────────────────────────────────────────────────

    // Push a new text value to a Label element.
    auto set_text = [&](const std::string& name, const std::string& text) {
      dynamic f;
      f["text"_key] = text;
      pm.at(name).set(std::move(f));
    };

    // Flip the visible field of any element.
    auto set_visible = [&](const std::string& name, bool vis) {
      dynamic f;
      f["visible"_key] = vis;
      pm.at(name).set(std::move(f));
    };

    // Write a status message to the bottom label and optionally log it.
    auto status = [&, set_text](const std::string& msg) {
      vlog("event: " + msg);
      set_text("lbl_status", msg);
    };

    // ── Buttons ───────────────────────────────────────────────────────────

    pm.at("btn_row.btn_click").onEvent("clicked"_key,
        [&, set_text, status](dynamic) {
          ++click_count_;
          set_text("lbl_clicks",
                   "Click counter: " + std::to_string(click_count_));
          status("'Click me' pressed (" +
                 std::to_string(click_count_) + " times total)");
        });

    pm.at("btn_row.btn_reset").onEvent("clicked"_key,
        [&, set_text, status](dynamic) {
          click_count_ = 0;
          set_text("lbl_clicks", "Click counter: 0");
          status("Counter reset.");
        });

    pm.at("btn_row.btn_wide").onEvent("clicked"_key,
        [status](dynamic) { status("Wide button clicked."); });

    // ── Checkboxes ────────────────────────────────────────────────────────

    pm.at("chk_a").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          status("Option A: " + std::string(v ? "checked" : "unchecked"));
        });

    pm.at("chk_b").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          status("Option B: " + std::string(v ? "checked" : "unchecked"));
        });

    pm.at("chk_vis").onEvent("changed"_key,
        [set_visible, status](dynamic p) {
          const auto* f = p.findField("value"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          set_visible("lbl_hidden", v);
          status("Visibility: label is now " +
                 std::string(v ? "visible" : "hidden"));
        });

    // ── Sliders ───────────────────────────────────────────────────────────

    pm.at("sf_opacity").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
          char buf[64];
          std::snprintf(buf, sizeof(buf), "Opacity: %.2f", v);
          status(buf);
        });

    pm.at("sf_angle").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
          char buf[64];
          std::snprintf(buf, sizeof(buf), "Angle: %.0f deg", v);
          status(buf);
        });

    pm.at("si_count").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
          status("Count: " + std::to_string(v));
        });

    // ── Text inputs ───────────────────────────────────────────────────────

    pm.at("txt_name").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "";
          status("Name: \"" + v + "\"");
        });

    pm.at("txt_msg").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "";
          status("Message: \"" + v + "\"");
        });

    // ── Layout buttons — echo click to status ─────────────────────────────

    for (const auto* name : {
             "hlay.hl_a",     "hlay.hl_b",     "hlay.hl_c",
             "vlay.vr1.vr1c1","vlay.vr1.vr1c2","vlay.vr1.vr1c3",
             "vlay.vr2.vr2c1","vlay.vr2.vr2c2"}) {
      pm.at(name).onEvent("clicked"_key,
          [s = status, n = std::string(name)](dynamic) {
            s("Layout button '" + n + "' clicked.");
          });
    }

    // ── Theme buttons — live theme switching ──────────────────────────────

    pm.at("theme_row.theme_dark").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("dark").get();
          status("Theme: dark");
        });

    pm.at("theme_row.theme_light").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("light").get();
          status("Theme: light");
        });

    pm.at("theme_row.theme_classic").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("classic").get();
          status("Theme: classic");
        });

    vlog("ready - waiting for window close");

    while (!renderer_->should_quit())
      std::this_thread::sleep_for(std::chrono::milliseconds{16});

    vlog("window closed");
  }

 private:
  void vlog(const std::string& msg) const {
    if (verbose_) std::clog << "[demo] " << msg << "\n";
  }

  wish::sdl3_renderer* renderer_;
  bool verbose_;
  std::string theme_;
  int click_count_ = 0;
};

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  bool verbose = false;
  std::string theme = "dark";
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--verbose" || arg == "-v") {
      verbose = true;
    } else if ((arg == "--theme" || arg == "-t") && i + 1 < argc) {
      theme = argv[++i];
      if (theme != "dark" && theme != "light" && theme != "classic") {
        std::cerr << "Unknown theme '" << theme
                  << "'. Valid values: dark, light, classic\n";
        return 1;
      }
    }
  }

  if (verbose) std::clog << "[demo] starting\n";

  memory_server_transport transport;

  auto r    = std::make_unique<wish::sdl3_renderer>("wish Widget Demo", 780, 670);
  auto rptr = r.get();

  wish::server server{transport, std::move(r)};
  server.start();

  if (verbose) std::clog << "[demo] server started - connecting client\n";

  demo_client client{transport.connect(), rptr, verbose, theme};
  client.run();

  if (verbose) std::clog << "[demo] client done - stopping server\n";

  server.stop();
  return 0;
}
