// MIT License © 2026 Binary Dice Games
/// @file kubectl.cpp
/// @brief Implementation of the KubectlFrontend form.
///
/// Inline JSON window layouts + C++-built table rows on the panels shared
/// by the bdg tool forms (modules/bdg/common/server). The four list windows
/// (Pods / Deployments / Services / Nodes) share one build_list_window() /
/// apply_list_filter() / run_row_action() path.
#include "kubectl.hpp"

#include "src/bison/bison_object.hpp"

#include <ui/dock_layout_spec.hpp>

#include <iomanip>
#include <map>
#include <sstream>

namespace bdg::wish {

using namespace bison;
using common::for_each_entry;
using common::kBad;
using common::kIdle;
using common::kOk;
using common::kWarn;
using common::theme_color;
using common::wish_id_of;

namespace {

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// Pod .status.phase (+ container waiting reason) -> colour.
theme_color pod_status_colour(const std::string& effective, const std::string& phase) {
  if (contains(effective, "BackOff") || contains(effective, "Err") || contains(effective, "Crash") ||
      contains(effective, "Invalid") || contains(effective, "Failed") || phase == "Failed")
    return kBad;
  if (phase == "Running")
    return kOk;
  if (phase == "Pending" || contains(effective, "Creating") || contains(effective, "Init") ||
      contains(effective, "Waiting"))
    return kWarn;
  return kIdle; // Succeeded, Completed, Unknown, ...
}

// Node "Ready"/"NotReady"[,SchedulingDisabled] -> colour.
theme_color node_status_colour(const std::string& status) {
  if (contains(status, "NotReady"))
    return kBad;
  if (contains(status, "SchedulingDisabled"))
    return kWarn;
  return kOk;
}

// "a/b" -> ok when a == b and a != "0"; warn otherwise.
theme_color ready_colour(const std::string& ready) {
  auto slash = ready.find('/');
  if (slash != std::string::npos) {
    std::string have = ready.substr(0, slash);
    std::string want = ready.substr(slash + 1);
    if (have == want && have != "0" && !have.empty())
      return kOk;
  }
  return kWarn;
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
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 },
      "state":       { "type": "Combo", "items": "All\nRunning\nPending\nSucceeded\nFailed", "value": 0, "width": 120 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pods_table", "columns": 7,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
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
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##deployments_table", "columns": 7,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
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
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##services_table", "columns": 7,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
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
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##nodes_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
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

// Logs is common::text_viewer_panel's toolbar + read-only TextEditor with
// Follow / Lines controls added ("auto_scroll": true so it follows the
// newest line as `kubectl logs` output arrives). Describe uses the panel's
// default layout.

static constexpr const char* kLogsLayout = R"json({
  "type": "Window", "title": "Logs", "width": 900, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":     { "type": "Label", "text": "(no pod selected)" },
      "spring":     { "type": "Spring" },
      "follow":     { "type": "Checkbox", "label": "Follow", "value": false },
      "lines":      { "type": "InputInt", "label": "Lines", "value": 500, "step": 100, "width": 130 },
      "btn_refresh":{ "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" }
    } },
    "sep": { "type": "Separator" },
    "editor": {
      "type": "TextEditor", "file_path": "", "language": "log", "read_only": true,
      "auto_scroll": true, "width": -1, "height": -1
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
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
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
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
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

kubectl_frontend::kubectl_frontend(dynamic&& base) : tool_form(std::move(base)) {}

void kubectl_frontend::on_init() {
  internal_root_key_ = next_available_key("__kubectl_");

  auto* title_f = findField<std::string>("title"_key);
  title_ = title_f ? *title_f : std::string{"Kubernetes"};

  // Pods is the main root -- form::init() registers internal_root_key_ as
  // this form's top-level object automatically; the other windows register
  // themselves (tool_form::build_window()).
  build_list_window(pods_, kPodsLayout, internal_root_key_);
  build_list_window(deployments_, kDeploymentsLayout, internal_root_key_ + "_deployments");
  build_list_window(services_, kServicesLayout, internal_root_key_ + "_services");
  build_list_window(nodes_, kNodesLayout, internal_root_key_ + "_nodes");

  common::text_viewer_options logs_options;
  logs_options.layout_json = kLogsLayout;
  logs_options.file_stem = "logs";
  logs_options.on_refresh = [this] { emit_logs_request(); };
  logs_.build(*this, internal_root_key_ + "_logs", std::move(logs_options), [&](ui_tree& tree) {
    tree.with("vbox.toolbar.follow", [&](const auto& e) { logs_follow_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.lines", [&](const auto& e) { logs_lines_id_ = wish_id_of(e); });
  });

  describe_.build(
      *this,
      internal_root_key_ + "_describe",
      {.title = "Describe", .language = "yaml", .file_stem = "describe", .on_refresh = [this] {
         emit_describe_request();
       }});

  console_.build(
      *this,
      internal_root_key_ + "_console",
      {.table_id = "##kubectl_console_table", .width = 960, .command_width = 380});

  top_root_key_ = internal_root_key_ + "_top";
  build_top_window();

  // Seed the first-run arrangement (mirrors docker): a wide left column of
  // tabbed list/top windows over a Console strip, and a narrower right
  // column with Logs + Describe. Owned by imgui.ini after the first run;
  // bump the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "kubectl_dock", "Kubectl",
        layout(
            split(
                dir::left, 0.62f,
                split(
                    dir::down,
                    0.24f,
                    area({console_.root_key()}),
                    area(
                        {internal_root_key_,
                         deployments_.panel.root_key(),
                         services_.panel.root_key(),
                         nodes_.panel.root_key(),
                         top_root_key_},
                        internal_root_key_)),
                area({logs_.root_key(), describe_.root_key()}, logs_.root_key())),
            /*version=*/1,
            /*target=*/"kubectl_dock")));
  }

  // Initial population is triggered client-side (run_kubectl() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event (docker's / git's documented initial-load-race
  // fix).
}

void kubectl_frontend::build_list_window(list_window& lw, const char* layout_json, const std::string& root_key) {
  lw.panel.build(*this, root_key, layout_json, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) { lw.name_filter_id = wish_id_of(e); });
    tree.with("vbox.toolbar.ns", [&](const auto& e) { lw.ns_filter_id = wish_id_of(e); });
    tree.with("vbox.toolbar.state", [&](const auto& e) { lw.phase_combo_id = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { emit("refresh_requested"_key); });
    });
  });
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

common::menu_item
kubectl_frontend::action_item(const std::string& label, const resource& r, const std::string& action, bool confirm) {
  return {label, [this, r, action] { run_row_action(r, action); }, confirm};
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void kubectl_frontend::rebuild_pods(const dynamic& args) {
  pods_.panel.clear();

  int running = 0, total = 0;
  for_each_entry(args, "pods"_key, [&](const dynamic& e) {
    const std::string phase = e.as<std::string>("phase"_key);
    const std::string reason = e.as<std::string>("reason"_key);
    const std::string ready = e.as<std::string>("ready"_key);
    const std::string restarts = e.as<std::string>("restarts"_key);
    const std::string effective = reason.empty() ? phase : reason;

    resource r{"pod", e.as<std::string>("name"_key), e.as<std::string>("namespace"_key), phase};
    const bool restarted = !restarts.empty() && restarts != "0";
    std::vector<ui_element_ptr> cells = {
        make_label(r.ns, kIdle),
        make_label(r.name),
        make_label(ready, ready_colour(ready)),
        make_label(effective, pod_status_colour(effective, phase)),
        restarted ? make_label(restarts, kWarn) : make_label(restarts),
        make_label(e.as<std::string>("age"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Logs", r, "logs", false),
        action_item("Describe", r, "describe", false),
        {},
        action_item("Delete", r, "delete", true)};
    pods_.panel.add(std::move(r), cells, items);
    ++total;
    if (phase == "Running")
      ++running;
  });

  pods_.panel.refresh();
  apply_list_filter(pods_);
  pods_.panel.set_status(
      std::to_string(total) + (total == 1 ? " pod (" : " pods (") + std::to_string(running) + " running)", true);
}

void kubectl_frontend::rebuild_deployments(const dynamic& args) {
  deployments_.panel.clear();

  int total = 0;
  for_each_entry(args, "deployments"_key, [&](const dynamic& e) {
    resource r{"deployment", e.as<std::string>("name"_key), e.as<std::string>("namespace"_key), {}};
    const std::string ready = e.as<std::string>("ready"_key);
    std::vector<ui_element_ptr> cells = {
        make_label(r.ns, kIdle),
        make_label(r.name),
        make_label(ready, ready_colour(ready)),
        make_label(e.as<std::string>("uptodate"_key)),
        make_label(e.as<std::string>("available"_key)),
        make_label(e.as<std::string>("age"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Restart", r, "restart", false),
        action_item("Describe", r, "describe", false),
        {},
        action_item("Delete", r, "delete", true)};
    deployments_.panel.add(std::move(r), cells, items);
    ++total;
  });

  deployments_.panel.refresh();
  apply_list_filter(deployments_);
  deployments_.panel.set_status(std::to_string(total) + (total == 1 ? " deployment" : " deployments"), true);
}

void kubectl_frontend::rebuild_services(const dynamic& args) {
  services_.panel.clear();

  int total = 0;
  for_each_entry(args, "services"_key, [&](const dynamic& e) {
    resource r{"service", e.as<std::string>("name"_key), e.as<std::string>("namespace"_key), {}};
    std::vector<ui_element_ptr> cells = {
        make_label(r.ns, kIdle),
        make_label(r.name),
        make_label(e.as<std::string>("type"_key)),
        make_label(e.as<std::string>("cluster_ip"_key), kIdle),
        make_label(e.as<std::string>("ports"_key)),
        make_label(e.as<std::string>("age"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Describe", r, "describe", false), {}, action_item("Delete", r, "delete", true)};
    services_.panel.add(std::move(r), cells, items);
    ++total;
  });

  services_.panel.refresh();
  apply_list_filter(services_);
  services_.panel.set_status(std::to_string(total) + (total == 1 ? " service" : " services"), true);
}

void kubectl_frontend::rebuild_nodes(const dynamic& args) {
  nodes_.panel.clear();

  int ready = 0, total = 0;
  for_each_entry(args, "nodes"_key, [&](const dynamic& e) {
    const std::string status = e.as<std::string>("status"_key);
    const bool schedulable = e.as<std::string>("schedulable"_key) == "true";

    resource r{"node", e.as<std::string>("name"_key), {}, status};
    std::vector<ui_element_ptr> cells = {
        make_label(r.name),
        make_label(status, node_status_colour(status)),
        make_label(e.as<std::string>("version"_key), kIdle),
        make_label(e.as<std::string>("age"_key), kIdle),
    };
    // Cordon on a schedulable node; Uncordon on a cordoned one.
    std::vector<common::menu_item> items = {
        schedulable ? action_item("Cordon", r, "cordon", false) : action_item("Uncordon", r, "uncordon", false),
        action_item("Drain", r, "drain", true),
        {},
        action_item("Describe", r, "describe", false)};
    nodes_.panel.add(std::move(r), cells, items);
    ++total;
    if (contains(status, "Ready") && !contains(status, "NotReady"))
      ++ready;
  });

  nodes_.panel.refresh();
  apply_list_filter(nodes_);
  nodes_.panel.set_status(
      std::to_string(total) + (total == 1 ? " node (" : " nodes (") + std::to_string(ready) + " ready)", true);
}

void kubectl_frontend::apply_list_filter(list_window& lw) {
  const std::string name_needle = common::lower(lw.name_filter);
  const std::string ns_needle = common::lower(lw.ns_filter);
  static const char* kPhaseNames[] = {"", "Running", "Pending", "Succeeded", "Failed"};
  const bool by_phase = lw.phase_combo_id.id && lw.phase_filter > 0 && lw.phase_filter < 5;

  lw.panel.apply_filter([&](const resource& r) {
    if (by_phase && r.state != kPhaseNames[lw.phase_filter])
      return false;
    if (!ns_needle.empty() && common::lower(r.ns).find(ns_needle) == std::string::npos)
      return false;
    return name_needle.empty() || common::lower(r.name).find(name_needle) != std::string::npos;
  });
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
  logs_.set_title(args.as<std::string>("title"_key));
  logs_.set_text(args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic kubectl_frontend::do_update_describe(const dynamic& args) {
  if (args.as<std::string>("name"_key) != open_describe_name_ ||
      args.as<std::string>("namespace"_key) != open_describe_ns_ ||
      args.as<std::string>("kind"_key) != open_describe_kind_)
    return dynamic{};
  describe_.set_title(args.as<std::string>("title"_key));
  describe_.set_text(args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic kubectl_frontend::do_command_result(const dynamic& args) {
  const std::string scope = common::str_of(args, "scope"_key);
  list_window& lw = scope == "deployments" ? deployments_
      : scope == "services"                ? services_
      : scope == "nodes"                   ? nodes_
                                           : pods_;
  bool ok = false;
  const std::string text = common::command_result_text(args, ok);
  lw.panel.set_status(text, ok);
  return dynamic{};
}

dynamic kubectl_frontend::do_append_command_log(const dynamic& args) {
  console_.append_from(args);
  return dynamic{};
}

// ── Top window (live `kubectl top` graphs) ─────────────────────────────────

void kubectl_frontend::build_top_window() {
  build_window(top_root_key_, kTopLayout, top_window_id_, [&](ui_tree& tree) {
    using y_axis = common::rolling_plot::y_axis;
    tree.with("vbox.status", [&](const auto& e) { top_status_label_ = e; });
    tree.with("vbox.pods_table", [&](const auto& e) { top_pods_.attach(*this, e); });
    tree.with("vbox.nodes_table", [&](const auto& e) { top_nodes_.attach(*this, e); });
    // The node % plots are true 0..100 gauges; the pod millicore / MiB
    // plots auto-fit Y. The Top window is a scrolling VerticalLayout, so the
    // legend column below each plot is absorbed by the scroll region.
    tree.with("vbox.pods_cpu_plot", [&](const auto& e) {
      common::rolling_plot::configure_axes(e, y_axis::auto_fit);
      pods_cpu_plot_.init(*this, e, "Total");
    });
    tree.with("vbox.pods_mem_plot", [&](const auto& e) {
      common::rolling_plot::configure_axes(e, y_axis::auto_fit);
      pods_mem_plot_.init(*this, e, "Total");
    });
    tree.with("vbox.nodes_cpu_plot", [&](const auto& e) {
      common::rolling_plot::configure_axes(e, y_axis::percent);
      nodes_cpu_plot_.init(*this, e, "Cluster avg", /*average=*/true);
    });
    tree.with("vbox.nodes_mem_plot", [&](const auto& e) {
      common::rolling_plot::configure_axes(e, y_axis::percent);
      nodes_mem_plot_.init(*this, e, "Cluster avg", /*average=*/true);
    });
  });
}

void kubectl_frontend::rebuild_top_table(
    common::table_rows<>& rows,
    const dynamic& args,
    key_t array_key,
    const std::function<std::vector<ui_element_ptr>(const dynamic&)>& make_cells) {
  rows.clear();
  for_each_entry(args, array_key, [&](const dynamic& e) { rows.add({}, make_cells(e)); });
  rows.refresh();
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

  pods_cpu_plot_.push(pod_cpu);
  pods_mem_plot_.push(pod_mem);
  nodes_cpu_plot_.push(node_cpu);
  nodes_mem_plot_.push(node_mem);

  rebuild_top_table(top_pods_, args, "pods"_key, [&](const dynamic& e) {
    return std::vector<ui_element_ptr>{
        make_label(e.as<std::string>("namespace"_key)),
        make_label(e.as<std::string>("name"_key)),
        make_label(e.as<std::string>("cpu"_key)),
        make_label(e.as<std::string>("mem"_key)),
    };
  });
  rebuild_top_table(top_nodes_, args, "nodes"_key, [&](const dynamic& e) {
    return std::vector<ui_element_ptr>{
        make_label(e.as<std::string>("name"_key)),
        make_label(e.as<std::string>("cpu"_key)),
        make_label(format_pct(e.as<float>("cpu_pct"_key))),
        make_label(e.as<std::string>("mem"_key)),
        make_label(format_pct(e.as<float>("mem_pct"_key))),
    };
  });

  const std::string error = common::str_of(args, "error"_key);
  if (!error.empty())
    common::set_status_text(top_status_label_, "kubectl top unavailable: " + error, false);
  else
    common::set_status_text(
        top_status_label_, std::to_string(pod_count) + " pods, " + std::to_string(node_count) + " nodes", true);
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void kubectl_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == pods_.panel.window_id() || id == deployments_.panel.window_id() || id == services_.panel.window_id() ||
       id == nodes_.panel.window_id() || id == logs_.window_id() || id == describe_.window_id() ||
       id == console_.window_id() || id == top_window_id_)) {
    logs_.remove_file();
    describe_.remove_file();
    emit("closed"_key);
    remove_objects_at(deployments_.panel.root_key());
    remove_objects_at(services_.panel.root_key());
    remove_objects_at(nodes_.panel.root_key());
    remove_objects_at(logs_.root_key());
    remove_objects_at(describe_.root_key());
    remove_objects_at(console_.root_key());
    remove_objects_at(top_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    for (auto* lw : {&pods_, &deployments_, &services_, &nodes_}) {
      if (lw->name_filter_id.id && id == lw->name_filter_id) {
        lw->name_filter = payload.as<std::string>("value"_key);
        apply_list_filter(*lw);
        return;
      }
      if (lw->ns_filter_id.id && id == lw->ns_filter_id) {
        lw->ns_filter = payload.as<std::string>("value"_key);
        apply_list_filter(*lw);
        return;
      }
      if (lw->phase_combo_id.id && id == lw->phase_combo_id) {
        lw->phase_filter = payload.as<int32_t>("value"_key);
        apply_list_filter(*lw);
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

  if (event == "clicked"_key)
    dispatch_click(id);
}

void kubectl_frontend::run_row_action(const resource& r, const std::string& action) {
  const std::string qualified = r.ns.empty() ? r.name : r.ns + "/" + r.name;

  if (action == "logs") {
    open_logs_name_ = r.name;
    open_logs_ns_ = r.ns;
    logs_.set_title(qualified);
    emit_logs_request();
    return;
  }
  if (action == "describe") {
    open_describe_name_ = r.name;
    open_describe_ns_ = r.ns;
    open_describe_kind_ = r.scope;
    describe_.set_title(r.scope + ": " + qualified);
    emit_describe_request();
    return;
  }

  const key_t event_name = r.scope == "pod" ? "pod_action_requested"_key
      : r.scope == "deployment"             ? "deployment_action_requested"_key
      : r.scope == "service"                ? "service_action_requested"_key
                                            : "node_action_requested"_key;
  auto fire = [this, event_name, r, action] {
    dynamic p;
    p["name"_key] = r.name;
    if (!r.ns.empty())
      p["namespace"_key] = r.ns;
    p["action"_key] = action;
    emit(event_name, std::move(p));
  };

  const bool destructive = action == "delete" || action == "drain";
  if (!destructive) {
    fire();
    return;
  }

  std::string message;
  if (action == "drain")
    message = "Drain node '" + r.name + "'? Its running pods will be evicted.";
  else
    message = std::string{"Delete "} + noun_for(r.scope) + " '" + r.name + "'" +
        (r.ns.empty() ? std::string{} : " in namespace '" + r.ns + "'") + "?";
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
