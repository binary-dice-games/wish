// MIT License © 2025 Binary Dice Games
/// @file top.cpp
/// @brief Implementation of the Top form.
#include "top.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/ui_importer.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

// Number of samples kept in the rolling CPU%/memory% history; also doubles
// as the fixed X-axis width for both plots (see on_init()).
constexpr size_t kMaxHistory = 60;

// ImPlotAxisFlags_NoTickLabels: the X axis is a rolling sample index, not a
// meaningful timestamp, so its numeric labels would just be noise.
constexpr int32_t kHideXTickLabels = 8;

std::string format_percent(float pct) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << pct << "%";
  return oss.str();
}

std::string format_bytes(float bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = bytes;
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << value << " " << kUnits[unit];
  return oss.str();
}

// The 6 selectable priority levels offered by each row's Priority submenu,
// on the Linux nice-value scale (-20 highest .. 19 lowest) on every
// platform -- process_control_win.cpp's nice_to_priority_class() (and
// process_info_win.cpp's reverse priority_class_to_nice()) bucket to/from
// these same 6 values so a Windows client shows/accepts the identical menu.
struct priority_level_def {
  const char* label;
  int32_t nice;
};
constexpr priority_level_def kPriorityLevels[] = {
    {"Low", 19},
    {"Below Normal", 10},
    {"Normal", 0},
    {"Above Normal", -5},
    {"High", -10},
    {"Realtime", -20},
};
constexpr size_t kPriorityLevelCount = std::size(kPriorityLevels);

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// "proc_table"'s "flags": "Resizable|RowBg|Borders|Sortable" (the first
// three match the "tbl_catalog" example in examples/demo/main.cpp;
// Sortable makes column headers clickable -- see the "sorted" event handling
// in on_event()/resort_rows()). Each TableColumn's "flags" is "WidthFixed",
// except "col_cpu" which also ORs in "DefaultSort|PreferSortDescending" so
// the column-header UI's initial sort indicator matches sort_column_id_/
// sort_ascending_'s own defaults below.
// "cores" is given an explicit empty "children" object -- even though empty
// -- so the importer allocates a private children map for this instance
// instead of sharing the Element base prototype's default (see nano.cpp's
// tab_bar for the same technique). Rows in "proc_table" are added, updated,
// and removed at runtime by update_snapshot(), same as nano's tabs.

static constexpr const char* kLayout = R"({
  "type": "Window",
  "title": "Top",
  "width": 900, "height": 700,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "summary": {
          "type": "HorizontalLayout",
          "spacing": 16,
          "children": {
            "cpu_label": { "type": "Label", "text": "CPU: --" },
            "mem_label": { "type": "Label", "text": "Mem: --" }
          }
        },
        "cores": { "type": "HorizontalLayout", "spacing": 6, "children": {} },
        "cpu_plot": {
          "type": "Plot", "title": "CPU % History", "height": 160,
          "y_label": "%",
          "children": {
            "cpu_series": { "type": "PlotShaded", "label": "CPU %" }
          }
        },
        "mem_plot": {
          "type": "Plot", "title": "Memory % History", "height": 160,
          "y_label": "%",
          "children": {
            "mem_series": { "type": "PlotLine", "label": "Memory %" }
          }
        },
        "proc_table": {
          "type": "Table", "id": "##proc_table", "columns": 6,
          "flags": "Resizable|RowBg|Borders|Sortable", "headers": true,
          "children": {
            "col_pid":   { "type": "TableColumn", "label": "PID",     "flags": "WidthFixed", "init_width": 70,  "column_id": 0 },
            "col_name":  { "type": "TableColumn", "label": "Name",    "flags": "WidthFixed", "init_width": 140, "column_id": 1 },
            "col_state": { "type": "TableColumn", "label": "State",   "flags": "WidthFixed", "init_width": 60,  "column_id": 2 },
            "col_cpu":   { "type": "TableColumn", "label": "CPU %",   "flags": "WidthFixed|DefaultSort|PreferSortDescending", "init_width": 100, "column_id": 3 },
            "col_mem":   { "type": "TableColumn", "label": "Memory",  "flags": "WidthFixed", "init_width": 100, "column_id": 4 },
            "col_cmd":   { "type": "TableColumn", "label": "Command", "column_id": 5 }
          }
        },
        "status_label": { "type": "Label", "text": "" }
      }
    }
  }
})";

// Confirm-kill dialog -- mirrors mc's overwrite-confirmation
// dialog: a small internal Window merged as its own top-level object,
// closed via the __request_close__/closed handshake (see form.hpp's
// request_close_at()).
static constexpr const char* kConfirmKillLayout = R"({
  "type": "Window", "title": "Confirm Kill", "modal": true,
  "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "message": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_yes": { "type": "Button", "label": "Kill", "height": 32 },
      "btn_no": { "type": "Button", "label": "Cancel", "height": 32 }
    } }
  }
})";

// Properties (extended info) dialog -- fixed shape, populated with
// "Loading..." at show-time and filled in once do_report_process_details()
// delivers the client's response (see that method's doc comment).
static constexpr const char* kPropertiesLayout = R"({
  "type": "Window", "title": "Process Properties", "modal": true,
  "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "width": 420,
  "children": {
    "grid": {
      "type": "VerticalLayout",
      "children": {
        "pid_row": { "type": "Label", "text": "" },
        "ppid_row": { "type": "Label", "text": "" },
        "user_row": { "type": "Label", "text": "" },
        "threads_row": { "type": "Label", "text": "" },
        "start_row": { "type": "Label", "text": "" },
        "priority_row": { "type": "Label", "text": "" },
        "affinity_row": { "type": "Label", "text": "" },
        "exe_row": { "type": "Label", "text": "", "wrap": true },
        "cwd_row": { "type": "Label", "text": "", "wrap": true },
        "cmdline_row": { "type": "Label", "text": "", "wrap": true }
      }
    },
    "sep": { "type": "Separator" },
    "close_row": { "type": "HorizontalLayout", "children": {
      "btn_close": { "type": "Button", "label": "Close", "height": 32 }
    } }
  }
})";

// ── top ─────────────────────────────────────────────────────────

top::top(dynamic&& base) : form(std::move(base)) {}

void top::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__top_");

  auto tree = import_json(kLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Top"};

  // put_object() files each element under the current request's group (see
  // rmi::context::current_group) so they're cleaned up together with the
  // rest of this form when relayed through rmi::bridge.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.summary.cpu_label", [&](const auto& e) { cpu_summary_label_ = e; });
  tree.with("vbox.summary.mem_label", [&](const auto& e) { mem_summary_label_ = e; });
  tree.with("vbox.cores", [&](const auto& e) { cores_container_ = e; });
  tree.with("vbox.cpu_plot.cpu_series", [&](const auto& e) { cpu_plot_series_ = e; });
  tree.with("vbox.mem_plot.mem_series", [&](const auto& e) { mem_plot_series_ = e; });
  tree.with("vbox.proc_table", [&](const auto& e) {
    proc_table_ = e;
    proc_table_id_ = e->template as<key_t>("__wish_id"_key);
  });
  tree.with("vbox.status_label", [&](const auto& e) { status_label_ = e; });

  // Fix both axes so the graphs read as stable percentage gauges instead of
  // auto-fitting (which otherwise locks onto whatever tiny range existed on
  // the very first render, before any real data arrived) -- Y is always
  // 0..100%; X is a rolling sample-index window with its numeric labels
  // hidden, since "sample 37" isn't meaningful to a user.
  auto fix_axes = [](const auto& e) {
    e["x_min"_key] = 0.0f;
    e["x_max"_key] = static_cast<float>(kMaxHistory - 1);
    e["y_min"_key] = 0.0f;
    e["y_max"_key] = 100.0f;
    e["x_flags"_key] = kHideXTickLabels;
  };
  tree.with("vbox.cpu_plot", fix_axes);
  tree.with("vbox.mem_plot", fix_axes);

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

// ── update_snapshot ───────────────────────────────────────────────────────────

dynamic top::do_update_snapshot(const dynamic& args) {
  float cpu_pct = args.as<float>("cpu_percent"_key);
  if (cpu_summary_label_)
    cpu_summary_label_["text"_key] = "CPU: " + format_percent(cpu_pct);
  push_history(cpu_history_, cpu_pct);

  float mem_total = args.as<float>("mem_total_bytes"_key);
  float mem_used = args.as<float>("mem_used_bytes"_key);
  float mem_pct = mem_total > 0 ? 100.0f * mem_used / mem_total : 0.0f;
  if (mem_summary_label_) {
    mem_summary_label_["text"_key] =
        "Mem: " + format_percent(mem_pct) + " (" + format_bytes(mem_used) + " / " + format_bytes(mem_total) + ")";
  }
  push_history(mem_history_, mem_pct);

  // Both histories are pushed exactly once per call, so they always share
  // one length -- an index axis (0, 1, 2, ...) is all either series needs.
  update_history_xs(cpu_history_.size());
  if (cpu_plot_series_) {
    cpu_plot_series_["xs"_key] = history_xs_;
    cpu_plot_series_["ys"_key] = cpu_history_;
  }
  if (mem_plot_series_) {
    mem_plot_series_["xs"_key] = history_xs_;
    mem_plot_series_["ys"_key] = mem_history_;
  }

  if (auto* per_core = args.findField<std::vector<float>>("per_core_percent"_key)) {
    ensure_core_meters(per_core->size());
    for (size_t i = 0; i < core_bars_.size() && i < per_core->size(); ++i) {
      float pct = (*per_core)[i];
      core_bars_[i]["value"_key] = pct / 100.0f;
      core_bars_[i]["label"_key] = "Core " + std::to_string(i) + ": " + format_percent(pct);
    }
  }

  update_process_table(args);
  return dynamic{};
}

dynamic top::do_report_action_result(const dynamic& args) {
  int32_t pid = args.as<int32_t>("pid"_key);
  std::string action = args.as<std::string>("action"_key);
  bool success = args.as<bool>("success"_key);
  std::string error = args.as<std::string>("error"_key);

  std::string verb = action == "kill"            ? "Kill"
      : action == "pause"                        ? "Pause"
      : action == "resume"                        ? "Resume"
      : action == "set_priority"                  ? "Set priority for"
      : action == "set_affinity"                  ? "Set CPU affinity for"
                                                    : action;

  if (success)
    set_status(verb + " process " + std::to_string(pid) + " succeeded.");
  else
    set_status(verb + " process " + std::to_string(pid) + " failed: " + (error.empty() ? "unknown error" : error));

  return dynamic{};
}

dynamic top::do_report_process_details(const dynamic& args) {
  int32_t pid = args.as<int32_t>("pid"_key);
  // Stale response guard: the Properties dialog may have been closed, or
  // reopened for a different pid, before this RMI call arrived.
  if (properties_root_key_.empty() || pid != properties_dialog_pid_)
    return dynamic{};

  bool found = args.as<bool>("found"_key);
  if (!found) {
    std::string error = args.as<std::string>("error"_key);
    if (properties_ppid_label_)
      properties_ppid_label_["text"_key] = error.empty() ? std::string{"Process no longer exists."} : error;
    return dynamic{};
  }

  int32_t ppid = args.as<int32_t>("ppid"_key);
  std::string user = args.as<std::string>("user"_key);
  int32_t threads = args.as<int32_t>("thread_count"_key);
  std::string start_time = args.as<std::string>("start_time"_key);
  std::string exe_path = args.as<std::string>("exe_path"_key);
  std::string cwd = args.as<std::string>("cwd"_key);
  std::string cmdline = args.as<std::string>("cmdline"_key);
  int32_t nice = args.as<int32_t>("nice"_key);

  std::vector<int32_t> affinity_cores;
  if (auto* af = args.findField<std::vector<int32_t>>("affinity_cores"_key))
    affinity_cores = *af;

  if (properties_ppid_label_)
    properties_ppid_label_["text"_key] = "Parent PID: " + std::to_string(ppid);
  if (properties_user_label_)
    properties_user_label_["text"_key] = "User: " + (user.empty() ? std::string{"(unknown)"} : user);
  if (properties_threads_label_)
    properties_threads_label_["text"_key] = "Threads: " + std::to_string(threads);
  if (properties_start_label_)
    properties_start_label_["text"_key] = "Started: " + (start_time.empty() ? std::string{"(unknown)"} : start_time);
  if (properties_priority_label_) {
    std::string level_label = "Custom";
    for (auto& level : kPriorityLevels)
      if (level.nice == nice)
        level_label = level.label;
    properties_priority_label_["text"_key] = "Priority: " + level_label + " (nice " + std::to_string(nice) + ")";
  }
  if (properties_affinity_label_) {
    std::ostringstream oss;
    oss << "CPU Affinity: ";
    if (affinity_cores.empty())
      oss << "(unknown)";
    for (size_t i = 0; i < affinity_cores.size(); ++i) {
      if (i > 0)
        oss << ", ";
      oss << affinity_cores[i];
    }
    properties_affinity_label_["text"_key] = oss.str();
  }
  if (properties_exe_label_)
    properties_exe_label_["text"_key] = "Executable: " + (exe_path.empty() ? std::string{"(unknown)"} : exe_path);
  if (properties_cwd_label_)
    properties_cwd_label_["text"_key] = "Working Dir: " + (cwd.empty() ? std::string{"(unavailable)"} : cwd);
  if (properties_cmdline_label_)
    properties_cmdline_label_["text"_key] = "Command: " + cmdline;

  return dynamic{};
}

void top::ensure_core_meters(size_t core_count) {
  if (!core_bars_.empty() || core_count == 0 || !cores_container_)
    return;
  auto* children_p = cores_container_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (size_t i = 0; i < core_count; ++i) {
    ui_element_ptr bar{dynamic::instantiate("wish"_key, "ProgressBar"_key)};
    bar["label"_key] = "Core " + std::to_string(i) + ": --";
    bar["width"_key] = 90.0f;
    bar["order"_key] = static_cast<int32_t>(i);

    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, bar);
    bar["__wish_id"_key] = id;

    (*children)[next_child_key_++] = dynamic_ptr{bar};
    core_bars_.push_back(bar);
  }
  cores_container_->refresh_children_order();
}

void top::push_history(std::vector<float>& history, float value) {
  history.push_back(value);
  if (history.size() > kMaxHistory)
    history.erase(history.begin());
}

void top::update_history_xs(size_t count) {
  if (history_xs_.size() == count)
    return;
  history_xs_.resize(count);
  for (size_t i = 0; i < count; ++i)
    history_xs_[i] = static_cast<float>(i);
}

void top::set_status(const std::string& text) {
  if (status_label_)
    status_label_["text"_key] = text;
}

// ── Row context menu ─────────────────────────────────────────────────────────

ui_element_ptr top::build_row_context_menu(row_entry& entry, int pid, const std::string& state, int32_t nice) {
  auto assign_id = [&](ui_element_ptr& el) {
    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, el);
    el["__wish_id"_key] = id;
    return id;
  };

  ui_element_ptr menu{dynamic::instantiate("wish"_key, "ContextMenu"_key)};
  assign_id(menu);

  ui_element_ptr properties{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  properties["label"_key] = std::string{"Properties..."};
  key_t properties_id = assign_id(properties);
  entry.properties_id = properties_id;
  action_item_targets_[properties_id] = row_action_target{pid, row_action_kind::properties, 0};

  ui_element_ptr sep1{dynamic::instantiate("wish"_key, "Separator"_key)};
  assign_id(sep1);

  ui_element_ptr pause_resume{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  pause_resume["label"_key] = std::string{state == "T" ? "Resume" : "Pause"};
  key_t pause_resume_id = assign_id(pause_resume);
  action_item_targets_[pause_resume_id] = row_action_target{pid, row_action_kind::pause_or_resume, 0};
  entry.pause_resume_item = pause_resume;

  ui_element_ptr kill{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  kill["label"_key] = std::string{"Kill Process"};
  key_t kill_id = assign_id(kill);
  entry.kill_id = kill_id;
  action_item_targets_[kill_id] = row_action_target{pid, row_action_kind::kill, 0};

  ui_element_ptr sep2{dynamic::instantiate("wish"_key, "Separator"_key)};
  assign_id(sep2);

  ui_element_ptr priority_menu{dynamic::instantiate("wish"_key, "Menu"_key)};
  priority_menu["label"_key] = std::string{"Priority"};
  assign_id(priority_menu);

  entry.priority_items.clear();
  entry.priority_items.reserve(kPriorityLevelCount);
  auto priority_children = dynamic_ptr{key_t{0U}, {}};
  size_t pk = 0;
  for (auto& level : kPriorityLevels) {
    ui_element_ptr item{dynamic::instantiate("wish"_key, "MenuItem"_key)};
    item["label"_key] = std::string{level.label};
    item["checked"_key] = nice == level.nice;
    key_t item_id = assign_id(item);
    action_item_targets_[item_id] = row_action_target{pid, row_action_kind::priority, level.nice};
    (*priority_children)[pk++] = dynamic_ptr{item};
    entry.priority_items.push_back(item);
  }
  priority_menu["children"_key] = priority_children;
  priority_menu->refresh_children_order();

  ui_element_ptr affinity{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  affinity["label"_key] = std::string{"Set CPU Affinity..."};
  key_t affinity_id = assign_id(affinity);
  entry.affinity_id = affinity_id;
  action_item_targets_[affinity_id] = row_action_target{pid, row_action_kind::affinity_dialog, 0};

  auto menu_children = dynamic_ptr{key_t{0U}, {}};
  size_t mk = 0;
  (*menu_children)[mk++] = dynamic_ptr{properties};
  (*menu_children)[mk++] = dynamic_ptr{sep1};
  (*menu_children)[mk++] = dynamic_ptr{pause_resume};
  (*menu_children)[mk++] = dynamic_ptr{kill};
  (*menu_children)[mk++] = dynamic_ptr{sep2};
  (*menu_children)[mk++] = dynamic_ptr{priority_menu};
  (*menu_children)[mk++] = dynamic_ptr{affinity};
  menu["children"_key] = menu_children;
  menu->refresh_children_order();

  return menu;
}

void top::update_row_context_menu(row_entry& entry, const std::string& state, int32_t nice) {
  if (entry.pause_resume_item)
    entry.pause_resume_item["label"_key] = std::string{state == "T" ? "Resume" : "Pause"};
  for (size_t i = 0; i < entry.priority_items.size() && i < kPriorityLevelCount; ++i)
    if (entry.priority_items[i])
      entry.priority_items[i]["checked"_key] = nice == kPriorityLevels[i].nice;
}

// ── Confirm-kill dialog ───────────────────────────────────────────────────────

void top::show_confirm_kill(int pid) {
  if (!confirm_root_key_.empty())
    remove_confirm_objects();

  auto row_it = pid_to_row_.find(pid);
  std::string name = row_it != pid_to_row_.end() ? row_it->second.name : std::string{};
  confirm_kill_pid_ = pid;

  auto tree = import_json(kConfirmKillLayout);
  std::string message = "Kill process " + std::to_string(pid) + (name.empty() ? std::string{} : " (" + name + ")") +
      "? This cannot be undone.";
  tree.with("message", [&](const auto& e) { e["text"_key] = message; });

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  confirm_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("buttons.btn_yes", [&](const auto& e) { confirm_yes_id_ = wish_id_of(e); });
  tree.with("buttons.btn_no", [&](const auto& e) { confirm_no_id_ = wish_id_of(e); });

  auto lock = context_wlock{*sync_ctx_};
  context& s = *lock;
  for (int i = 0;; ++i) {
    std::string candidate = "__top_confirm_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      confirm_root_key_ = candidate;
      break;
    }
  }
  s.ui_objects.merge(std::move(tree), confirm_root_key_);
  auto it = s.ui_objects.find(confirm_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{confirm_root_key_}] = it->second;
    (*it->second)["__path__"_key] = confirm_root_key_;
    s.top_level_handlers[key_t{confirm_root_key_}] = this;
  }
}

void top::request_close_confirm() {
  request_close_at(confirm_root_key_);
}

void top::remove_confirm_objects() {
  remove_objects_at(confirm_root_key_);
  confirm_root_key_.clear();
}

// ── Set CPU Affinity dialog ───────────────────────────────────────────────────

void top::show_affinity_dialog(int pid) {
  if (!affinity_root_key_.empty())
    remove_affinity_objects();

  auto row_it = pid_to_row_.find(pid);
  if (row_it == pid_to_row_.end())
    return;

  affinity_dialog_pid_ = pid;
  const auto& current_cores = row_it->second.affinity_cores;
  auto is_set = [&](int32_t core) {
    return std::find(current_cores.begin(), current_cores.end(), core) != current_cores.end();
  };

  size_t core_count = core_bars_.size();

  // Built as generated JSON (rather than object-by-object like row cells)
  // since Window/Checkbox get their usual import_json()+ui_objects.merge()
  // top-level-object treatment for free this way, and the checkbox count is
  // the only part that actually varies per machine.
  std::ostringstream checkboxes_json;
  for (size_t i = 0; i < core_count; ++i) {
    if (i > 0)
      checkboxes_json << ",";
    checkboxes_json << "\"core" << i << "\": { \"type\": \"Checkbox\", \"label\": \"Core " << i
                     << "\", \"value\": " << (is_set(static_cast<int32_t>(i)) ? "true" : "false") << " }";
  }

  std::ostringstream layout;
  layout << R"({
    "type": "Window", "title": "Set CPU Affinity - PID )" << pid << R"(", "modal": true,
    "flags": "NoResize|NoCollapse|AlwaysAutoResize",
    "children": {
      "cores": { "type": "VerticalLayout", "children": { )"
         << checkboxes_json.str() << R"( } },
      "sep": { "type": "Separator" },
      "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
        "btn_apply": { "type": "Button", "label": "Apply", "height": 32 },
        "btn_cancel": { "type": "Button", "label": "Cancel", "height": 32 }
      } }
    }
  })";

  auto tree = import_json(layout.str());
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  affinity_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("buttons.btn_apply", [&](const auto& e) { affinity_apply_id_ = wish_id_of(e); });
  tree.with("buttons.btn_cancel", [&](const auto& e) { affinity_cancel_id_ = wish_id_of(e); });

  affinity_checkboxes_.clear();
  for (size_t i = 0; i < core_count; ++i) {
    tree.with("cores.core" + std::to_string(i), [&](const auto& e) {
      affinity_checkboxes_.push_back({e, static_cast<int32_t>(i)});
    });
  }

  auto lock = context_wlock{*sync_ctx_};
  context& s = *lock;
  for (int i = 0;; ++i) {
    std::string candidate = "__top_affinity_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      affinity_root_key_ = candidate;
      break;
    }
  }
  s.ui_objects.merge(std::move(tree), affinity_root_key_);
  auto it = s.ui_objects.find(affinity_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{affinity_root_key_}] = it->second;
    (*it->second)["__path__"_key] = affinity_root_key_;
    s.top_level_handlers[key_t{affinity_root_key_}] = this;
  }
}

void top::request_close_affinity() {
  request_close_at(affinity_root_key_);
}

void top::remove_affinity_objects() {
  remove_objects_at(affinity_root_key_);
  affinity_root_key_.clear();
  affinity_checkboxes_.clear();
}

// ── Properties (extended info) dialog ─────────────────────────────────────────

void top::show_properties_dialog(int pid) {
  if (!properties_root_key_.empty())
    remove_properties_objects();

  properties_dialog_pid_ = pid;

  auto tree = import_json(kPropertiesLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  properties_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  (*tree[""])["title"_key] = "Process Properties - PID " + std::to_string(pid);
  tree.with("close_row.btn_close", [&](const auto& e) { properties_close_id_ = wish_id_of(e); });
  tree.with("grid.pid_row", [&](const auto& e) { properties_pid_label_ = e; });
  tree.with("grid.ppid_row", [&](const auto& e) { properties_ppid_label_ = e; });
  tree.with("grid.user_row", [&](const auto& e) { properties_user_label_ = e; });
  tree.with("grid.threads_row", [&](const auto& e) { properties_threads_label_ = e; });
  tree.with("grid.start_row", [&](const auto& e) { properties_start_label_ = e; });
  tree.with("grid.priority_row", [&](const auto& e) { properties_priority_label_ = e; });
  tree.with("grid.affinity_row", [&](const auto& e) { properties_affinity_label_ = e; });
  tree.with("grid.exe_row", [&](const auto& e) { properties_exe_label_ = e; });
  tree.with("grid.cwd_row", [&](const auto& e) { properties_cwd_label_ = e; });
  tree.with("grid.cmdline_row", [&](const auto& e) { properties_cmdline_label_ = e; });

  if (properties_pid_label_)
    properties_pid_label_["text"_key] = "PID: " + std::to_string(pid);
  if (properties_ppid_label_)
    properties_ppid_label_["text"_key] = std::string{"Loading..."};

  {
    auto lock = context_wlock{*sync_ctx_};
    context& s = *lock;
    for (int i = 0;; ++i) {
      std::string candidate = "__top_properties_" + std::to_string(i);
      if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
        properties_root_key_ = candidate;
        break;
      }
    }
    s.ui_objects.merge(std::move(tree), properties_root_key_);
    auto it = s.ui_objects.find(properties_root_key_);
    if (it != s.ui_objects.end()) {
      s.top_level_objects[key_t{properties_root_key_}] = it->second;
      (*it->second)["__path__"_key] = properties_root_key_;
      s.top_level_handlers[key_t{properties_root_key_}] = this;
    }
  }

  // emit() acquires the session wlock itself when called outside dispatch
  // (see form::emit()) -- must run after the block above releases `lock`,
  // not while it's still held, or this deadlocks on the same non-recursive
  // mutex.
  dynamic req;
  req["pid"_key] = static_cast<int32_t>(pid);
  emit("on_process_details_requested"_key, std::move(req));
}

void top::request_close_properties() {
  request_close_at(properties_root_key_);
}

void top::remove_properties_objects() {
  remove_objects_at(properties_root_key_);
  properties_root_key_.clear();
  properties_pid_label_.reset();
  properties_ppid_label_.reset();
  properties_user_label_.reset();
  properties_threads_label_.reset();
  properties_start_label_.reset();
  properties_priority_label_.reset();
  properties_affinity_label_.reset();
  properties_exe_label_.reset();
  properties_cwd_label_.reset();
  properties_cmdline_label_.reset();
}

// ── Process table reconciliation ─────────────────────────────────────────────

void top::update_process_table(const dynamic& args) {
  if (!proc_table_)
    return;
  auto* children_p = proc_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::unordered_map<int, bool> seen;

  auto* procs_f = args.findField<dynamic_ptr>("processes"_key);
  if (procs_f && *procs_f) {
    (*procs_f)->forEach([&](key_t, const field& f) {
      if (!f.is<dynamic_ptr>())
        return;
      auto entry_ptr = f.as<dynamic_ptr>();
      if (!entry_ptr)
        return;
      auto& e = *entry_ptr;

      int pid = e.as<int32_t>("pid"_key);
      std::string name = e.as<std::string>("name"_key);
      std::string command = e.as<std::string>("command"_key);
      std::string state = e.as<std::string>("state"_key);
      float cpu_percent = e.as<float>("cpu_percent"_key);
      float mem_rss = e.as<float>("mem_rss_bytes"_key);
      int32_t nice = e.as<int32_t>("nice_value"_key);
      std::vector<int32_t> affinity_cores;
      if (auto* af = e.findField<std::vector<int32_t>>("affinity_cores"_key))
        affinity_cores = *af;

      seen[pid] = true;
      auto it = pid_to_row_.find(pid);
      if (it == pid_to_row_.end()) {
        row_entry entry;
        entry.row = ui_element_ptr{dynamic::instantiate("wish"_key, "TableRow"_key)};

        entry.pid_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
        entry.pid_label["text"_key] = std::to_string(pid);

        entry.name_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
        entry.name_label["text"_key] = name;

        entry.state_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
        entry.state_label["text"_key] = state;

        entry.cpu_bar = ui_element_ptr{dynamic::instantiate("wish"_key, "ProgressBar"_key)};
        entry.cpu_bar["value"_key] = cpu_percent / 100.0f;
        entry.cpu_bar["label"_key] = format_percent(cpu_percent);

        entry.mem_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
        entry.mem_label["text"_key] = format_bytes(mem_rss);

        entry.command_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
        entry.command_label["text"_key] = command;

        auto assign_id = [&](ui_element_ptr& el) {
          key_t id2 = rmi::shared::generate_id();
          ctx().put_object(id2, el);
          el["__wish_id"_key] = id2;
        };
        assign_id(entry.row);
        assign_id(entry.pid_label);
        assign_id(entry.name_label);
        assign_id(entry.state_label);
        assign_id(entry.cpu_bar);
        assign_id(entry.mem_label);
        assign_id(entry.command_label);

        entry.nice = nice;
        entry.affinity_cores = affinity_cores;
        ui_element_ptr context_menu = build_row_context_menu(entry, pid, state, nice);

        auto row_children = dynamic_ptr{key_t{0U}, {}};
        size_t k = 0;
        (*row_children)[k++] = dynamic_ptr{entry.pid_label};
        (*row_children)[k++] = dynamic_ptr{entry.name_label};
        (*row_children)[k++] = dynamic_ptr{entry.state_label};
        (*row_children)[k++] = dynamic_ptr{entry.cpu_bar};
        (*row_children)[k++] = dynamic_ptr{entry.mem_label};
        (*row_children)[k++] = dynamic_ptr{entry.command_label};
        (*row_children)[k++] = dynamic_ptr{context_menu};
        entry.row["children"_key] = row_children;
        entry.row->refresh_children_order();

        entry.child_key = next_child_key_++;
        (*children)[entry.child_key] = dynamic_ptr{entry.row};
        entry.name = name;
        entry.state = state;
        entry.command = command;
        entry.cpu_percent = cpu_percent;
        entry.mem_rss_bytes = mem_rss;

        pid_to_row_.emplace(pid, std::move(entry));
      } else {
        auto& entry = it->second;
        entry.name_label["text"_key] = name;
        entry.state_label["text"_key] = state;
        entry.cpu_bar["value"_key] = cpu_percent / 100.0f;
        entry.cpu_bar["label"_key] = format_percent(cpu_percent);
        entry.mem_label["text"_key] = format_bytes(mem_rss);
        entry.command_label["text"_key] = command;
        update_row_context_menu(entry, state, nice);
        entry.name = name;
        entry.state = state;
        entry.command = command;
        entry.cpu_percent = cpu_percent;
        entry.mem_rss_bytes = mem_rss;
        entry.nice = nice;
        entry.affinity_cores = affinity_cores;
      }
    });
  }

  // Remove rows for pids that were not present in this snapshot.
  for (auto it = pid_to_row_.begin(); it != pid_to_row_.end();) {
    if (!seen.count(it->first)) {
      auto& entry = it->second;
      action_item_targets_.erase(entry.kill_id);
      action_item_targets_.erase(entry.properties_id);
      action_item_targets_.erase(entry.affinity_id);
      if (entry.pause_resume_item)
        action_item_targets_.erase(entry.pause_resume_item->as<key_t>("__wish_id"_key));
      for (auto& item : entry.priority_items)
        if (item)
          action_item_targets_.erase(item->as<key_t>("__wish_id"_key));
      children->erase(it->second.child_key);
      it = pid_to_row_.erase(it);
    } else {
      ++it;
    }
  }

  resort_rows();
}

void top::resort_rows() {
  if (!proc_table_ || pid_to_row_.empty())
    return;

  std::vector<std::pair<int, row_entry*>> rows;
  rows.reserve(pid_to_row_.size());
  for (auto& [pid, entry] : pid_to_row_)
    rows.push_back({pid, &entry});

  // Comparator always expressed in ascending terms; descending just swaps
  // the operand order, matching column_id assignments in kLayout above
  // (0=PID, 1=Name, 2=State, 3=CPU %, 4=Memory, 5=Command).
  auto ascending_less = [&](const std::pair<int, row_entry*>& a, const std::pair<int, row_entry*>& b) {
    switch (sort_column_id_) {
      case 0:
        return a.first < b.first;
      case 1:
        return a.second->name < b.second->name;
      case 2:
        return a.second->state < b.second->state;
      case 4:
        return a.second->mem_rss_bytes < b.second->mem_rss_bytes;
      case 5:
        return a.second->command < b.second->command;
      case 3:
      default:
        return a.second->cpu_percent < b.second->cpu_percent;
    }
  };
  std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
    return sort_ascending_ ? ascending_less(a, b) : ascending_less(b, a);
  });

  for (size_t i = 0; i < rows.size(); ++i)
    rows[i].second->row["order"_key] = static_cast<int32_t>(i);

  proc_table_->refresh_children_order();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void top::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == proc_table_id_ && event == "sorted"_key) {
    sort_column_id_ = payload.as<int32_t>("column_id"_key);
    sort_ascending_ = payload.as<bool>("ascending"_key);
    resort_rows();
    return;
  }

  if (event == "clicked"_key) {
    auto target_it = action_item_targets_.find(id);
    if (target_it != action_item_targets_.end()) {
      const row_action_target target = target_it->second;
      switch (target.kind) {
        case row_action_kind::kill:
          show_confirm_kill(target.pid);
          return;
        case row_action_kind::pause_or_resume: {
          auto row_it = pid_to_row_.find(target.pid);
          bool is_paused = row_it != pid_to_row_.end() && row_it->second.state == "T";
          dynamic req;
          req["pid"_key] = static_cast<int32_t>(target.pid);
          req["action"_key] = std::string{is_paused ? "resume" : "pause"};
          emit("on_process_action_requested"_key, std::move(req));
          return;
        }
        case row_action_kind::properties:
          show_properties_dialog(target.pid);
          return;
        case row_action_kind::affinity_dialog:
          show_affinity_dialog(target.pid);
          return;
        case row_action_kind::priority: {
          dynamic req;
          req["pid"_key] = static_cast<int32_t>(target.pid);
          req["action"_key] = std::string{"set_priority"};
          req["nice"_key] = target.nice;
          emit("on_process_action_requested"_key, std::move(req));
          return;
        }
      }
    }
  }

  if (!confirm_root_key_.empty()) {
    if (id == confirm_window_id_ && event == "closed"_key) {
      remove_confirm_objects();
      confirm_kill_pid_ = 0;
      return;
    }
    if (id == confirm_yes_id_ && event == "clicked"_key) {
      dynamic req;
      req["pid"_key] = static_cast<int32_t>(confirm_kill_pid_);
      req["action"_key] = std::string{"kill"};
      emit("on_process_action_requested"_key, std::move(req));
      request_close_confirm();
      return;
    }
    if (id == confirm_no_id_ && event == "clicked"_key) {
      request_close_confirm();
      return;
    }
  }

  if (!affinity_root_key_.empty()) {
    if (id == affinity_window_id_ && event == "closed"_key) {
      remove_affinity_objects();
      affinity_dialog_pid_ = 0;
      return;
    }
    if (id == affinity_cancel_id_ && event == "clicked"_key) {
      request_close_affinity();
      return;
    }
    if (id == affinity_apply_id_ && event == "clicked"_key) {
      std::vector<int32_t> cores;
      for (auto& [checkbox, core] : affinity_checkboxes_)
        if (checkbox && checkbox->as<bool>("value"_key))
          cores.push_back(core);
      if (cores.empty()) {
        set_status("Set CPU affinity failed: at least one core must stay checked.");
      } else {
        dynamic req;
        req["pid"_key] = static_cast<int32_t>(affinity_dialog_pid_);
        req["action"_key] = std::string{"set_affinity"};
        req["cores"_key] = cores;
        emit("on_process_action_requested"_key, std::move(req));
      }
      request_close_affinity();
      return;
    }
  }

  if (!properties_root_key_.empty()) {
    if (id == properties_window_id_ && event == "closed"_key) {
      remove_properties_objects();
      properties_dialog_pid_ = 0;
      return;
    }
    if (id == properties_close_id_ && event == "clicked"_key) {
      request_close_properties();
      return;
    }
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_top() {
  auto proto = dynamic_ptr{"Top"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Top"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addMethod(
      "update_snapshot"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<top&>(self).do_update_snapshot(args);
      }});

  proto->addMethod(
      "report_action_result"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<top&>(self).do_report_action_result(args);
      }});

  proto->addMethod(
      "report_process_details"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<top&>(self).do_report_process_details(args);
      }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Top"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Top/htop-style system monitor: CPU and memory history graphs, "
                        "per-core meters, and a process table. Right-click a row to kill, "
                        "pause/resume, change priority or CPU affinity, or view extended "
                        "properties for that process. The client owns all sampling and "
                        "process-control work and calls update_snapshot() periodically; the "
                        "server only renders whatever it was last given and relays context-menu "
                        "actions back via 'on_process_action_requested'/'on_process_details_requested'. "
                        "Listen for the 'closed' event to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<top>("wish"_key, "Top"_key));
}

} // namespace bdg::wish
