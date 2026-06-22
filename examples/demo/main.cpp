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

static constexpr const char* kDemoDesc = R"json({
  "type": "Window", "title": "wish Widget Demo",
  "width": 820, "height": 900,
  "children": {

    "main_menu": { "type": "MenuBar",
      "children": {
        "m_file": { "type": "Menu", "label": "File",
          "children": {
            "mi_new":   { "type": "MenuItem", "label": "New",   "shortcut": "Ctrl+N" },
            "mi_open":  { "type": "MenuItem", "label": "Open",  "shortcut": "Ctrl+O" },
            "mi_sep":   { "type": "Separator" },
            "mi_quit":  { "type": "MenuItem", "label": "Quit",  "shortcut": "Alt+F4" }
          }
        },
        "m_view": { "type": "Menu", "label": "View",
          "children": {
            "mi_dark":    { "type": "MenuItem", "label": "Dark theme"    },
            "mi_light":   { "type": "MenuItem", "label": "Light theme"   },
            "mi_classic": { "type": "MenuItem", "label": "Classic theme" }
          }
        },
        "m_check": { "type": "Menu", "label": "Options",
          "children": {
            "mi_verbose": { "type": "MenuItem", "label": "Verbose logging", "checked": false }
          }
        }
      }
    },

    "tabs_root": { "type": "TabBar", "id": "demo_tabs",
      "children": {

        "tab_basics": { "type": "TabItem", "label": "Basics",
          "children": {

            "sec_labels":  { "type": "SeparatorText", "label": "Labels" },
            "lbl_static":  { "type": "Label", "text": "Static label — text set in JSON." },
            "lbl_dynamic": { "type": "Label", "text": "(updated by events)" },
            "lbl_clicks":  { "type": "Label", "text": "Click counter: 0" },

            "sec_buttons": { "type": "SeparatorText", "label": "Buttons" },
            "btn_row": {
              "type": "HorizontalLayout", "spacing": 8,
              "children": {
                "btn_click": { "type": "Button", "label": "Click me"      },
                "btn_reset": { "type": "Button", "label": "Reset counter" },
                "btn_wide":  { "type": "Button", "label": "Wide button",  "width": 160 }
              }
            },

            "sec_checks":  { "type": "SeparatorText", "label": "Checkboxes" },
            "chk_a":       { "type": "Checkbox", "label": "Option A", "value": false },
            "chk_b":       { "type": "Checkbox", "label": "Option B", "value": true  },
            "chk_vis":     { "type": "Checkbox", "label": "Show hidden label (visible field)", "value": true },
            "lbl_hidden":  { "type": "Label",    "text": "    This label is toggled by the checkbox above." },

            "sec_radio":   { "type": "SeparatorText", "label": "Radio Buttons" },
            "radio_row": {
              "type": "HorizontalLayout", "spacing": 12,
              "children": {
                "rb_a": { "type": "RadioButton", "label": "Alpha",  "active": true  },
                "rb_b": { "type": "RadioButton", "label": "Beta",   "active": false },
                "rb_c": { "type": "RadioButton", "label": "Gamma",  "active": false }
              }
            }
          }
        },

        "tab_sliders": { "type": "TabItem", "label": "Sliders & Drags",
          "children": {

            "sec_sliders":  { "type": "SeparatorText", "label": "Sliders" },
            "sf_opacity":   { "type": "SliderFloat", "label": "Opacity", "value": 1.0,  "min": 0.0,    "max": 1.0,   "format": "%.2f"     },
            "sf_angle":     { "type": "SliderFloat", "label": "Angle",   "value": 0.0,  "min": -180.0, "max": 180.0, "format": "%.0f deg" },
            "si_count":     { "type": "SliderInt",   "label": "Count",   "value": 10,   "min": 0,      "max": 50     },

            "sec_drags":    { "type": "SeparatorText", "label": "Drag Widgets" },
            "df_val":       { "type": "DragFloat", "label": "Float drag", "value": 1.5,  "speed": 0.05, "min": 0.0, "max": 10.0, "format": "%.2f" },
            "di_val":       { "type": "DragInt",   "label": "Int drag",   "value": 42,   "speed": 0.5,  "min": 0,   "max": 200  }
          }
        },

        "tab_inputs": { "type": "TabItem", "label": "Text & Numbers",
          "children": {

            "sec_text":    { "type": "SeparatorText", "label": "Text Input" },
            "txt_name":    { "type": "InputText", "label": "Name",    "value": "",             "hint": "Type your name..."  },
            "txt_msg":     { "type": "InputText", "label": "Message", "value": "Hello, wish!"                               },

            "sec_nums":    { "type": "SeparatorText", "label": "Numeric Input" },
            "ii_qty":      { "type": "InputInt",   "label": "Quantity",    "value": 1,   "step": 1,    "step_fast": 10  },
            "if_price":    { "type": "InputFloat", "label": "Price ($)",   "value": 9.99, "step": 0.01, "step_fast": 1.0, "format": "%.2f" }
          }
        },

        "tab_selection": { "type": "TabItem", "label": "Selection",
          "children": {

            "sec_combo":   { "type": "SeparatorText", "label": "Combo Box" },
            "cmb_fruit":   { "type": "Combo", "label": "Fruit",
                             "items": "Apple\nBanana\nCherry\nDate\nElder\nFig\nGrape",
                             "value": 0 },
            "cmb_size":    { "type": "Combo", "label": "Size",
                             "items": "Small\nMedium\nLarge\nExtra Large",
                             "value": 1 },

            "sec_sel":     { "type": "SeparatorText", "label": "Selectables" },
            "sel_a":       { "type": "Selectable", "label": "Item Alpha",   "selected": false },
            "sel_b":       { "type": "Selectable", "label": "Item Beta",    "selected": true  },
            "sel_c":       { "type": "Selectable", "label": "Item Gamma",   "selected": false },
            "sel_d":       { "type": "Selectable", "label": "Item Delta",   "selected": false }
          }
        },

        "tab_tree": { "type": "TabItem", "label": "Tree & Collapse",
          "children": {

            "sec_tree":  { "type": "SeparatorText", "label": "TreeNode" },
            "tn_root":   { "type": "TreeNode", "label": "Root node", "open": true,
              "children": {
                "tn_child_a": { "type": "TreeNode", "label": "Child A",
                  "children": {
                    "tn_leaf_1": { "type": "TreeNode", "label": "Leaf 1", "leaf": true },
                    "tn_leaf_2": { "type": "TreeNode", "label": "Leaf 2", "leaf": true }
                  }
                },
                "tn_child_b": { "type": "TreeNode", "label": "Child B",
                  "children": {
                    "tn_leaf_3": { "type": "TreeNode", "label": "Leaf 3", "leaf": true }
                  }
                }
              }
            },

            "sec_collap":    { "type": "SeparatorText", "label": "CollapsingHeader" },
            "ch_details":    { "type": "CollapsingHeader", "label": "Details",
              "children": {
                "ch_lbl_a": { "type": "Label", "text": "Line A inside collapsing header." },
                "ch_lbl_b": { "type": "Label", "text": "Line B inside collapsing header." }
              }
            },
            "ch_advanced":   { "type": "CollapsingHeader", "label": "Advanced",
              "children": {
                "ch_lbl_c": { "type": "Label", "text": "Advanced options would live here." }
              }
            }
          }
        },

        "tab_misc": { "type": "TabItem", "label": "Misc",
          "children": {

            "sec_progress": { "type": "SeparatorText", "label": "Progress Bar" },
            "pb_download":  { "type": "ProgressBar", "value": 0.65, "label": "65 %" },
            "pb_notext":    { "type": "ProgressBar", "value": 0.30 },

            "sec_layouts":  { "type": "SeparatorText", "label": "Layouts" },
            "lbl_hlay":     { "type": "Label",  "text": "HorizontalLayout (spacing 12):" },
            "hlay": {
              "type": "HorizontalLayout", "spacing": 12,
              "children": {
                "hl_a": { "type": "Button", "label": "Alpha" },
                "hl_b": { "type": "Button", "label": "Beta"  },
                "hl_c": { "type": "Button", "label": "Gamma" }
              }
            },
            "lbl_vlay": { "type": "Label", "text": "VerticalLayout (spacing 4) with nested rows:" },
            "vlay": {
              "type": "VerticalLayout", "spacing": 4,
              "children": {
                "vr1": {
                  "type": "HorizontalLayout", "spacing": 6,
                  "children": {
                    "vr1c1": { "type": "Button", "label": "R1 C1" },
                    "vr1c2": { "type": "Button", "label": "R1 C2" },
                    "vr1c3": { "type": "Button", "label": "R1 C3" }
                  }
                },
                "vr2": {
                  "type": "HorizontalLayout", "spacing": 6,
                  "children": {
                    "vr2c1": { "type": "Button", "label": "R2 C1" },
                    "vr2c2": { "type": "Button", "label": "R2 C2" }
                  }
                }
              }
            },

            "sec_theme":    { "type": "SeparatorText", "label": "Theme" },
            "lbl_theme":    { "type": "Label", "text": "Switch the visual theme at runtime:" },
            "theme_row": {
              "type": "HorizontalLayout", "spacing": 8,
              "children": {
                "theme_dark":    { "type": "Button", "label": "Dark"    },
                "theme_light":   { "type": "Button", "label": "Light"   },
                "theme_classic": { "type": "Button", "label": "Classic" }
              }
            }
          }
        }

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

    auto set_text = [&](const std::string& name, const std::string& text) {
      dynamic f;
      f["text"_key] = text;
      pm.at(name).set(std::move(f));
    };

    auto set_visible = [&](const std::string& name, bool vis) {
      dynamic f;
      f["visible"_key] = vis;
      pm.at(name).set(std::move(f));
    };

    auto status = [&, set_text](const std::string& msg) {
      vlog("event: " + msg);
      set_text("lbl_status", msg);
    };

    // ── Basics tab: labels & counters ─────────────────────────────────────

    pm.at("tabs_root.tab_basics.btn_row.btn_click").onEvent("clicked"_key,
        [&, set_text, status](dynamic) {
          ++click_count_;
          set_text("tabs_root.tab_basics.lbl_clicks",
                   "Click counter: " + std::to_string(click_count_));
          status("'Click me' pressed (" +
                 std::to_string(click_count_) + " times total)");
        });

    pm.at("tabs_root.tab_basics.btn_row.btn_reset").onEvent("clicked"_key,
        [&, set_text, status](dynamic) {
          click_count_ = 0;
          set_text("tabs_root.tab_basics.lbl_clicks", "Click counter: 0");
          status("Counter reset.");
        });

    pm.at("tabs_root.tab_basics.btn_row.btn_wide").onEvent("clicked"_key,
        [status](dynamic) { status("Wide button clicked."); });

    pm.at("tabs_root.tab_basics.chk_a").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          status("Option A: " + std::string(v ? "checked" : "unchecked"));
        });

    pm.at("tabs_root.tab_basics.chk_b").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          status("Option B: " + std::string(v ? "checked" : "unchecked"));
        });

    pm.at("tabs_root.tab_basics.chk_vis").onEvent("changed"_key,
        [set_visible, status](dynamic p) {
          const auto* f = p.findField("value"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          set_visible("tabs_root.tab_basics.lbl_hidden", v);
          status("Visibility: label is now " + std::string(v ? "shown" : "hidden"));
        });

    // Radio buttons — manage mutual exclusivity
    static const char* kRBNames[] = {
        "tabs_root.tab_basics.radio_row.rb_a",
        "tabs_root.tab_basics.radio_row.rb_b",
        "tabs_root.tab_basics.radio_row.rb_c"};
    static const char* kRBLabels[] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; ++i) {
      pm.at(kRBNames[i]).onEvent("clicked"_key,
          [&, i, set_text, status](dynamic) {
            for (int j = 0; j < 3; ++j) {
              dynamic f;
              f["active"_key] = (j == i);
              pm.at(kRBNames[j]).set(std::move(f));
            }
            status(std::string("Radio: ") + kRBLabels[i] + " selected.");
          });
    }

    // ── Sliders & Drags tab ───────────────────────────────────────────────

    pm.at("tabs_root.tab_sliders.sf_opacity").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
          char buf[64]; std::snprintf(buf, sizeof(buf), "Opacity: %.2f", v);
          status(buf);
        });

    pm.at("tabs_root.tab_sliders.sf_angle").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
          char buf[64]; std::snprintf(buf, sizeof(buf), "Angle: %.0f deg", v);
          status(buf);
        });

    pm.at("tabs_root.tab_sliders.si_count").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
          status("Count: " + std::to_string(v));
        });

    pm.at("tabs_root.tab_sliders.df_val").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
          char buf[64]; std::snprintf(buf, sizeof(buf), "Float drag: %.2f", v);
          status(buf);
        });

    pm.at("tabs_root.tab_sliders.di_val").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
          status("Int drag: " + std::to_string(v));
        });

    // ── Text & Numbers tab ────────────────────────────────────────────────

    pm.at("tabs_root.tab_inputs.txt_name").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "";
          status("Name: \"" + v + "\"");
        });

    pm.at("tabs_root.tab_inputs.txt_msg").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "";
          status("Message: \"" + v + "\"");
        });

    pm.at("tabs_root.tab_inputs.ii_qty").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
          status("Quantity: " + std::to_string(v));
        });

    pm.at("tabs_root.tab_inputs.if_price").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("value"_key);
          float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
          char buf[64]; std::snprintf(buf, sizeof(buf), "Price: $%.2f", v);
          status(buf);
        });

    // ── Selection tab ─────────────────────────────────────────────────────

    pm.at("tabs_root.tab_selection.cmb_fruit").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("text"_key);
          std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "?";
          status("Fruit: " + v);
        });

    pm.at("tabs_root.tab_selection.cmb_size").onEvent("changed"_key,
        [status](dynamic p) {
          const auto* f = p.findField("text"_key);
          std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "?";
          status("Size: " + v);
        });

    static const char* kSelNames[] = {
        "tabs_root.tab_selection.sel_a",
        "tabs_root.tab_selection.sel_b",
        "tabs_root.tab_selection.sel_c",
        "tabs_root.tab_selection.sel_d"};
    static const char* kSelLabels[] = {"Alpha", "Beta", "Gamma", "Delta"};
    for (int i = 0; i < 4; ++i) {
      pm.at(kSelNames[i]).onEvent("changed"_key,
          [i, status](dynamic p) {
            const auto* f = p.findField("selected"_key);
            bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
            status(std::string("Selectable ") + kSelLabels[i] + ": " +
                   (v ? "selected" : "deselected"));
          });
    }

    // ── Tree tab ──────────────────────────────────────────────────────────

    auto on_toggled = [status](const std::string& name) {
      return [name, status](dynamic p) {
        const auto* f = p.findField("open"_key);
        bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
        status(name + ": " + (v ? "expanded" : "collapsed"));
      };
    };

    pm.at("tabs_root.tab_tree.tn_root").onEvent("toggled"_key,
        on_toggled("Root node"));
    pm.at("tabs_root.tab_tree.tn_root.tn_child_a").onEvent("toggled"_key,
        on_toggled("Child A"));
    pm.at("tabs_root.tab_tree.tn_root.tn_child_b").onEvent("toggled"_key,
        on_toggled("Child B"));
    pm.at("tabs_root.tab_tree.ch_details").onEvent("toggled"_key,
        on_toggled("CollapsingHeader: Details"));
    pm.at("tabs_root.tab_tree.ch_advanced").onEvent("toggled"_key,
        on_toggled("CollapsingHeader: Advanced"));

    // ── Misc tab: layout buttons & theme ──────────────────────────────────

    for (const auto* name : {
             "tabs_root.tab_misc.hlay.hl_a",
             "tabs_root.tab_misc.hlay.hl_b",
             "tabs_root.tab_misc.hlay.hl_c",
             "tabs_root.tab_misc.vlay.vr1.vr1c1",
             "tabs_root.tab_misc.vlay.vr1.vr1c2",
             "tabs_root.tab_misc.vlay.vr1.vr1c3",
             "tabs_root.tab_misc.vlay.vr2.vr2c1",
             "tabs_root.tab_misc.vlay.vr2.vr2c2"}) {
      pm.at(name).onEvent("clicked"_key,
          [s = status, n = std::string(name)](dynamic) {
            s("Layout button '" + n + "' clicked.");
          });
    }

    pm.at("tabs_root.tab_misc.theme_row.theme_dark").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("dark").get();
          status("Theme: dark");
        });
    pm.at("tabs_root.tab_misc.theme_row.theme_light").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("light").get();
          status("Theme: light");
        });
    pm.at("tabs_root.tab_misc.theme_row.theme_classic").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("classic").get();
          status("Theme: classic");
        });

    // ── Menu bar ──────────────────────────────────────────────────────────

    pm.at("main_menu.m_file.mi_new").onEvent("clicked"_key,
        [status](dynamic) { status("Menu: File > New"); });
    pm.at("main_menu.m_file.mi_open").onEvent("clicked"_key,
        [status](dynamic) { status("Menu: File > Open"); });
    pm.at("main_menu.m_file.mi_quit").onEvent("clicked"_key,
        [status](dynamic) { status("Menu: File > Quit"); });

    pm.at("main_menu.m_view.mi_dark").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("dark").get();
          status("Menu: View > Dark theme");
        });
    pm.at("main_menu.m_view.mi_light").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("light").get();
          status("Menu: View > Light theme");
        });
    pm.at("main_menu.m_view.mi_classic").onEvent("clicked"_key,
        [&, status](dynamic) {
          set_style_preset("classic").get();
          status("Menu: View > Classic theme");
        });

    pm.at("main_menu.m_check.mi_verbose").onEvent("clicked"_key,
        [status](dynamic p) {
          const auto* f = p.findField("checked"_key);
          bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
          status(std::string("Verbose logging: ") + (v ? "on" : "off"));
        });

    // ── Tab events ────────────────────────────────────────────────────────

    static const char* kTabNames[] = {
        "tabs_root.tab_basics",
        "tabs_root.tab_sliders",
        "tabs_root.tab_inputs",
        "tabs_root.tab_selection",
        "tabs_root.tab_tree",
        "tabs_root.tab_misc"};
    static const char* kTabLabels[] = {
        "Basics", "Sliders & Drags", "Text & Numbers",
        "Selection", "Tree & Collapse", "Misc"};
    for (int i = 0; i < 6; ++i) {
      pm.at(kTabNames[i]).onEvent("selected"_key,
          [i, status](dynamic) {
            status(std::string("Tab selected: ") + kTabLabels[i]);
          });
    }

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

  auto r    = std::make_unique<wish::sdl3_renderer>("wish Widget Demo", 820, 900);
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
