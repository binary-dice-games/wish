// MIT License © 2026 Binary Dice Games
/// @file kubectl.cpp
/// @brief Implementation of the KubectlFrontend form.
///
/// A close port of modules/bdg/dev/docker/server/docker.cpp: inline JSON
/// window layouts + import_json(), C++-built table rows, a per-row `...`
/// MenuButton, show_confirm() via a privately-instantiated MessageBox, and
/// an id -> handler dispatch map rebuilt on every update_*. The four list
/// windows (Pods / Deployments / Services / Nodes) share one
/// build_list_window() / clear_list_rows() / add_list_row() path.
#include "kubectl.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/dock_layout_spec.hpp>
#include <ui/forms/message_box.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// bison::dynamic has no initializer-list constructor -- event payloads are
// built field-by-field (docker.cpp's payload1/payload2).
template <typename T>
dynamic payload1(key_t k, T v) {
  dynamic d;
  d[k] = std::move(v);
  return d;
}
template <typename T1, typename T2>
dynamic payload2(key_t k1, T1 v1, key_t k2, T2 v2) {
  dynamic d;
  d[k1] = std::move(v1);
  d[k2] = std::move(v2);
  return d;
}

template <typename Fn>
void for_each_entry(const dynamic& parent, key_t field_key, Fn&& fn) {
  const auto* arr_f = parent.findField<dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return;
  (*arr_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto entry_ptr = f.as<dynamic_ptr>();
    if (entry_ptr)
      fn(*entry_ptr);
  });
}

// "#RRGGBBAA" light/dark pairs -- GitHub Primer tokens, the docker.cpp
// theme_hex pattern. A single text_color tuned for one theme reads poorly on
// the other.
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kIdleLight = "#656D76FF";
constexpr const char* kIdleDark = "#8B949EFF";
constexpr const char* kWarnLight = "#9A6700FF";
constexpr const char* kWarnDark = "#D29922FF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// Pod .status.phase (+ container waiting reason) -> (light, dark) colour.
std::pair<const char*, const char*> pod_status_colour(const std::string& effective, const std::string& phase) {
  if (contains(effective, "BackOff") || contains(effective, "Err") || contains(effective, "Crash") ||
      contains(effective, "Invalid") || contains(effective, "Failed") || phase == "Failed")
    return {kBadLight, kBadDark};
  if (phase == "Running")
    return {kOkLight, kOkDark};
  if (phase == "Pending" || contains(effective, "Creating") || contains(effective, "Init") ||
      contains(effective, "Waiting"))
    return {kWarnLight, kWarnDark};
  return {kIdleLight, kIdleDark}; // Succeeded, Completed, Unknown, ...
}

// Node "Ready"/"NotReady"[,SchedulingDisabled] -> (light, dark) colour.
std::pair<const char*, const char*> node_status_colour(const std::string& status) {
  if (contains(status, "NotReady"))
    return {kBadLight, kBadDark};
  if (contains(status, "SchedulingDisabled"))
    return {kWarnLight, kWarnDark};
  return {kOkLight, kOkDark};
}

// "a/b" -> ok when a == b and a != "0"; warn otherwise.
std::pair<const char*, const char*> ready_colour(const std::string& ready) {
  auto slash = ready.find('/');
  if (slash != std::string::npos) {
    std::string have = ready.substr(0, slash);
    std::string want = ready.substr(slash + 1);
    if (have == want && have != "0" && !have.empty())
      return {kOkLight, kOkDark};
  }
  return {kWarnLight, kWarnDark};
}

// ── Window layouts ─────────────────────────────────────────────────────────
//
// The windows carry no "pos_x"/"pos_y": each opens un-positioned and docks
// into the ambient host dockspace. The *arrangement* is seeded once at the
// end of on_init() via form::set_default_dock_layout(), then owned by
// imgui.ini like any user drag. See docs/dock-layout.md.
//
// Each list table carries both "height": -1 (stretch row in `vbox`) and
// "outer_height": -1 (fill that region) -- the load-bearing pair documented
// in docker.cpp / git.cpp / tail.cpp. `vbox` is each Window's sole direct
// child so it already fills the body, no hint needed.
//
// kubectl_mock.json / kubectl_mock.html (this directory) mirror these
// layouts as a single tabbed window -- the mockup validated in the `editor`
// tool. Keep them roughly in sync when changing columns.

static constexpr const char* kPodsLayout = R"json({
  "type": "Window", "title": "Pods", "width": 960, "height": 500,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 },
      "state":       { "type": "Combo", "items": "All\nRunning\nPending\nSucceeded\nFailed", "value": 0, "width": 120 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pods_table", "columns": 7,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_ns":       { "type": "TableColumn", "label": "Namespace", "flags": "WidthFixed", "init_width": 130, "column_id": 0 },
        "col_name":     { "type": "TableColumn", "label": "Name",      "flags": "WidthStretch",                    "column_id": 1 },
        "col_ready":    { "type": "TableColumn", "label": "Ready",     "flags": "WidthFixed", "init_width": 64,  "column_id": 2 },
        "col_status":   { "type": "TableColumn", "label": "Status",    "flags": "WidthFixed", "init_width": 150, "column_id": 3 },
        "col_restarts": { "type": "TableColumn", "label": "Restarts",  "flags": "WidthFixed", "init_width": 72,  "column_id": 4 },
        "col_age":      { "type": "TableColumn", "label": "Age",       "flags": "WidthFixed", "init_width": 80,  "column_id": 5 },
        "col_actions":  { "type": "TableColumn", "label": "",          "flags": "WidthFixed", "init_width": 40,  "column_id": 6 }
      }
    }
  } } }
})json";

static constexpr const char* kDeploymentsLayout = R"json({
  "type": "Window", "title": "Deployments", "width": 820, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##deployments_table", "columns": 7,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_ns":        { "type": "TableColumn", "label": "Namespace",   "flags": "WidthFixed", "init_width": 130, "column_id": 0 },
        "col_name":      { "type": "TableColumn", "label": "Name",        "flags": "WidthStretch",                    "column_id": 1 },
        "col_ready":     { "type": "TableColumn", "label": "Ready",       "flags": "WidthFixed", "init_width": 70,  "column_id": 2 },
        "col_uptodate":  { "type": "TableColumn", "label": "Up-to-date",  "flags": "WidthFixed", "init_width": 90,  "column_id": 3 },
        "col_available": { "type": "TableColumn", "label": "Available",   "flags": "WidthFixed", "init_width": 80,  "column_id": 4 },
        "col_age":       { "type": "TableColumn", "label": "Age",         "flags": "WidthFixed", "init_width": 80,  "column_id": 5 },
        "col_actions":   { "type": "TableColumn", "label": "",            "flags": "WidthFixed", "init_width": 40,  "column_id": 6 }
      }
    }
  } } }
})json";

static constexpr const char* kServicesLayout = R"json({
  "type": "Window", "title": "Services", "width": 820, "height": 320,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##services_table", "columns": 7,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_ns":     { "type": "TableColumn", "label": "Namespace",  "flags": "WidthFixed", "init_width": 130, "column_id": 0 },
        "col_name":   { "type": "TableColumn", "label": "Name",       "flags": "WidthStretch",                    "column_id": 1 },
        "col_type":   { "type": "TableColumn", "label": "Type",       "flags": "WidthFixed", "init_width": 110, "column_id": 2 },
        "col_ip":     { "type": "TableColumn", "label": "Cluster-IP", "flags": "WidthFixed", "init_width": 130, "column_id": 3 },
        "col_ports":  { "type": "TableColumn", "label": "Ports",      "flags": "WidthFixed", "init_width": 140, "column_id": 4 },
        "col_age":    { "type": "TableColumn", "label": "Age",        "flags": "WidthFixed", "init_width": 80,  "column_id": 5 },
        "col_actions":{ "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 6 }
      }
    }
  } } }
})json";

static constexpr const char* kNodesLayout = R"json({
  "type": "Window", "title": "Nodes", "width": 720, "height": 320,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##nodes_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":   { "type": "TableColumn", "label": "Name",    "flags": "WidthStretch",                    "column_id": 0 },
        "col_status": { "type": "TableColumn", "label": "Status",  "flags": "WidthFixed", "init_width": 200, "column_id": 1 },
        "col_ver":    { "type": "TableColumn", "label": "Version", "flags": "WidthFixed", "init_width": 130, "column_id": 2 },
        "col_age":    { "type": "TableColumn", "label": "Age",     "flags": "WidthFixed", "init_width": 90,  "column_id": 3 },
        "col_actions":{ "type": "TableColumn", "label": "",        "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

// Logs / Describe are a toolbar + a single-column scrolling Table of Label
// lines (docker.cpp's diff-viewer shape -- no session-sandbox file, no
// InputText max_length cap).

static constexpr const char* kLogsLayout = R"json({
  "type": "Window", "title": "Logs", "width": 900, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":     { "type": "Label", "text": "(no pod selected)" },
      "spring":     { "type": "Spring" },
      "follow":     { "type": "Checkbox", "label": "Follow", "value": false },
      "lines":      { "type": "InputInt", "label": "Lines", "value": 500, "step": 100, "width": 130 },
      "btn_refresh":{ "type": "Button", "label": "Refresh", "width": 90 }
    } },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##logs_table", "columns": 1,
      "flags": "ScrollX|ScrollY", "headers": false,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": { "col_line": { "type": "TableColumn", "flags": "WidthFixed", "column_id": 0 } }
    }
  } } }
})json";

static constexpr const char* kDescribeLayout = R"json({
  "type": "Window", "title": "Describe", "width": 820, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":     { "type": "Label", "text": "(nothing selected)" },
      "spring":     { "type": "Spring" },
      "btn_refresh":{ "type": "Button", "label": "Refresh", "width": 90 }
    } },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##describe_table", "columns": 1,
      "flags": "ScrollX|ScrollY", "headers": false,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": { "col_line": { "type": "TableColumn", "flags": "WidthFixed", "column_id": 0 } }
    }
  } } }
})json";

// The Console window: a FIFO-capped `Table` tracing every `kubectl` command
// the client ran (git's "Log" window, renamed to avoid clashing with the
// pod Logs window). "auto_scroll": true so it follows the newest row.

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 960, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##kubectl_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed", "init_width": 44,  "column_id": 0 },
        "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed", "init_width": 380, "column_id": 1 },
        "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed", "init_width": 50,  "column_id": 2 },
        "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
      }
    }
  } } }
})json";

// The Top window: four `top`-style rolling Plots fed one sample per
// update_stats call by kubectl_source's background poll thread, plus two
// current-values Tables, inside a scrolling VerticalLayout. Each Plot starts
// with an empty children map; build_top_window() creates the aggregate line
// and per-pod / per-node lines are added/removed at runtime.

static constexpr const char* kTopLayout = R"json({
  "type": "Window", "title": "Top", "width": 960, "height": 660,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "scroll": true, "children": {
    "status": { "type": "Label", "text": "" },
    "pods_cpu_plot":  { "type": "Plot", "title": "Pods CPU (millicores)", "height": 420, "profiler_marker": "K8s Pods CPU", "y_label": "m",   "children": {} },
    "pods_mem_plot":  { "type": "Plot", "title": "Pods Memory (MiB)",     "height": 420, "profiler_marker": "K8s Pods Mem", "y_label": "MiB", "children": {} },
    "nodes_cpu_plot": { "type": "Plot", "title": "Nodes CPU %",           "height": 200, "profiler_marker": "K8s Nodes CPU", "y_label": "%",  "children": {} },
    "nodes_mem_plot": { "type": "Plot", "title": "Nodes Memory %",        "height": 200, "profiler_marker": "K8s Nodes Mem", "y_label": "%",  "children": {} },
    "sep": { "type": "Separator" },
    "pods_table": {
      "type": "Table", "id": "##k8s_top_pods", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": 200, "outer_height": 200, "auto_scroll": false,
      "children": {
        "col_ns":   { "type": "TableColumn", "label": "Namespace", "flags": "WidthFixed", "init_width": 150, "column_id": 0 },
        "col_name": { "type": "TableColumn", "label": "Pod",       "flags": "WidthStretch",                   "column_id": 1 },
        "col_cpu":  { "type": "TableColumn", "label": "CPU",       "flags": "WidthFixed", "init_width": 90,  "column_id": 2 },
        "col_mem":  { "type": "TableColumn", "label": "Memory",    "flags": "WidthFixed", "init_width": 100, "column_id": 3 }
      }
    },
    "nodes_table": {
      "type": "Table", "id": "##k8s_top_nodes", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": 170, "outer_height": 170, "auto_scroll": false,
      "children": {
        "col_name": { "type": "TableColumn", "label": "Node",     "flags": "WidthStretch",                   "column_id": 0 },
        "col_cpu":  { "type": "TableColumn", "label": "CPU",      "flags": "WidthFixed", "init_width": 90,  "column_id": 1 },
        "col_cpup": { "type": "TableColumn", "label": "CPU %",    "flags": "WidthFixed", "init_width": 70,  "column_id": 2 },
        "col_mem":  { "type": "TableColumn", "label": "Memory",   "flags": "WidthFixed", "init_width": 100, "column_id": 3 },
        "col_memp": { "type": "TableColumn", "label": "Memory %", "flags": "WidthFixed", "init_width": 80,  "column_id": 4 }
      }
    }
  } } }
})json";

std::string format_pct(float pct) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(0) << pct << "%";
  return oss.str();
}

const char* noun_for(const std::string& scope) {
  if (scope == "pod")
    return "pod";
  if (scope == "deployment")
    return "deployment";
  if (scope == "service")
    return "service";
  return "node";
}

} // namespace

// ── kubectl_frontend ───────────────────────────────────────────────────────

kubectl_frontend::kubectl_frontend(dynamic&& base) : form(std::move(base)) {}

void kubectl_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void kubectl_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

ui_element_ptr kubectl_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void kubectl_frontend::on_init() {
  internal_root_key_ = next_available_key("__kubectl_");

  auto* title_f = findField<std::string>("title"_key);
  title_ = title_f ? *title_f : std::string{"Kubernetes"};

  // Pods is the main root -- form::init() registers internal_root_key_ as
  // this form's top-level object automatically. The other windows are
  // registered by hand inside build_list_window() / build_text_window()
  // (docker.cpp's build_*_window() pattern).
  build_list_window(pods_, kPodsLayout, internal_root_key_, "pod", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) {
      pods_.name_filter_input = e;
      pods_.name_filter_id = wish_id_of(e);
    });
    tree.with("vbox.toolbar.ns", [&](const auto& e) {
      pods_.ns_filter_input = e;
      pods_.ns_filter_id = wish_id_of(e);
    });
    tree.with("vbox.toolbar.state", [&](const auto& e) { pods_.phase_combo_id = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
  });

  build_list_window(
      deployments_, kDeploymentsLayout, internal_root_key_ + "_deployments", "deployment", [&](ui_tree& tree) {
        tree.with("vbox.toolbar.filter", [&](const auto& e) {
          deployments_.name_filter_input = e;
          deployments_.name_filter_id = wish_id_of(e);
        });
        tree.with("vbox.toolbar.ns", [&](const auto& e) {
          deployments_.ns_filter_input = e;
          deployments_.ns_filter_id = wish_id_of(e);
        });
        tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
          click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
        });
      });

  build_list_window(services_, kServicesLayout, internal_root_key_ + "_services", "service", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) {
      services_.name_filter_input = e;
      services_.name_filter_id = wish_id_of(e);
    });
    tree.with("vbox.toolbar.ns", [&](const auto& e) {
      services_.ns_filter_input = e;
      services_.ns_filter_id = wish_id_of(e);
    });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
  });

  build_list_window(nodes_, kNodesLayout, internal_root_key_ + "_nodes", "node", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) {
      nodes_.name_filter_input = e;
      nodes_.name_filter_id = wish_id_of(e);
    });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
  });

  logs_root_key_ = internal_root_key_ + "_logs";
  build_text_window(logs_root_key_, kLogsLayout, logs_window_id_, logs_table_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { logs_target_label_ = e; });
    tree.with("vbox.toolbar.follow", [&](const auto& e) { logs_follow_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.lines", [&](const auto& e) { logs_lines_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_logs_request(); };
    });
  });

  describe_root_key_ = internal_root_key_ + "_describe";
  build_text_window(describe_root_key_, kDescribeLayout, describe_window_id_, describe_table_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { describe_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_describe_request(); };
    });
  });

  console_root_key_ = internal_root_key_ + "_console";
  build_text_window(console_root_key_, kConsoleLayout, console_window_id_, console_table_, [](ui_tree&) {});

  top_root_key_ = internal_root_key_ + "_top";
  build_top_window();

  // Seed the first-run arrangement (mirrors docker): a wide left column of
  // tabbed list/top windows over a Console strip, and a narrower right
  // column with Logs + Describe. Owned by imgui.ini after the first run;
  // bump the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    const std::string deployments = internal_root_key_ + "_deployments";
    const std::string services = internal_root_key_ + "_services";
    const std::string nodes = internal_root_key_ + "_nodes";
    set_default_dock_layout(layout(
        split(
            dir::left, 0.62f,
            split(
                dir::down, 0.24f,
                area({console_root_key_}),
                area({internal_root_key_, deployments, services, nodes, top_root_key_}, internal_root_key_)),
            area({logs_root_key_, describe_root_key_}, logs_root_key_))));
  }

  // Initial population is triggered client-side (run_kubectl() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event (docker's / git's documented initial-load-race
  // fix).
}

void kubectl_frontend::build_text_window(
    const std::string& root_key, const char* layout_json, key_t& window_id_out, ui_element_ptr& table_out,
    const std::function<void(ui_tree&)>& wire_toolbar) {
  auto tree = import_json(layout_json);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
  window_id_out = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.table", [&](const auto& e) { table_out = e; });
  wire_toolbar(tree);

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), root_key);
  sess().top_level_objects[key_t{root_key}] = root_ptr;
  sess().top_level_handlers[key_t{root_key}] = this;
  (*root_ptr)["__path__"_key] = root_key;
}

void kubectl_frontend::set_text_lines(
    const ui_element_ptr& table, std::vector<key_t>& line_ids, size_t& next_key, const std::string& text) {
  if (!table)
    return;
  auto* children_p = table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  for (auto id : line_ids)
    ctx().objects.erase(id.id);
  line_ids.clear();
  next_key = 0;

  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);
    ui_element_ptr cell = ui_element_ptr::create("wish"_key, "Label"_key);
    cell["text"_key] = line;
    assign_id(cell);
    set_children_list(row, {cell});
    line_ids.push_back(wish_id_of(row));
    line_ids.push_back(wish_id_of(cell));
    (*children)[next_key++] = dynamic_ptr{row};

    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  table->refresh_children_order();
}

void kubectl_frontend::emit_logs_request() {
  if (open_logs_name_.empty())
    return;
  dynamic p;
  p["name"_key] = open_logs_name_;
  p["namespace"_key] = open_logs_ns_;
  p["follow"_key] = logs_follow_;
  p["lines"_key] = logs_lines_;
  emit("logs_requested"_key, std::move(p));
}

void kubectl_frontend::emit_describe_request() {
  if (open_describe_name_.empty())
    return;
  dynamic p;
  p["kind"_key] = open_describe_kind_;
  p["name"_key] = open_describe_name_;
  p["namespace"_key] = open_describe_ns_;
  emit("describe_requested"_key, std::move(p));
}

void kubectl_frontend::build_list_window(
    list_window& lw, const char* layout_json, const std::string& root_key, const std::string& scope,
    const std::function<void(ui_tree&)>& wire_toolbar) {
  lw.root_key = root_key;
  lw.scope = scope;
  auto tree = import_json(layout_json);

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  lw.window_id = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.status", [&](const auto& e) { lw.status_label = e; });
  tree.with("vbox.table", [&](const auto& e) { lw.table = e; });

  wire_toolbar(tree);

  const bool is_main = root_key == internal_root_key_;
  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), root_key);
  if (!is_main) {
    sess().top_level_objects[key_t{root_key}] = root_ptr;
    sess().top_level_handlers[key_t{root_key}] = this;
    (*root_ptr)["__path__"_key] = root_key;
  }
}

// ── Confirmation modal (docker_frontend::show_confirm() port) ───────────────

void kubectl_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
  dynamic params;
  params["title"_key] = std::string{"Confirm"};
  params["message"_key] = message;
  params["icon"_key] = std::string{"warning"};
  params["buttons"_key] = std::string{"yes_no"};

  confirm_dialog_ = instantiate_child_form<message_box>(
      "MessageBox"_key, std::move(params),
      [on_confirm = std::move(on_confirm)](key_t /*event_name*/, const dynamic& payload) {
        if (payload.as<std::string>("button"_key) == "yes")
          on_confirm();
      });
}

// ── Generic row plumbing ───────────────────────────────────────────────────

void kubectl_frontend::clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key) {
  if (!lw.table)
    return;
  auto* children_p = lw.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto& r : rows) {
    children->erase(r.child_key);
    for (auto id : r.object_ids) {
      ctx().objects.erase(id.id);
      menu_action_targets_.erase(id);
    }
  }
  rows.clear();
  next_key = 0;
}

void kubectl_frontend::add_list_row(
    list_window& lw, std::vector<list_row>& rows, size_t& next_key, list_row&& meta,
    const std::vector<ui_element_ptr>& cells, const std::vector<menu_spec>& items) {
  if (!lw.table)
    return;
  auto* children_p = lw.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> obj_ids;
  for (auto& cell : cells)
    obj_ids.push_back(wish_id_of(cell)); // make_label() already assign_id()'d these

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);
  obj_ids.push_back(wish_id_of(row));

  ui_element_ptr menu = ui_element_ptr::create("wish"_key, "MenuButton"_key);
  menu["label"_key] = std::string{"..."};
  assign_id(menu);
  obj_ids.push_back(wish_id_of(menu));

  std::vector<ui_element_ptr> menu_kids;
  for (auto& it : items) {
    if (it.label.empty()) {
      ui_element_ptr s = ui_element_ptr::create("wish"_key, "Separator"_key);
      assign_id(s);
      obj_ids.push_back(wish_id_of(s));
      menu_kids.push_back(s);
      continue;
    }
    ui_element_ptr mi = ui_element_ptr::create("wish"_key, "MenuItem"_key);
    mi["label"_key] = it.confirm ? it.label + "..." : it.label;
    assign_id(mi);
    obj_ids.push_back(wish_id_of(mi));
    menu_action_targets_[wish_id_of(mi)] = row_action{meta.scope, meta.name, meta.ns, it.action};
    menu_kids.push_back(mi);
  }
  set_children_list(menu, menu_kids);

  std::vector<ui_element_ptr> row_cells = cells;
  row_cells.push_back(menu);
  set_children_list(row, row_cells);

  meta.row = row;
  meta.child_key = next_key++;
  meta.object_ids = std::move(obj_ids);
  (*children)[meta.child_key] = dynamic_ptr{row};
  rows.push_back(std::move(meta));
}

void kubectl_frontend::set_status(list_window& lw, const std::string& text, bool ok) {
  if (!lw.status_label)
    return;
  lw.status_label["text"_key] = text;
  lw.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  lw.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void kubectl_frontend::rebuild_pods(const dynamic& args) {
  clear_list_rows(pods_, pod_rows_, next_pod_key_);

  int running = 0, total = 0;
  for_each_entry(args, "pods"_key, [&](const dynamic& e) {
    const std::string phase = e.as<std::string>("phase"_key);
    const std::string reason = e.as<std::string>("reason"_key);
    const std::string ready = e.as<std::string>("ready"_key);
    const std::string restarts = e.as<std::string>("restarts"_key);
    const std::string effective = reason.empty() ? phase : reason;
    auto [cl, cd] = pod_status_colour(effective, phase);

    list_row meta;
    meta.scope = "pod";
    meta.name = e.as<std::string>("name"_key);
    meta.ns = e.as<std::string>("namespace"_key);
    meta.state = phase;

    auto [rl, rd] = ready_colour(ready);
    const bool restarted = !restarts.empty() && restarts != "0";
    std::vector<ui_element_ptr> cells = {
        make_label(meta.ns, kIdleLight, kIdleDark),
        make_label(meta.name),
        make_label(ready, rl, rd),
        make_label(effective, cl, cd),
        make_label(restarts, restarted ? kWarnLight : nullptr, restarted ? kWarnDark : nullptr),
        make_label(e.as<std::string>("age"_key), kIdleLight, kIdleDark),
    };

    std::vector<menu_spec> items = {
        {"Logs", "logs", false}, {"Describe", "describe", false}, {}, {"Delete", "delete", true}};
    add_list_row(pods_, pod_rows_, next_pod_key_, std::move(meta), cells, items);
    ++total;
    if (phase == "Running")
      ++running;
  });

  if (pods_.table)
    pods_.table->refresh_children_order();
  apply_list_filter(pods_, pod_rows_);

  set_status(
      pods_, std::to_string(total) + (total == 1 ? " pod (" : " pods (") + std::to_string(running) + " running)", true);
}

void kubectl_frontend::rebuild_deployments(const dynamic& args) {
  clear_list_rows(deployments_, deployment_rows_, next_deployment_key_);

  int total = 0;
  for_each_entry(args, "deployments"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "deployment";
    meta.name = e.as<std::string>("name"_key);
    meta.ns = e.as<std::string>("namespace"_key);

    const std::string ready = e.as<std::string>("ready"_key);
    auto [rl, rd] = ready_colour(ready);
    std::vector<ui_element_ptr> cells = {
        make_label(meta.ns, kIdleLight, kIdleDark),
        make_label(meta.name),
        make_label(ready, rl, rd),
        make_label(e.as<std::string>("uptodate"_key)),
        make_label(e.as<std::string>("available"_key)),
        make_label(e.as<std::string>("age"_key), kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {
        {"Restart", "restart", false}, {"Describe", "describe", false}, {}, {"Delete", "delete", true}};
    add_list_row(deployments_, deployment_rows_, next_deployment_key_, std::move(meta), cells, items);
    ++total;
  });

  if (deployments_.table)
    deployments_.table->refresh_children_order();
  apply_list_filter(deployments_, deployment_rows_);
  set_status(deployments_, std::to_string(total) + (total == 1 ? " deployment" : " deployments"), true);
}

void kubectl_frontend::rebuild_services(const dynamic& args) {
  clear_list_rows(services_, service_rows_, next_service_key_);

  int total = 0;
  for_each_entry(args, "services"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "service";
    meta.name = e.as<std::string>("name"_key);
    meta.ns = e.as<std::string>("namespace"_key);

    std::vector<ui_element_ptr> cells = {
        make_label(meta.ns, kIdleLight, kIdleDark),
        make_label(meta.name),
        make_label(e.as<std::string>("type"_key)),
        make_label(e.as<std::string>("cluster_ip"_key), kIdleLight, kIdleDark),
        make_label(e.as<std::string>("ports"_key)),
        make_label(e.as<std::string>("age"_key), kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {{"Describe", "describe", false}, {}, {"Delete", "delete", true}};
    add_list_row(services_, service_rows_, next_service_key_, std::move(meta), cells, items);
    ++total;
  });

  if (services_.table)
    services_.table->refresh_children_order();
  apply_list_filter(services_, service_rows_);
  set_status(services_, std::to_string(total) + (total == 1 ? " service" : " services"), true);
}

void kubectl_frontend::rebuild_nodes(const dynamic& args) {
  clear_list_rows(nodes_, node_rows_, next_node_key_);

  int ready = 0, total = 0;
  for_each_entry(args, "nodes"_key, [&](const dynamic& e) {
    const std::string status = e.as<std::string>("status"_key);
    const bool schedulable = e.as<std::string>("schedulable"_key) == "true";
    auto [cl, cd] = node_status_colour(status);

    list_row meta;
    meta.scope = "node";
    meta.name = e.as<std::string>("name"_key);
    meta.state = status;

    std::vector<ui_element_ptr> cells = {
        make_label(meta.name),
        make_label(status, cl, cd),
        make_label(e.as<std::string>("version"_key), kIdleLight, kIdleDark),
        make_label(e.as<std::string>("age"_key), kIdleLight, kIdleDark),
    };
    // Cordon on a schedulable node; Uncordon on a cordoned one.
    std::vector<menu_spec> items;
    if (schedulable)
      items.push_back({"Cordon", "cordon", false});
    else
      items.push_back({"Uncordon", "uncordon", false});
    items.push_back({"Drain", "drain", true});
    items.push_back({});
    items.push_back({"Describe", "describe", false});
    add_list_row(nodes_, node_rows_, next_node_key_, std::move(meta), cells, items);
    ++total;
    if (contains(status, "Ready") && !contains(status, "NotReady"))
      ++ready;
  });

  if (nodes_.table)
    nodes_.table->refresh_children_order();
  apply_list_filter(nodes_, node_rows_);
  set_status(
      nodes_, std::to_string(total) + (total == 1 ? " node (" : " nodes (") + std::to_string(ready) + " ready)", true);
}

void kubectl_frontend::apply_list_filter(list_window& lw, std::vector<list_row>& rows) {
  auto lc = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
  };
  const std::string name_needle = lc(lw.name_filter);
  const std::string ns_needle = lc(lw.ns_filter);

  static const char* kPhaseNames[] = {"", "Running", "Pending", "Succeeded", "Failed"};

  for (auto& r : rows) {
    if (!r.row)
      continue;
    bool show = true;
    if (lw.phase_combo_id.id && lw.phase_filter > 0 && lw.phase_filter < 5)
      show = r.state == kPhaseNames[lw.phase_filter];
    if (show && !ns_needle.empty())
      show = lc(r.ns).find(ns_needle) != std::string::npos;
    if (show && !name_needle.empty())
      show = lc(r.name).find(name_needle) != std::string::npos;
    r.row["visible"_key] = show;
  }
}

// ── RMI methods ────────────────────────────────────────────────────────────

dynamic kubectl_frontend::do_update_pods(const dynamic& args) {
  rebuild_pods(args);
  return dynamic{};
}
dynamic kubectl_frontend::do_update_deployments(const dynamic& args) {
  rebuild_deployments(args);
  return dynamic{};
}
dynamic kubectl_frontend::do_update_services(const dynamic& args) {
  rebuild_services(args);
  return dynamic{};
}
dynamic kubectl_frontend::do_update_nodes(const dynamic& args) {
  rebuild_nodes(args);
  return dynamic{};
}

dynamic kubectl_frontend::do_update_logs(const dynamic& args) {
  if (args.as<std::string>("name"_key) != open_logs_name_ ||
      args.as<std::string>("namespace"_key) != open_logs_ns_)
    return dynamic{}; // stale response for a pod the user navigated away from.
  if (logs_target_label_)
    logs_target_label_["text"_key] = args.as<std::string>("title"_key);
  set_text_lines(logs_table_, logs_line_ids_, next_logs_line_key_, args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic kubectl_frontend::do_update_describe(const dynamic& args) {
  if (args.as<std::string>("name"_key) != open_describe_name_ ||
      args.as<std::string>("namespace"_key) != open_describe_ns_ ||
      args.as<std::string>("kind"_key) != open_describe_kind_)
    return dynamic{};
  if (describe_target_label_)
    describe_target_label_["text"_key] = args.as<std::string>("title"_key);
  set_text_lines(describe_table_, describe_line_ids_, next_describe_line_key_, args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic kubectl_frontend::do_command_result(const dynamic& args) {
  const std::string command = args.as<std::string>("command"_key);
  const bool ok = args.as<bool>("ok"_key);
  const std::string output = args.as<std::string>("output"_key);
  const std::string scope =
      args.findField<std::string>("scope"_key) ? args.as<std::string>("scope"_key) : std::string{"pods"};
  list_window* lw = scope == "deployments" ? &deployments_
      : scope == "services"                ? &services_
      : scope == "nodes"                   ? &nodes_
                                           : &pods_;
  if (ok)
    set_status(*lw, command + ": OK", true);
  else
    set_status(*lw, command + " failed: " + (output.empty() ? "unknown error" : output), false);
  return dynamic{};
}

// ── Console window (client `kubectl` subprocess trace) ─────────────────────

void kubectl_frontend::append_console_row(
    const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
  if (!console_table_)
    return;
  auto* children_p = console_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  const char* cl = ok ? kOkLight : kBadLight;
  const char* cd = ok ? kOkDark : kBadDark;

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);

  ui_element_ptr cell_seq = make_label(std::to_string(++console_seq_), kIdleLight, kIdleDark);
  ui_element_ptr cell_command = make_label(command, cl, cd);
  ui_element_ptr cell_exit = make_label(std::to_string(exit_code), cl, cd);
  ui_element_ptr cell_output = make_label(output, cl, cd);

  // Right-click any row for "Copy Entry" (this row's command/exit/output,
  // via MenuItem.copy_text) and "Clear Console" (every row). git's Log window.
  ui_element_ptr context_menu = ui_element_ptr::create("wish"_key, "ContextMenu"_key);
  assign_id(context_menu);

  ui_element_ptr copy_item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  copy_item["label"_key] = std::string{"Copy Entry"};
  copy_item["copy_text"_key] = command + "\nexit: " + std::to_string(exit_code) + "\n" + output;
  assign_id(copy_item);

  ui_element_ptr clear_item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  clear_item["label"_key] = std::string{"Clear Console"};
  assign_id(clear_item);
  click_handlers_[wish_id_of(clear_item)] = [this] { clear_console_rows(); };

  set_children_list(context_menu, {copy_item, clear_item});
  set_children_list(row, {cell_seq, cell_command, cell_exit, cell_output, context_menu});

  console_row_entry entry;
  entry.child_key = next_console_child_key_++;
  entry.object_ids = {
      wish_id_of(row),       wish_id_of(cell_seq),      wish_id_of(cell_command), wish_id_of(cell_exit),
      wish_id_of(cell_output), wish_id_of(context_menu), wish_id_of(copy_item),   wish_id_of(clear_item)};
  (*children)[entry.child_key] = dynamic_ptr{row};
  console_rows_.push_back(std::move(entry));

  if (console_rows_.size() > kMaxConsoleRows) {
    erase_console_row_objects(console_rows_.front());
    children->erase(console_rows_.front().child_key);
    console_rows_.pop_front();
  }
  console_table_->refresh_children_order();
}

void kubectl_frontend::erase_console_row_objects(const console_row_entry& entry) {
  for (auto id : entry.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

void kubectl_frontend::clear_console_rows() {
  if (!console_table_)
    return;
  auto* children_p = console_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto& entry : console_rows_) {
    erase_console_row_objects(entry);
    children->erase(entry.child_key);
  }
  console_rows_.clear();
  console_seq_ = 0;
  next_console_child_key_ = 0; // every numeric child key was just erased.
  console_table_->refresh_children_order();
}

dynamic kubectl_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      args.as<std::string>("command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      args.as<std::string>("output"_key));
  return dynamic{};
}

// ── Top window (live `kubectl top` graphs) ─────────────────────────────────

void kubectl_frontend::build_top_window() {
  auto tree = import_json(kTopLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
  top_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.status", [&](const auto& e) { top_status_label_ = e; });
  tree.with("vbox.pods_table", [&](const auto& e) { top_pods_table_ = e; });
  tree.with("vbox.nodes_table", [&](const auto& e) { top_nodes_table_ = e; });
  tree.with("vbox.pods_cpu_plot", [&](const auto& e) { init_stats_plot(pods_cpu_plot_, e, "Total", false); });
  tree.with("vbox.pods_mem_plot", [&](const auto& e) { init_stats_plot(pods_mem_plot_, e, "Total", false); });
  tree.with("vbox.nodes_cpu_plot", [&](const auto& e) { init_stats_plot(nodes_cpu_plot_, e, "Cluster avg", true); });
  tree.with("vbox.nodes_mem_plot", [&](const auto& e) { init_stats_plot(nodes_mem_plot_, e, "Cluster avg", true); });

  // X is a rolling sample index: hide its numeric labels
  // (ImPlotAxisFlags_NoTickLabels = 1 << 3) and keep it continuously
  // auto-fitted to the collected history (ImPlotAxisFlags_AutoFit = 1 << 11)
  // so the trace always fills the frame from the first sample. The node %
  // plots are true 0..100 gauges; the pod millicore / MiB plots auto-fit Y.
  constexpr int32_t kNoTickLabels = 1 << 3;
  constexpr int32_t kAutoFit = 1 << 11;
  constexpr int32_t kLegendSouth = 1 << 1;   // ImPlotLocation_South
  constexpr int32_t kLegendOutside = 1 << 4; // ImPlotLegendFlags_Outside
  // A busy cluster puts a dozen-plus pod lines on the plot. Keep the legend
  // below the frame as a single vertical column: ImPlot shrinks the trace
  // area to fit the *whole* column (no clipping), so every pod/node line
  // stays labelled. A horizontal legend would be clamped to the plot width
  // and crop the tail entries. The Top window is a scrolling VerticalLayout,
  // so the extra height is absorbed by the scroll region.
  auto legend_below = [&](const auto& e) {
    e["legend_location"_key] = kLegendSouth;
    e["legend_flags"_key] = kLegendOutside;
  };
  auto fit_xy = [&](const auto& e) {
    e["x_flags"_key] = kNoTickLabels | kAutoFit;
    e["y_flags"_key] = kAutoFit;
    legend_below(e);
  };
  auto fit_x_pct_y = [&](const auto& e) {
    e["x_flags"_key] = kNoTickLabels | kAutoFit;
    e["y_min"_key] = 0.0f;
    e["y_max"_key] = 100.0f;
    legend_below(e);
  };
  tree.with("vbox.pods_cpu_plot", fit_xy);
  tree.with("vbox.pods_mem_plot", fit_xy);
  tree.with("vbox.nodes_cpu_plot", fit_x_pct_y);
  tree.with("vbox.nodes_mem_plot", fit_x_pct_y);

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), top_root_key_);
  sess().top_level_objects[key_t{top_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{top_root_key_}] = this;
  (*root_ptr)["__path__"_key] = top_root_key_;
}

void kubectl_frontend::init_stats_plot(
    stats_plot& sp, const ui_element_ptr& plot_el, const std::string& aggregate_label, bool average) {
  sp.plot = plot_el;
  sp.average = average;
  auto* children_p = plot_el->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  ui_element_ptr line = ui_element_ptr::create("wish"_key, "PlotLine"_key);
  line["label"_key] = aggregate_label;
  line["order"_key] = static_cast<int32_t>(0);
  assign_id(line);
  sp.aggregate.el = line;
  sp.aggregate.id = wish_id_of(line);
  sp.aggregate.child_key = 0;
  sp.next_child_key = 0;
  (**children_p)[static_cast<size_t>(0)] = dynamic_ptr{line};
  plot_el->refresh_children_order();
}

void kubectl_frontend::push_stats_history(std::vector<float>& history, float value) {
  history.push_back(value);
  if (history.size() > kMaxStatsHistory)
    history.erase(history.begin());
}

void kubectl_frontend::update_stats_xs(size_t count) {
  if (stats_xs_.size() == count)
    return;
  stats_xs_.resize(count);
  for (size_t i = 0; i < count; ++i)
    stats_xs_[i] = static_cast<float>(i);
}

void kubectl_frontend::update_stats_plot(stats_plot& sp, const std::map<std::string, float>& values) {
  if (!sp.plot)
    return;
  auto* children_p = sp.plot->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  float agg = 0.0f;
  for (const auto& [name, v] : values)
    agg += v;
  if (sp.average && !values.empty())
    agg /= static_cast<float>(values.size());
  push_stats_history(sp.aggregate.hist, agg);

  std::vector<std::pair<std::string, float>> ranked(values.begin(), values.end());
  std::stable_sort(
      ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::set<std::string> keep;
  for (size_t i = 0; i < ranked.size() && i < kMaxStatsSeries; ++i)
    keep.insert(ranked[i].first);

  for (auto it = sp.series.begin(); it != sp.series.end();) {
    if (keep.count(it->first)) {
      ++it;
      continue;
    }
    children->erase(it->second.child_key);
    ctx().objects.erase(it->second.id.id);
    it = sp.series.erase(it);
  }

  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (const auto& name : keep) {
    if (sp.series.count(name))
      continue;
    ui_element_ptr line = ui_element_ptr::create("wish"_key, "PlotLine"_key);
    line["label"_key] = name;
    line["order"_key] = static_cast<int32_t>(1);
    assign_id(line);
    stats_series s;
    s.el = line;
    s.id = wish_id_of(line);
    s.child_key = ++sp.next_child_key;
    s.hist.assign(sp.aggregate.hist.empty() ? 0 : sp.aggregate.hist.size() - 1, nan);
    (*children)[s.child_key] = dynamic_ptr{line};
    sp.series.emplace(name, std::move(s));
  }

  for (const auto& name : keep)
    push_stats_history(sp.series.at(name).hist, values.at(name));

  update_stats_xs(sp.aggregate.hist.size());

  sp.aggregate.el["xs"_key] = stats_xs_;
  sp.aggregate.el["ys"_key] = sp.aggregate.hist;
  for (auto& [name, s] : sp.series) {
    s.el["xs"_key] = stats_xs_;
    s.el["ys"_key] = s.hist;
  }
  sp.plot->refresh_children_order();
}

void kubectl_frontend::rebuild_top_table(
    const ui_element_ptr& table, std::vector<key_t>& row_ids, size_t& next_key, const dynamic& args,
    key_t array_key, const std::function<std::vector<ui_element_ptr>(const dynamic&)>& make_cells) {
  if (!table)
    return;
  auto* children_p = table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() &&
        f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  for (auto id : row_ids)
    ctx().objects.erase(id.id);
  row_ids.clear();
  next_key = 0;

  for_each_entry(args, array_key, [&](const dynamic& e) {
    std::vector<ui_element_ptr> cells = make_cells(e);
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);
    for (auto& cell : cells)
      row_ids.push_back(wish_id_of(cell));
    row_ids.push_back(wish_id_of(row));
    set_children_list(row, cells);
    (*children)[next_key++] = dynamic_ptr{row};
  });
  table->refresh_children_order();
}

dynamic kubectl_frontend::do_update_stats(const dynamic& args) {
  std::map<std::string, float> pod_cpu;
  std::map<std::string, float> pod_mem;
  std::map<std::string, float> node_cpu;
  std::map<std::string, float> node_mem;
  size_t pod_count = 0;
  size_t node_count = 0;

  for_each_entry(args, "pods"_key, [&](const dynamic& e) {
    // Pod names carry a unique suffix (ReplicaSet/DaemonSet hash, or the node
    // name for static control-plane pods), so the bare name is a safe series
    // key -- and a far shorter legend label than "namespace/name". The
    // namespace is still its own column in the pods table.
    const std::string key = e.as<std::string>("name"_key);
    pod_cpu[key] = e.as<float>("cpu_m"_key);
    pod_mem[key] = e.as<float>("mem_mib"_key);
    ++pod_count;
  });
  for_each_entry(args, "nodes"_key, [&](const dynamic& e) {
    const std::string key = e.as<std::string>("name"_key);
    node_cpu[key] = e.as<float>("cpu_pct"_key);
    node_mem[key] = e.as<float>("mem_pct"_key);
    ++node_count;
  });

  update_stats_plot(pods_cpu_plot_, pod_cpu);
  update_stats_plot(pods_mem_plot_, pod_mem);
  update_stats_plot(nodes_cpu_plot_, node_cpu);
  update_stats_plot(nodes_mem_plot_, node_mem);

  rebuild_top_table(
      top_pods_table_, top_pods_row_ids_, next_top_pods_key_, args, "pods"_key, [&](const dynamic& e) {
        return std::vector<ui_element_ptr>{
            make_label(e.as<std::string>("namespace"_key)),
            make_label(e.as<std::string>("name"_key)),
            make_label(e.as<std::string>("cpu"_key)),
            make_label(e.as<std::string>("mem"_key)),
        };
      });
  rebuild_top_table(
      top_nodes_table_, top_nodes_row_ids_, next_top_nodes_key_, args, "nodes"_key, [&](const dynamic& e) {
        return std::vector<ui_element_ptr>{
            make_label(e.as<std::string>("name"_key)),
            make_label(e.as<std::string>("cpu"_key)),
            make_label(format_pct(e.as<float>("cpu_pct"_key))),
            make_label(e.as<std::string>("mem"_key)),
            make_label(format_pct(e.as<float>("mem_pct"_key))),
        };
      });

  if (top_status_label_) {
    std::string error;
    if (const auto* ef = args.findField<std::string>("error"_key))
      error = *ef;
    if (!error.empty()) {
      top_status_label_["text"_key] = "kubectl top unavailable: " + error;
      top_status_label_["text_color_light"_key] = std::string{kBadLight};
      top_status_label_["text_color_dark"_key] = std::string{kBadDark};
    } else {
      top_status_label_["text"_key] =
          std::to_string(pod_count) + " pods, " + std::to_string(node_count) + " nodes";
      top_status_label_["text_color_light"_key] = std::string{kIdleLight};
      top_status_label_["text_color_dark"_key] = std::string{kIdleDark};
    }
  }
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void kubectl_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == pods_.window_id || id == deployments_.window_id || id == services_.window_id ||
       id == nodes_.window_id || id == logs_window_id_ || id == describe_window_id_ ||
       id == console_window_id_ || id == top_window_id_)) {
    emit("closed"_key);
    remove_objects_at(deployments_.root_key);
    remove_objects_at(services_.root_key);
    remove_objects_at(nodes_.root_key);
    remove_objects_at(logs_root_key_);
    remove_objects_at(describe_root_key_);
    remove_objects_at(console_root_key_);
    remove_objects_at(top_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    for (auto* lw : {&pods_, &deployments_, &services_, &nodes_}) {
      auto& rows = lw == &pods_ ? pod_rows_
          : lw == &deployments_ ? deployment_rows_
          : lw == &services_    ? service_rows_
                                : node_rows_;
      if (id == lw->name_filter_id) {
        lw->name_filter = payload.as<std::string>("value"_key);
        apply_list_filter(*lw, rows);
        return;
      }
      if (lw->ns_filter_id.id && id == lw->ns_filter_id) {
        lw->ns_filter = payload.as<std::string>("value"_key);
        apply_list_filter(*lw, rows);
        return;
      }
      if (lw->phase_combo_id.id && id == lw->phase_combo_id) {
        lw->phase_filter = payload.as<int32_t>("value"_key);
        apply_list_filter(*lw, rows);
        return;
      }
    }
    if (id == logs_follow_id_) {
      logs_follow_ = payload.as<bool>("value"_key);
      emit_logs_request();
    } else if (id == logs_lines_id_) {
      logs_lines_ = payload.as<int32_t>("value"_key);
    }
    return;
  }

  if (event != "clicked"_key)
    return;

  if (auto ch = click_handlers_.find(id); ch != click_handlers_.end()) {
    ch->second();
    return;
  }

  auto mi = menu_action_targets_.find(id);
  if (mi == menu_action_targets_.end())
    return;
  const row_action target = mi->second;

  const std::string qualified = target.ns.empty() ? target.name : target.ns + "/" + target.name;

  if (target.action == "logs") {
    open_logs_name_ = target.name;
    open_logs_ns_ = target.ns;
    if (logs_target_label_)
      logs_target_label_["text"_key] = qualified;
    emit_logs_request();
    return;
  }
  if (target.action == "describe") {
    open_describe_name_ = target.name;
    open_describe_ns_ = target.ns;
    open_describe_kind_ = target.scope;
    if (describe_target_label_)
      describe_target_label_["text"_key] = target.scope + ": " + qualified;
    emit_describe_request();
    return;
  }

  const key_t event_name = target.scope == "pod" ? "pod_action_requested"_key
      : target.scope == "deployment"             ? "deployment_action_requested"_key
      : target.scope == "service"                ? "service_action_requested"_key
                                                 : "node_action_requested"_key;
  auto fire = [this, event_name, target] {
    dynamic p;
    p["name"_key] = target.name;
    if (!target.ns.empty())
      p["namespace"_key] = target.ns;
    p["action"_key] = target.action;
    emit(event_name, std::move(p));
  };

  const bool destructive = target.action == "delete" || target.action == "drain";
  if (!destructive) {
    fire();
    return;
  }

  std::string message;
  if (target.action == "drain")
    message = "Drain node '" + target.name + "'? Its running pods will be evicted.";
  else
    message = std::string{"Delete "} + noun_for(target.scope) + " '" + target.name + "'" +
        (target.ns.empty() ? std::string{} : " in namespace '" + target.ns + "'") + "?";
  show_confirm(message, fire);
}

// ── Registration ───────────────────────────────────────────────────────────

void register_kubectl() {
  auto proto = dynamic_ptr{"KubectlFrontend"_key, {}};

  proto->addField("title"_key, field{std::string{"Kubernetes"}});

  auto add_method = [&](key_t name, dynamic (kubectl_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<kubectl_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_pods"_key, &kubectl_frontend::do_update_pods);
  add_method("update_deployments"_key, &kubectl_frontend::do_update_deployments);
  add_method("update_services"_key, &kubectl_frontend::do_update_services);
  add_method("update_nodes"_key, &kubectl_frontend::do_update_nodes);
  add_method("update_logs"_key, &kubectl_frontend::do_update_logs);
  add_method("update_describe"_key, &kubectl_frontend::do_update_describe);
  add_method("command_result"_key, &kubectl_frontend::do_command_result);
  add_method("append_command_log"_key, &kubectl_frontend::do_append_command_log);
  add_method("update_stats"_key, &kubectl_frontend::do_update_stats);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("KubectlFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Kubernetes-dashboard-style GUI frontend for the local `kubectl` CLI. All `kubectl` invocation "
      "happens client-side; this form only renders whatever snapshot it was last given. Listen for the "
      "'closed' event to detect when the user is done, and the '*_requested' events to react to user "
      "actions -- see kubectl.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U},
      dynamic::make_factory<kubectl_frontend>("wish"_key, "KubectlFrontend"_key));
}

} // namespace bdg::wish
