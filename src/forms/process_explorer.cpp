// MIT License © 2025 Binary Dice Games
/// @file process_explorer.cpp
/// @brief Implementation of the ProcessExplorer form.
#include "process_explorer.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui_importer.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

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

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// ImGuiTableFlags_Resizable=1, RowBg=64, Borders=1920 -> 1985 (matches the
// "tbl_catalog" example in examples/demo/main.cpp).
// "cores" is given an explicit empty "children" object -- even though empty
// -- so the importer allocates a private children map for this instance
// instead of sharing the Element base prototype's default (see notepad.cpp's
// tab_bar for the same technique). Rows in "proc_table" are added, updated,
// and removed at runtime by update_snapshot(), same as notepad's tabs.

static constexpr const char* kLayout = R"({
  "type": "Window",
  "title": "Process Explorer",
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
          "flags": 1985, "headers": true,
          "children": {
            "col_pid":   { "type": "TableColumn", "label": "PID",     "flags": 16, "init_width": 70 },
            "col_name":  { "type": "TableColumn", "label": "Name",    "flags": 16, "init_width": 140 },
            "col_state": { "type": "TableColumn", "label": "State",   "flags": 16, "init_width": 60 },
            "col_cpu":   { "type": "TableColumn", "label": "CPU %",   "flags": 16, "init_width": 100 },
            "col_mem":   { "type": "TableColumn", "label": "Memory",  "flags": 16, "init_width": 100 },
            "col_cmd":   { "type": "TableColumn", "label": "Command" }
          }
        }
      }
    }
  }
})";

// ── process_explorer ─────────────────────────────────────────────────────────

process_explorer::process_explorer(dynamic&& base) : form(std::move(base)) {}

void process_explorer::on_init() {
  internal_root_key_ = "__procexp_" + std::to_string(reinterpret_cast<uintptr_t>(this));

  auto tree = import_json(kLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Process Explorer"};

  auto& objects = ctx().objects;
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    objects[id.id] = elem;
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.summary.cpu_label", [&](const auto& e) { cpu_summary_label_ = e; });
  tree.with("vbox.summary.mem_label", [&](const auto& e) { mem_summary_label_ = e; });
  tree.with("vbox.cores", [&](const auto& e) { cores_container_ = e; });
  tree.with("vbox.cpu_plot.cpu_series", [&](const auto& e) { cpu_plot_series_ = e; });
  tree.with("vbox.mem_plot.mem_series", [&](const auto& e) { mem_plot_series_ = e; });
  tree.with("vbox.proc_table", [&](const auto& e) { proc_table_ = e; });

  sess().objects.merge(std::move(tree), internal_root_key_);
}

// ── update_snapshot ───────────────────────────────────────────────────────────

dynamic process_explorer::do_update_snapshot(const dynamic& args) {
  float cpu_pct = args.as<float>("cpu_percent"_key);
  if (cpu_summary_label_)
    cpu_summary_label_["text"_key] = "CPU: " + format_percent(cpu_pct);
  push_history(cpu_history_, cpu_pct);
  if (cpu_plot_series_)
    cpu_plot_series_["ys"_key] = cpu_history_;

  float mem_total = args.as<float>("mem_total_bytes"_key);
  float mem_used = args.as<float>("mem_used_bytes"_key);
  float mem_pct = mem_total > 0 ? 100.0f * mem_used / mem_total : 0.0f;
  if (mem_summary_label_) {
    mem_summary_label_["text"_key] =
        "Mem: " + format_percent(mem_pct) + " (" + format_bytes(mem_used) + " / " + format_bytes(mem_total) + ")";
  }
  push_history(mem_history_, mem_pct);
  if (mem_plot_series_)
    mem_plot_series_["ys"_key] = mem_history_;

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

void process_explorer::ensure_core_meters(size_t core_count) {
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
    ctx().objects[id.id] = bar;
    bar["__wish_id"_key] = id;

    (*children)[next_child_key_++] = dynamic_ptr{bar};
    core_bars_.push_back(bar);
  }
  cores_container_->refresh_children_order();
}

void process_explorer::push_history(std::vector<float>& history, float value) {
  static constexpr size_t kMaxHistory = 60;
  history.push_back(value);
  if (history.size() > kMaxHistory)
    history.erase(history.begin());
}

void process_explorer::update_process_table(const dynamic& args) {
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
          ctx().objects[id2.id] = el;
          el["__wish_id"_key] = id2;
        };
        assign_id(entry.row);
        assign_id(entry.pid_label);
        assign_id(entry.name_label);
        assign_id(entry.state_label);
        assign_id(entry.cpu_bar);
        assign_id(entry.mem_label);
        assign_id(entry.command_label);

        auto row_children = dynamic_ptr{key_t{0U}, {}};
        size_t k = 0;
        (*row_children)[k++] = dynamic_ptr{entry.pid_label};
        (*row_children)[k++] = dynamic_ptr{entry.name_label};
        (*row_children)[k++] = dynamic_ptr{entry.state_label};
        (*row_children)[k++] = dynamic_ptr{entry.cpu_bar};
        (*row_children)[k++] = dynamic_ptr{entry.mem_label};
        (*row_children)[k++] = dynamic_ptr{entry.command_label};
        entry.row["children"_key] = row_children;
        entry.row->refresh_children_order();

        entry.child_key = next_child_key_++;
        (*children)[entry.child_key] = dynamic_ptr{entry.row};
        entry.cpu_percent = cpu_percent;

        pid_to_row_.emplace(pid, std::move(entry));
      } else {
        auto& entry = it->second;
        entry.name_label["text"_key] = name;
        entry.state_label["text"_key] = state;
        entry.cpu_bar["value"_key] = cpu_percent / 100.0f;
        entry.cpu_bar["label"_key] = format_percent(cpu_percent);
        entry.mem_label["text"_key] = format_bytes(mem_rss);
        entry.command_label["text"_key] = command;
        entry.cpu_percent = cpu_percent;
      }
    });
  }

  // Remove rows for pids that were not present in this snapshot.
  for (auto it = pid_to_row_.begin(); it != pid_to_row_.end();) {
    if (!seen.count(it->first)) {
      children->erase(it->second.child_key);
      it = pid_to_row_.erase(it);
    } else {
      ++it;
    }
  }

  // Sort by CPU% descending (top's default "busiest first" ordering).
  std::vector<row_entry*> rows;
  rows.reserve(pid_to_row_.size());
  for (auto& [pid, entry] : pid_to_row_)
    rows.push_back(&entry);
  std::sort(rows.begin(), rows.end(), [](const row_entry* a, const row_entry* b) {
    return a->cpu_percent > b->cpu_percent;
  });
  for (size_t i = 0; i < rows.size(); ++i)
    rows[i]->row["order"_key] = static_cast<int32_t>(i);

  proc_table_->refresh_children_order();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void process_explorer::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_process_explorer() {
  auto proto = dynamic_ptr{"ProcessExplorer"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Process Explorer"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addMethod(
      "update_snapshot"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<process_explorer&>(self).do_update_snapshot(args);
      }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("ProcessExplorer"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Read-only top/htop-style system monitor: CPU and memory history graphs, "
                        "per-core meters, and a process table. The client owns all sampling and "
                        "calls update_snapshot() periodically; the server only renders whatever "
                        "it was last given. Listen for the 'closed' event to detect when the "
                        "user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<process_explorer>("wish"_key, "ProcessExplorer"_key));
}

} // namespace bdg::wish
