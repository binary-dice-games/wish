// MIT License © 2025 Binary Dice Games
/// @file top.cpp
/// @brief Implementation of the Top form.
#include "top.hpp"
#include "modules/bdg/desktop/top/top_settings.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/dock_layout_spec.hpp>
#include <ui/forms/properties_dialog.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;
using common::wish_id_of;

namespace {

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

std::string format_rate(float bytes_per_sec) {
  return format_bytes(bytes_per_sec) + "/s";
}

// A throughput graph's display unit: the largest one the visible peak still
// reaches at least 1 of, so the Y axis reads "12.5 MB/s" rather than
// "13107200".
struct rate_unit {
  const char* label;
  float divisor;
};
rate_unit pick_rate_unit(float peak_bytes_per_sec) {
  static constexpr rate_unit kUnits[] = {
      {"GB/s", 1024.0f * 1024.0f * 1024.0f}, {"MB/s", 1024.0f * 1024.0f}, {"KB/s", 1024.0f}};
  for (auto& unit : kUnits)
    if (peak_bytes_per_sec >= unit.divisor)
      return unit;
  return {"B/s", 1.0f};
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
    {"$$TOP_LOW", 19},
    {"$$TOP_BELOW_NORMAL", 10},
    {"$$TOP_NORMAL", 0},
    {"$$TOP_ABOVE_NORMAL", -5},
    {"$$TOP_HIGH", -10},
    {"$$TOP_REALTIME", -20},
};
constexpr size_t kPriorityLevelCount = std::size(kPriorityLevels);

std::string to_lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return text;
}

} // namespace

// ── UI layouts ────────────────────────────────────────────────────────────────
//
// The tool is split into seven dockable panels -- Processes, CPU, Memory,
// Network, Disk I/O, Ports and Cores -- seeded into a first-run arrangement by on_init()'s
// set_default_dock_layout() call, the same multi-window pattern the dev
// modules (git, curl, docker) use. The user can re-dock, tab or float any of
// them; imgui.ini owns the arrangement after the first run. Each Window keeps
// a width/height: the size a panel restores to when dragged out of the dock.
//
// Processes (the form's main root, internal_root_key_):
// "proc_table"'s "flags": "Resizable|RowBg|Borders|Sortable|ScrollY" (the
// first three match the "tbl_catalog" example in examples/demo/main.cpp;
// Sortable makes column headers clickable -- see the "sorted" event handling
// in on_event()/resort_rows()). Each TableColumn's "flags" is "WidthFixed",
// except "col_cpu" which also ORs in "DefaultSort|PreferSortDescending" so
// the column-header UI's initial sort indicator matches sort_column_id_/
// sort_ascending_'s own defaults below. Rows are added, updated, and
// removed at runtime by update_snapshot(), same as nano's tabs.
//
// "toolbar" holds a case-insensitive name filter (docker.cpp's container
// filter pattern): each "changed" event re-runs apply_process_filter(),
// which toggles every row's "visible" field, as does every snapshot (so
// newly-appeared processes respect the current filter).
//
// "proc_table" carries "height": -1 (mc.cpp's left_table/right_table
// technique): "vbox" hands "toolbar"/"status_label" their natural height first, then
// gives "proc_table" whatever's left, so only the process list scrolls
// ("ScrollY") and the status label stays pinned to the panel's bottom.
//
// CPU / Memory: a summary label over a Plot with "height": -1 (sq.cpp's
// chart technique), so the graph fills whatever space the panel is docked
// into instead of a fixed 160px strip.
//
// Network / Disk I/O: like CPU / Memory, but with two series each and no
// fixed Y range: update_rate_plot() rescales the Y axis to the visible peak
// on every snapshot and relabels it with the unit that fits (B/s .. GB/s).
//
// Ports: a plain (non-sortable) Table of listening sockets, in the order
// the client sends them (by port), filling the panel like proc_table does.
// update_ports_table() rebuilds its rows only when the list changes.
//
// Cores: one ProgressBar per logical core, stacked vertically (default
// width -1 = full panel width) and added on the first snapshot by
// ensure_core_meters(). "cores" is given an explicit empty "children"
// object -- even though empty -- so the importer allocates a private
// children map for this instance instead of sharing the Element base
// prototype's default (see nano.cpp's tab_bar for the same technique).

static constexpr const char* kProcessesLayout = R"({
  "type": "Window",
  "title": "$$TOP_PROCESSES",
  "width": 900, "height": 460,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout", "spacing": 8,
          "children": {
            "filter": { "type": "InputText", "hint": "$$TOP_FILTER_BY_NAME", "width": 240 },
            "filter_count": { "type": "Label", "text": "" }
          }
        },
        "proc_table": {
          "type": "Table", "id": "##proc_table", "columns": 6,
          "profiler_marker": "Process Table",
          "flags": "Resizable|RowBg|Borders|Sortable|ScrollY", "headers": true,
          "outer_width": 0, "height": -1, "auto_scroll": false,
          "children": {
            "col_pid":   { "type": "TableColumn", "label": "$$TOP_PID",     "flags": "WidthFixed", "init_width": 70,  "column_id": 0 },
            "col_name":  { "type": "TableColumn", "label": "$$TOP_NAME",    "flags": "WidthFixed", "init_width": 140, "column_id": 1 },
            "col_state": { "type": "TableColumn", "label": "$$TOP_STATE",   "flags": "WidthFixed", "init_width": 60,  "column_id": 2 },
            "col_cpu":   { "type": "TableColumn", "label": "$$TOP_CPU",   "flags": "WidthFixed|DefaultSort|PreferSortDescending", "init_width": 100, "column_id": 3 },
            "col_mem":   { "type": "TableColumn", "label": "$$TOP_MEMORY",  "flags": "WidthFixed", "init_width": 100, "column_id": 4 },
            "col_cmd":   { "type": "TableColumn", "label": "$$TOP_COMMAND", "column_id": 5 }
          }
        },
        "status_label": { "type": "Label", "text": "" }
      }
    }
  }
})";

static constexpr const char* kCpuLayout = R"({
  "type": "Window",
  "title": "$$TOP_CPU_SHORT",
  "width": 450, "height": 240,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "cpu_label": { "type": "Label", "text": "$$TOP_CPU_NONE" },
        "cpu_plot": {
          "type": "Plot", "title": "$$TOP_CPU_HISTORY", "height": -1,
          "profiler_marker": "CPU Plot",
          "y_label": "%",
          "children": {
            "cpu_series": { "type": "PlotShaded", "label": "$$TOP_CPU" }
          }
        }
      }
    }
  }
})";

static constexpr const char* kMemoryLayout = R"({
  "type": "Window",
  "title": "$$TOP_MEMORY",
  "width": 450, "height": 240,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "mem_label": { "type": "Label", "text": "$$TOP_MEM_NONE" },
        "mem_plot": {
          "type": "Plot", "title": "$$TOP_MEMORY_HISTORY", "height": -1,
          "profiler_marker": "Memory Plot",
          "y_label": "%",
          "children": {
            "mem_series": { "type": "PlotLine", "label": "$$TOP_MEMORY_PERCENT" }
          }
        }
      }
    }
  }
})";

static constexpr const char* kNetworkLayout = R"({
  "type": "Window",
  "title": "$$TOP_NETWORK",
  "width": 450, "height": 240,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "net_label": { "type": "Label", "text": "$$TOP_NET_NONE" },
        "net_plot": {
          "type": "Plot", "title": "$$TOP_NETWORK_HISTORY", "height": -1,
          "profiler_marker": "Network Plot",
          "y_label": "B/s",
          "children": {
            "rx_series": { "type": "PlotLine", "label": "$$TOP_RECEIVED" },
            "tx_series": { "type": "PlotLine", "label": "$$TOP_SENT" }
          }
        }
      }
    }
  }
})";

static constexpr const char* kDiskLayout = R"({
  "type": "Window",
  "title": "$$TOP_DISK",
  "width": 450, "height": 240,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "disk_label": { "type": "Label", "text": "$$TOP_DISK_NONE" },
        "disk_plot": {
          "type": "Plot", "title": "$$TOP_DISK_HISTORY", "height": -1,
          "profiler_marker": "Disk Plot",
          "y_label": "B/s",
          "children": {
            "read_series": { "type": "PlotLine", "label": "$$TOP_READ" },
            "write_series": { "type": "PlotLine", "label": "$$TOP_WRITE" }
          }
        }
      }
    }
  }
})";

static constexpr const char* kPortsLayout = R"({
  "type": "Window",
  "title": "$$TOP_PORTS",
  "width": 600, "height": 360,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "ports_count": { "type": "Label", "text": "" },
        "ports_table": {
          "type": "Table", "id": "##ports_table", "columns": 5,
          "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true,
          "outer_width": 0, "height": -1, "auto_scroll": false,
          "children": {
            "col_proto":   { "type": "TableColumn", "label": "$$TOP_PROTOCOL", "flags": "WidthFixed", "init_width": 70 },
            "col_address": { "type": "TableColumn", "label": "$$TOP_ADDRESS",  "flags": "WidthFixed", "init_width": 200 },
            "col_port":    { "type": "TableColumn", "label": "$$TOP_PORT",     "flags": "WidthFixed", "init_width": 70 },
            "col_pid":     { "type": "TableColumn", "label": "$$TOP_PID",      "flags": "WidthFixed", "init_width": 70 },
            "col_process": { "type": "TableColumn", "label": "$$TOP_PROCESS" }
          }
        }
      }
    }
  }
})";

static constexpr const char* kCoresLayout = R"({
  "type": "Window",
  "title": "$$TOP_CORES",
  "width": 240, "height": 700,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "cores": { "type": "VerticalLayout", "spacing": 4, "children": {} }
      }
    }
  }
})";

// Confirm-kill and Process Properties are both privately-instantiated
// built-in forms (MessageBox / PropertiesDialog, see form::instantiate_child_form())
// rather than raw element trees owned directly by this form -- see
// show_confirm_kill()/show_properties_dialog() below.

// ── top ─────────────────────────────────────────────────────────

top::top(dynamic&& base) : tool_form(std::move(base)) {}

top::~top() {
  remove_panel_objects();
}

void top::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__top_");
  max_history_ = size_t(setting_int(top_settings::history()));
  cpu_root_key_ = internal_root_key_ + "_cpu";
  mem_root_key_ = internal_root_key_ + "_mem";
  net_root_key_ = internal_root_key_ + "_net";
  disk_root_key_ = internal_root_key_ + "_disk";
  ports_root_key_ = internal_root_key_ + "_ports";
  cores_root_key_ = internal_root_key_ + "_cores";

  auto* title_f = findField<std::string>("title"_key);
  const std::string title = tr(title_f ? *title_f : std::string{"$$TOP_TOP"});

  // Fix both axes so the graphs read as stable percentage gauges instead of
  // auto-fitting (which otherwise locks onto whatever tiny range existed on
  // the very first render, before any real data arrived) -- Y is always
  // 0..100%; X is a rolling sample-index window with its numeric labels
  // hidden, since "sample 37" isn't meaningful to a user.
  auto fix_axes = [this](const auto& e) {
    e["x_min"_key] = 0.0f;
    e["x_max"_key] = static_cast<float>(max_history_ - 1);
    e["y_min"_key] = 0.0f;
    e["y_max"_key] = 100.0f;
    e["x_flags"_key] = kHideXTickLabels;
  };

  build_window(internal_root_key_, kProcessesLayout, window_id_, [&](ui_tree& tree) {
    tree.with("vbox.proc_table", [&](const auto& e) {
      proc_table_ = e;
      proc_table_id_ = wish_id_of(e);
    });
    tree.with("vbox.status_label", [&](const auto& e) { status_label_ = e; });
    tree.with("vbox.toolbar.filter", [&](const auto& e) { filter_input_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.filter_count", [&](const auto& e) { filter_count_label_ = e; });
  });

  build_window(cpu_root_key_, kCpuLayout, cpu_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.cpu_label", [&](const auto& e) { cpu_summary_label_ = e; });
    tree.with("vbox.cpu_plot.cpu_series", [&](const auto& e) { cpu_plot_series_ = e; });
    tree.with("vbox.cpu_plot", fix_axes);
  });

  build_window(mem_root_key_, kMemoryLayout, mem_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.mem_label", [&](const auto& e) { mem_summary_label_ = e; });
    tree.with("vbox.mem_plot.mem_series", [&](const auto& e) { mem_plot_series_ = e; });
    tree.with("vbox.mem_plot", fix_axes);
  });

  // Throughput graphs: same fixed X window as the percentage graphs; Y is
  // left to update_rate_plot(), starting at 0..1 until data arrives.
  auto fix_rate_axes = [this](const auto& e) {
    e["x_min"_key] = 0.0f;
    e["x_max"_key] = static_cast<float>(max_history_ - 1);
    e["y_min"_key] = 0.0f;
    e["y_max"_key] = 1.0f;
    e["x_flags"_key] = kHideXTickLabels;
  };

  build_window(net_root_key_, kNetworkLayout, net_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.net_label", [&](const auto& e) { net_summary_label_ = e; });
    tree.with("vbox.net_plot.rx_series", [&](const auto& e) { net_rx_series_ = e; });
    tree.with("vbox.net_plot.tx_series", [&](const auto& e) { net_tx_series_ = e; });
    tree.with("vbox.net_plot", [&](const auto& e) {
      net_plot_ = e;
      fix_rate_axes(e);
    });
  });

  build_window(disk_root_key_, kDiskLayout, disk_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.disk_label", [&](const auto& e) { disk_summary_label_ = e; });
    tree.with("vbox.disk_plot.read_series", [&](const auto& e) { disk_read_series_ = e; });
    tree.with("vbox.disk_plot.write_series", [&](const auto& e) { disk_write_series_ = e; });
    tree.with("vbox.disk_plot", [&](const auto& e) {
      disk_plot_ = e;
      fix_rate_axes(e);
    });
  });

  build_window(ports_root_key_, kPortsLayout, ports_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.ports_count", [&](const auto& e) { ports_count_label_ = e; });
    tree.with("vbox.ports_table", [&](const auto& e) { ports_table_ = e; });
  });

  build_window(cores_root_key_, kCoresLayout, cores_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.cores", [&](const auto& e) { cores_container_ = e; });
  });

  // Seed the first-run arrangement inside the tool's own nested dockspace
  // (titled with the form's "title" field): a Cores column along the right
  // ~20%, and to its left a row of CPU/Memory/Network/Disk graphs over the
  // process table, with Ports as a second tab next to Processes. Owned by
  // imgui.ini after the first run (see docs/dock-layout.md); bump the
  // version arg to layout() if it changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "top_dock",
        title,
        layout(
            split(
                dir::right,
                0.20f,
                area({cores_root_key_}),
                split(
                    dir::up,
                    0.35f,
                    split(
                        dir::left,
                        0.50f,
                        split(dir::left, 0.50f, area({cpu_root_key_}), area({mem_root_key_})),
                        split(dir::left, 0.50f, area({net_root_key_}), area({disk_root_key_}))),
                    area({internal_root_key_, ports_root_key_}, internal_root_key_))),
            /*version=*/2,
            /*target=*/"top_dock")));
  }
}

void top::remove_panel_objects() {
  // Keys are forgotten once removed: next_available_key() may hand this
  // form's freed internal_root_key_ to a new Top instance, whose panels
  // would then reuse these exact secondary keys (see the same reasoning in
  // form::remove_objects_at()).
  for (std::string* key :
       {&cpu_root_key_, &mem_root_key_, &net_root_key_, &disk_root_key_, &ports_root_key_, &cores_root_key_}) {
    remove_objects_at(*key);
    key->clear();
  }
}

// ── update_snapshot ───────────────────────────────────────────────────────────

dynamic top::do_update_snapshot(const dynamic& args) {
  float cpu_pct = args.as<float>("cpu_percent"_key);
  if (cpu_summary_label_)
    cpu_summary_label_["text"_key] = trf("$$TOP_CPU_SUMMARY", {format_percent(cpu_pct)});
  push_history(cpu_history_, cpu_pct);

  float mem_total = args.as<float>("mem_total_bytes"_key);
  float mem_used = args.as<float>("mem_used_bytes"_key);
  float mem_pct = mem_total > 0 ? 100.0f * mem_used / mem_total : 0.0f;
  if (mem_summary_label_) {
    mem_summary_label_["text"_key] =
        trf("$$TOP_MEM_SUMMARY", {format_percent(mem_pct), format_bytes(mem_used), format_bytes(mem_total)});
  }
  push_history(mem_history_, mem_pct);

  // Network / disk fields are optional on the wire (an older client simply
  // shows 0 throughput); dynamic::as<float>() would throw on a missing key.
  auto optional_float = [&](key_t key) {
    auto* f = args.findField<float>(key);
    return f ? *f : 0.0f;
  };
  float net_rx = optional_float("net_rx_bytes_per_sec"_key);
  float net_tx = optional_float("net_tx_bytes_per_sec"_key);
  if (net_summary_label_) {
    net_summary_label_["text"_key] =
        trf("$$TOP_NET_SUMMARY",
            {format_rate(net_rx),
             format_rate(net_tx),
             format_bytes(optional_float("net_rx_total_bytes"_key)),
             format_bytes(optional_float("net_tx_total_bytes"_key))});
  }
  push_history(net_rx_history_, net_rx);
  push_history(net_tx_history_, net_tx);

  float disk_read = optional_float("disk_read_bytes_per_sec"_key);
  float disk_write = optional_float("disk_write_bytes_per_sec"_key);
  if (disk_summary_label_) {
    disk_summary_label_["text"_key] =
        trf("$$TOP_DISK_SUMMARY",
            {format_rate(disk_read),
             format_rate(disk_write),
             format_bytes(optional_float("disk_read_total_bytes"_key)),
             format_bytes(optional_float("disk_write_total_bytes"_key))});
  }
  push_history(disk_read_history_, disk_read);
  push_history(disk_write_history_, disk_write);

  // Every history is pushed exactly once per call, so they always share
  // one length -- an index axis (0, 1, 2, ...) is all any series needs.
  update_history_xs(cpu_history_.size());
  if (cpu_plot_series_) {
    cpu_plot_series_["xs"_key] = history_xs_;
    cpu_plot_series_["ys"_key] = cpu_history_;
  }
  if (mem_plot_series_) {
    mem_plot_series_["xs"_key] = history_xs_;
    mem_plot_series_["ys"_key] = mem_history_;
  }
  update_rate_plot(net_plot_, net_rx_series_, net_tx_series_, net_rx_history_, net_tx_history_);
  update_rate_plot(disk_plot_, disk_read_series_, disk_write_series_, disk_read_history_, disk_write_history_);

  if (auto* per_core = args.findField<std::vector<float>>("per_core_percent"_key)) {
    ensure_core_meters(per_core->size());
    for (size_t i = 0; i < core_bars_.size() && i < per_core->size(); ++i) {
      float pct = (*per_core)[i];
      core_bars_[i]["value"_key] = pct / 100.0f;
      core_bars_[i]["label"_key] = trf("$$TOP_CORE_PERCENT", {std::to_string(i), format_percent(pct)});
    }
  }

  update_process_table(args);
  update_ports_table(args);
  return dynamic{};
}

dynamic top::do_report_action_result(const dynamic& args) {
  int32_t pid = args.as<int32_t>("pid"_key);
  std::string action = args.as<std::string>("action"_key);
  bool success = args.as<bool>("success"_key);
  std::string error = args.as<std::string>("error"_key);

  // One full sentence per action and outcome, so every language can phrase it.
  static const std::unordered_map<std::string, std::pair<const char*, const char*>> kMessages = {
      {"kill", {"$$TOP_KILL_SUCCEEDED", "$$TOP_KILL_FAILED"}},
      {"pause", {"$$TOP_PAUSE_SUCCEEDED", "$$TOP_PAUSE_FAILED"}},
      {"resume", {"$$TOP_RESUME_SUCCEEDED", "$$TOP_RESUME_FAILED"}},
      {"set_priority", {"$$TOP_SET_PRIORITY_SUCCEEDED", "$$TOP_SET_PRIORITY_FAILED"}},
      {"set_affinity", {"$$TOP_SET_AFFINITY_SUCCEEDED", "$$TOP_SET_AFFINITY_FAILED"}},
  };
  const std::string pid_text = std::to_string(pid);
  const std::string reason = error.empty() ? tr("$$TOP_UNKNOWN_ERROR") : error;
  auto it = kMessages.find(action);
  if (it == kMessages.end())
    set_status(success ? action + " " + pid_text : action + " " + pid_text + ": " + reason);
  else if (success)
    set_status(trf(it->second.first, {pid_text}));
  else
    set_status(trf(it->second.second, {pid_text, reason}));

  return dynamic{};
}

dynamic_ptr top::make_process_details(
    int32_t pid, int32_t ppid, const std::string& user, int32_t thread_count, const std::string& start_time,
    const std::string& exe_path, const std::string& cwd, const std::string& cmdline, int32_t nice,
    const std::vector<int32_t>& affinity_cores) {
  dynamic_ptr details{dynamic::instantiate("wish"_key, "ProcessDetails"_key)};
  details["pid"_key] = pid;
  details["ppid"_key] = ppid;
  const std::string unknown = tr("$$TOP_UNKNOWN");
  details["user"_key] = user.empty() ? unknown : user;
  details["thread_count"_key] = thread_count;
  details["start_time"_key] = start_time.empty() ? unknown : start_time;

  std::string level_label = tr("$$TOP_CUSTOM");
  for (auto& level : kPriorityLevels)
    if (level.nice == nice)
      level_label = tr(level.label);
  details["priority"_key] = trf("$$TOP_PRIORITY_NICE", {level_label, std::to_string(nice)});

  std::ostringstream affinity_oss;
  if (affinity_cores.empty()) {
    affinity_oss << unknown;
  } else {
    for (size_t i = 0; i < affinity_cores.size(); ++i) {
      if (i > 0)
        affinity_oss << ", ";
      affinity_oss << affinity_cores[i];
    }
  }
  details["affinity"_key] = affinity_oss.str();

  details["exe_path"_key] = exe_path.empty() ? unknown : exe_path;
  details["cwd"_key] = cwd.empty() ? tr("$$TOP_UNAVAILABLE") : cwd;
  details["cmdline"_key] = cmdline;
  return details;
}

dynamic top::do_report_process_details(const dynamic& args) {
  int32_t pid = args.as<int32_t>("pid"_key);
  // Stale response guard: the Properties dialog may have been closed, or
  // reopened for a different pid, before this RMI call arrived.
  if (!properties_dialog_ || pid != properties_dialog_pid_)
    return dynamic{};

  bool found = args.as<bool>("found"_key);
  if (!found) {
    std::string error = args.as<std::string>("error"_key);
    std::string message = error.empty() ? tr("$$TOP_PROCESS_GONE") : error;
    dynamic_ptr details{dynamic::instantiate("wish"_key, "ProcessDetails"_key)};
    details["pid"_key] = pid;
    details["user"_key] = message;
    details["start_time"_key] = message;
    details["priority"_key] = message;
    details["affinity"_key] = message;
    details["exe_path"_key] = message;
    details["cwd"_key] = message;
    details["cmdline"_key] = message;
    properties_dialog_->set_target(details);
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

  properties_dialog_->set_target(
      make_process_details(pid, ppid, user, threads, start_time, exe_path, cwd, cmdline, nice, affinity_cores));
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
    ui_element_ptr bar = ui_element_ptr::create("wish"_key, "ProgressBar"_key);
    // No explicit width: ProgressBar's default (-1) spans the Cores panel.
    bar["label"_key] = trf("$$TOP_CORE_PERCENT", {std::to_string(i), "--"});
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
  if (history.size() > max_history_)
    history.erase(history.begin());
}

void top::update_history_xs(size_t count) {
  if (history_xs_.size() == count)
    return;
  history_xs_.resize(count);
  for (size_t i = 0; i < count; ++i)
    history_xs_[i] = static_cast<float>(i);
}

void top::update_rate_plot(
    const ui_element_ptr& plot,
    const ui_element_ptr& first_series,
    const ui_element_ptr& second_series,
    const std::vector<float>& first,
    const std::vector<float>& second) {
  if (!plot || !first_series || !second_series)
    return;
  float peak = 0.0f;
  for (const auto* history : {&first, &second})
    for (float v : *history)
      peak = std::max(peak, v);
  rate_unit unit = pick_rate_unit(peak);

  auto scaled = [&](const std::vector<float>& history) {
    std::vector<float> ys(history.size());
    for (size_t i = 0; i < history.size(); ++i)
      ys[i] = history[i] / unit.divisor;
    return ys;
  };
  first_series["xs"_key] = history_xs_;
  first_series["ys"_key] = scaled(first);
  second_series["xs"_key] = history_xs_;
  second_series["ys"_key] = scaled(second);

  // 15% headroom so the peak doesn't sit on the plot's top edge; never
  // below 1 unit, so an idle link doesn't magnify noise to full height.
  plot["y_max"_key] = std::max(1.0f, peak / unit.divisor * 1.15f);
  plot["y_label"_key] = std::string{unit.label};
}

void top::update_ports_table(const dynamic& args) {
  auto* ports_f = args.findField<dynamic_ptr>("ports"_key);
  if (!ports_table_ || !ports_f || !*ports_f)
    return;
  auto* children_p = ports_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<port_entry> ports;
  (*ports_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    auto& e = *f.as<dynamic_ptr>();
    ports.push_back(
        {e.as<std::string>("protocol"_key),
         e.as<std::string>("address"_key),
         e.as<int32_t>("port"_key),
         e.as<int32_t>("pid"_key),
         e.as<std::string>("process"_key)});
  });

  if (ports_count_label_)
    ports_count_label_["text"_key] = trf("$$TOP_PORTS_COUNT", {std::to_string(ports.size())});
  // Listening sockets rarely change between samples: skip the rebuild (and
  // its per-cell object churn) unless something actually did.
  if (ports == ports_)
    return;

  for (size_t key : port_row_keys_)
    children->erase(key);
  port_row_keys_.clear();

  auto make_label = [&](const std::string& text) {
    ui_element_ptr label = ui_element_ptr::create("wish"_key, "Label"_key);
    label["text"_key] = text;
    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, label);
    label["__wish_id"_key] = id;
    return label;
  };

  for (size_t i = 0; i < ports.size(); ++i) {
    const auto& p = ports[i];
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    key_t row_id = rmi::shared::generate_id();
    ctx().put_object(row_id, row);
    row["__wish_id"_key] = row_id;
    row["order"_key] = static_cast<int32_t>(i);

    auto row_children = dynamic_ptr{key_t{0U}, {}};
    size_t k = 0;
    (*row_children)[k++] = dynamic_ptr{make_label(p.protocol)};
    (*row_children)[k++] = dynamic_ptr{make_label(p.address)};
    (*row_children)[k++] = dynamic_ptr{make_label(std::to_string(p.port))};
    (*row_children)[k++] = dynamic_ptr{make_label(p.pid > 0 ? std::to_string(p.pid) : std::string{"-"})};
    (*row_children)[k++] = dynamic_ptr{make_label(p.process.empty() ? std::string{"-"} : p.process)};
    row["children"_key] = row_children;
    row->refresh_children_order();

    size_t child_key = next_child_key_++;
    (*children)[child_key] = dynamic_ptr{row};
    port_row_keys_.push_back(child_key);
  }
  ports_table_->refresh_children_order();
  ports_ = std::move(ports);
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

  ui_element_ptr menu = ui_element_ptr::create("wish"_key, "ContextMenu"_key);
  assign_id(menu);

  ui_element_ptr properties = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  properties["label"_key] = tr("$$TOP_PROPERTIES") + "...";
  common::set_icon(properties, "info");
  key_t properties_id = assign_id(properties);
  entry.properties_id = properties_id;
  action_item_targets_[properties_id] = row_action_target{pid, row_action_kind::properties, 0};

  ui_element_ptr sep1 = ui_element_ptr::create("wish"_key, "Separator"_key);
  assign_id(sep1);

  ui_element_ptr pause_resume = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  pause_resume["label"_key] = tr(state == "T" ? "$$TOP_RESUME" : "$$TOP_PAUSE");
  common::set_icon(pause_resume, state == "T" ? "play" : "pause");
  key_t pause_resume_id = assign_id(pause_resume);
  action_item_targets_[pause_resume_id] = row_action_target{pid, row_action_kind::pause_or_resume, 0};
  entry.pause_resume_item = pause_resume;

  ui_element_ptr kill = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  kill["label"_key] = tr("$$TOP_KILL_PROCESS");
  common::set_icon(kill, "close");
  key_t kill_id = assign_id(kill);
  entry.kill_id = kill_id;
  action_item_targets_[kill_id] = row_action_target{pid, row_action_kind::kill, 0};

  ui_element_ptr sep2 = ui_element_ptr::create("wish"_key, "Separator"_key);
  assign_id(sep2);

  ui_element_ptr priority_menu = ui_element_ptr::create("wish"_key, "Menu"_key);
  priority_menu["label"_key] = tr("$$TOP_PRIORITY");
  assign_id(priority_menu);

  entry.priority_items.clear();
  entry.priority_items.reserve(kPriorityLevelCount);
  auto priority_children = dynamic_ptr{key_t{0U}, {}};
  size_t pk = 0;
  for (auto& level : kPriorityLevels) {
    ui_element_ptr item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
    item["label"_key] = tr(level.label);
    item["checked"_key] = nice == level.nice;
    key_t item_id = assign_id(item);
    action_item_targets_[item_id] = row_action_target{pid, row_action_kind::priority, level.nice};
    (*priority_children)[pk++] = dynamic_ptr{item};
    entry.priority_items.push_back(item);
  }
  priority_menu["children"_key] = priority_children;
  priority_menu->refresh_children_order();

  ui_element_ptr affinity = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  affinity["label"_key] = tr("$$TOP_SET_CPU_AFFINITY") + "...";
  common::set_icon(affinity, "memory");
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
  if (entry.pause_resume_item) {
    entry.pause_resume_item["label"_key] = tr(state == "T" ? "$$TOP_RESUME" : "$$TOP_PAUSE");
    common::set_icon(entry.pause_resume_item, state == "T" ? "play" : "pause");
  }
  for (size_t i = 0; i < entry.priority_items.size() && i < kPriorityLevelCount; ++i)
    if (entry.priority_items[i])
      entry.priority_items[i]["checked"_key] = nice == kPriorityLevels[i].nice;
}

// ── Confirm-kill dialog ───────────────────────────────────────────────────────

void top::show_confirm_kill(int pid) {
  auto row_it = pid_to_row_.find(pid);
  std::string name = row_it != pid_to_row_.end() ? row_it->second.name : std::string{};
  std::string message = name.empty() ? trf("$$TOP_CONFIRM_KILL_PID", {std::to_string(pid)})
                                     : trf("$$TOP_CONFIRM_KILL_PID_NAME", {std::to_string(pid), name});

  show_confirm(
      message,
      [this, pid] {
        emit(
            "on_process_action_requested"_key,
            common::make_payload("pid"_key, static_cast<int32_t>(pid), "action"_key, std::string{"kill"}));
      },
      {.title = "$$TOP_CONFIRM_KILL"});
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
    // "\u001f" is make_message()'s separator: the label is translated as a
    // message once the JSON is parsed.
    checkboxes_json << "\"core" << i << "\": { \"type\": \"Checkbox\", \"label\": \"$$TOP_CORE_N\\u001f" << i
                     << "\", \"value\": " << (is_set(static_cast<int32_t>(i)) ? "true" : "false") << " }";
  }

  // Resizable, with "cores"'s "height": -1 absorbing any extra height the
  // user resizes into -- same mc.cpp kRenameLayout/kPropertiesLayout pattern
  // as kPropertiesLayout above -- so "sep"/"buttons" stay pinned to the
  // bottom instead of drifting up against a short core-checkbox list or
  // being pushed off-window by a long one.
  std::ostringstream layout;
  layout << R"({
    "type": "Window", "title": "$$TOP_AFFINITY_TITLE\u001f)" << pid << R"(", "modal": true,
    "flags": "NoCollapse",
    "width": 480, "height": 320,
    "children": {
      "vbox": {
        "type": "VerticalLayout",
        "children": {
          "cores": { "type": "VerticalLayout", "height": -1, "children": { )"
         << checkboxes_json.str() << R"( } },
          "sep": { "type": "Separator" },
          "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
            "btn_apply": { "type": "Button", "label": "$$TOP_APPLY", "shortcut": "Enter", "height": 32 },
            "btn_cancel": { "type": "Button", "label": "$$TOP_CANCEL", "shortcut": "Esc", "height": 32 }
          } }
        }
      }
    }
  })";

  auto tree = import_json(layout.str(), translations());
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  affinity_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.buttons.btn_apply", [&](const auto& e) { affinity_apply_id_ = wish_id_of(e); });
  tree.with("vbox.buttons.btn_cancel", [&](const auto& e) { affinity_cancel_id_ = wish_id_of(e); });

  affinity_checkboxes_.clear();
  for (size_t i = 0; i < core_count; ++i) {
    tree.with("vbox.cores.core" + std::to_string(i), [&](const auto& e) {
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
  properties_dialog_pid_ = pid;

  // Placeholder shown immediately, before the client's asynchronous
  // response arrives -- do_report_process_details() replaces it via
  // set_target() once "on_process_details_requested" is answered.
  dynamic_ptr placeholder{dynamic::instantiate("wish"_key, "ProcessDetails"_key)};
  placeholder["pid"_key] = static_cast<int32_t>(pid);
  placeholder["user"_key] = std::string{"Loading..."};
  placeholder["start_time"_key] = std::string{"Loading..."};
  placeholder["priority"_key] = std::string{"Loading..."};
  placeholder["affinity"_key] = std::string{"Loading..."};
  placeholder["exe_path"_key] = std::string{"Loading..."};
  placeholder["cwd"_key] = std::string{"Loading..."};
  placeholder["cmdline"_key] = std::string{"Loading..."};

  dynamic params;
  params["title"_key] = "Process Properties - PID " + std::to_string(pid);
  params["target"_key] = placeholder;

  // Overwriting properties_dialog_ (rather than requiring it be empty
  // first) is safe even if a properties dialog is already open for a
  // different pid -- see confirm_dialog_'s doc comment (properties_dialog_
  // follows the identical pattern).
  properties_dialog_ = instantiate_child_form<properties_dialog>("PropertiesDialog"_key, std::move(params));

  // emit() acquires the session wlock itself when called outside dispatch
  // (see form::emit()) -- must run after instantiate_child_form() releases
  // its own wlock, not while it's still held, or this deadlocks on the
  // same non-recursive mutex.
  dynamic req;
  req["pid"_key] = static_cast<int32_t>(pid);
  emit("on_process_details_requested"_key, std::move(req));
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
        entry.row = ui_element_ptr::create("wish"_key, "TableRow"_key);

        entry.pid_label = ui_element_ptr::create("wish"_key, "Label"_key);
        entry.pid_label["text"_key] = std::to_string(pid);

        entry.name_label = ui_element_ptr::create("wish"_key, "Label"_key);
        entry.name_label["text"_key] = name;

        entry.state_label = ui_element_ptr::create("wish"_key, "Label"_key);
        entry.state_label["text"_key] = state;

        entry.cpu_bar = ui_element_ptr::create("wish"_key, "ProgressBar"_key);
        entry.cpu_bar["value"_key] = cpu_percent / 100.0f;
        entry.cpu_bar["label"_key] = format_percent(cpu_percent);

        entry.mem_label = ui_element_ptr::create("wish"_key, "Label"_key);
        entry.mem_label["text"_key] = format_bytes(mem_rss);

        entry.command_label = ui_element_ptr::create("wish"_key, "Label"_key);
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
  apply_process_filter();
}

void top::apply_process_filter() {
  std::string needle = to_lower(filter_text_);
  size_t shown = 0;
  for (auto& [pid, entry] : pid_to_row_) {
    bool show = needle.empty() || to_lower(entry.name).find(needle) != std::string::npos;
    entry.row["visible"_key] = show;
    if (show)
      ++shown;
  }
  if (filter_count_label_) {
    filter_count_label_["text"_key] = needle.empty()
        ? std::string{}
        : std::to_string(shown) + " of " + std::to_string(pid_to_row_.size()) + " processes";
  }
}

void top::resort_rows() {
  if (!proc_table_ || pid_to_row_.empty())
    return;

  std::vector<std::pair<int, row_entry*>> rows;
  rows.reserve(pid_to_row_.size());
  for (auto& [pid, entry] : pid_to_row_)
    rows.push_back({pid, &entry});

  // Comparator always expressed in ascending terms; descending just swaps
  // the operand order, matching column_id assignments in kProcessesLayout above
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
  // Any panel's X button -> tear the whole tool down (docker/curl's rule).
  if (event == "closed"_key &&
      (id == window_id_ || id == cpu_window_id_ || id == mem_window_id_ || id == net_window_id_ ||
       id == disk_window_id_ || id == ports_window_id_ || id == cores_window_id_)) {
    emit("closed"_key);
    remove_panel_objects();
    remove_internal_objects();
    return;
  }

  if (id == filter_input_id_ && event == "changed"_key) {
    filter_text_ = payload.as<std::string>("value"_key);
    apply_process_filter();
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
        set_status(tr("$$TOP_AFFINITY_NEEDS_A_CORE"));
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
}

// ── Registration ──────────────────────────────────────────────────────────────

// Pure-data class reflected over by the PropertiesDialog show_properties_dialog()
// opens (see make_process_details()): every field is a plain, already-
// display-formatted string or int32 (formatting -- e.g. "Normal (nice 0)"
// for priority, "0, 1, 2" for affinity -- happens server-side in
// make_process_details(), not via bison attributes) shown read-only, so
// Order alone (not Range/Enum/etc.) is all that's needed to match the
// original hand-built grid's row order.
void register_process_details_class() {
  auto proto = dynamic_ptr{"ProcessDetails"_key, {}};

  auto add_field = [&](const char* name, auto default_value, const char* display_name, int32_t order) {
    proto->addField(
        key_t{name}, field{default_value, attr<DisplayName>(display_name), attr<Order>(order)});
  };

  add_field("pid", int32_t{0}, "PID", 0);
  add_field("ppid", int32_t{0}, "Parent PID", 1);
  add_field("user", std::string{}, "User", 2);
  add_field("thread_count", int32_t{0}, "Threads", 3);
  add_field("start_time", std::string{}, "Started", 4);
  add_field("priority", std::string{}, "Priority", 5);
  add_field("affinity", std::string{}, "CPU Affinity", 6);
  add_field("exe_path", std::string{}, "Executable", 7);
  add_field("cwd", std::string{}, "Working Dir", 8);
  add_field("cmdline", std::string{}, "Command", 9);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Process Details"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Extended per-process info shown by top's Properties dialog "
                        "(see top::make_process_details())."));

  dynamic::addClass("wish"_key, std::move(proto), key_t{0U});
}

void register_top() {
  register_process_details_class();
  common::register_tool_settings(top_settings::all());

  auto proto = dynamic_ptr{"Top"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"$$TOP_TOP"},
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
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Top/htop-style system monitor: CPU, memory, network and disk I/O "
      "history graphs, per-core meters, a listening-ports table, and a process table. Right-click a row to kill, "
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
