// MIT License © 2025 Binary Dice Games
/// @file process_explorer.cpp
/// @brief Implementation of the ProcessExplorer form.
#include "process_explorer.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui_importer.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

std::string format_percent(double pct) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << pct << "%";
  return oss.str();
}

std::string format_bytes(uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << value << " " << kUnits[unit];
  return oss.str();
}

double percent_of(uint64_t part, uint64_t whole) {
  return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

// Assigns a fresh RMI id to a freshly-instantiated element and registers it
// in ctx.objects, mirroring the calculator/notepad import-time convention:
// every element in a form's internal tree gets a __wish_id, not just the
// interactive ones.
void register_new_element(bison::rmi::context& ctx, ui_element_ptr& elem) {
  key_t id = rmi::shared::generate_id();
  ctx.objects[id.id] = elem;
  elem["__wish_id"_key] = id;
}

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// ImGuiTableFlags_Resizable=1, RowBg=64, Borders=1920 -> 1985 (matches the
// "tbl_catalog" example in examples/demo/main.cpp).
// "cores" is given an explicit empty "children" object -- even though empty
// -- so the importer allocates a private children map for this instance
// instead of sharing the Element base prototype's default (see notepad.cpp's
// tab_bar for the same technique).

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
            "col_pid":   { "type": "TableColumn", "label": "PID",     "init_width": 70 },
            "col_name":  { "type": "TableColumn", "label": "Name",    "init_width": 140 },
            "col_state": { "type": "TableColumn", "label": "State",   "init_width": 60 },
            "col_cpu":   { "type": "TableColumn", "label": "CPU %",   "init_width": 100 },
            "col_mem":   { "type": "TableColumn", "label": "Memory",  "init_width": 100 },
            "col_cmd":   { "type": "TableColumn", "label": "Command" }
          }
        }
      }
    }
  }
})";

// ── process_explorer ─────────────────────────────────────────────────────────

process_explorer::process_explorer(dynamic&& base) : form(std::move(base)) {}

process_explorer::~process_explorer() {
  stop_->store(true, std::memory_order_relaxed);
  if (detail::current_session != nullptr) {
    // We are being destroyed synchronously from inside an RMI dispatch that
    // already holds this session's write lock (e.g. an explicit client-side
    // destroy request) -- joining here would deadlock if the refresh thread
    // is currently blocked acquiring that same lock. The client connection
    // (and ctx) stay alive regardless, and the refresh thread only touches
    // `state_` (kept alive by its own shared_ptr) rather than `this`, so
    // detaching is safe: the thread notices `stop_` and exits on its own.
    if (refresh_thread_.joinable())
      refresh_thread_.detach();
  } else if (refresh_thread_.joinable()) {
    refresh_thread_.join();
  }
}

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

  state_ = std::make_shared<refresh_state>();
  state_->ctx = &ctx();

  tree.with("vbox.summary.cpu_label", [&](const auto& e) { state_->cpu_summary_label = e; });
  tree.with("vbox.summary.mem_label", [&](const auto& e) { state_->mem_summary_label = e; });
  tree.with("vbox.cores", [&](const auto& e) { state_->cores_container = e; });
  tree.with("vbox.cpu_plot.cpu_series", [&](const auto& e) { state_->cpu_plot_series = e; });
  tree.with("vbox.mem_plot.mem_series", [&](const auto& e) { state_->mem_plot_series = e; });
  tree.with("vbox.proc_table", [&](const auto& e) { state_->proc_table = e; });

  sess().objects.merge(std::move(tree), internal_root_key_);

  // One synchronous sample to size the core-meter row and populate the
  // table/plots before the window is ever rendered.
  auto snap = state_->source.sample();
  build_core_meters(snap.system.per_core_percent.size());
  apply_snapshot(*state_, snap);

  start_refresh_thread();
}

void process_explorer::build_core_meters(size_t core_count) {
  if (!state_->cores_container)
    return;
  auto* children_p = state_->cores_container->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (size_t i = 0; i < core_count; ++i) {
    ui_element_ptr bar{dynamic::instantiate("wish"_key, "ProgressBar"_key)};
    bar["label"_key] = "Core " + std::to_string(i) + ": --";
    bar["width"_key] = 90.0f;
    bar["order"_key] = static_cast<int32_t>(i);
    register_new_element(ctx(), bar);

    (*children)[state_->next_child_key++] = dynamic_ptr{bar};
    state_->core_bars.push_back(bar);
  }
  state_->cores_container->refresh_children_order();
}

void process_explorer::start_refresh_thread() {
  auto state = state_;
  auto sess_ptr = sync_sess_;
  auto stop = stop_;
  refresh_thread_ = std::thread([state, sess_ptr, stop] {
    using namespace std::chrono_literals;
    while (!stop->load(std::memory_order_relaxed)) {
      // Sleep in short chunks so a stop request is noticed promptly instead
      // of after a full ~1s tick.
      for (int i = 0; i < 10 && !stop->load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(100ms);
      if (stop->load(std::memory_order_relaxed))
        break;

      auto snap = state->source.sample(); // /proc I/O -- no lock held

      auto lock = sess_ptr->wlock();
      if (stop->load(std::memory_order_relaxed))
        break; // torn down while we were waiting for the lock
      apply_snapshot(*state, snap);
    }
  });
}

// ── Snapshot application (pure UI update; safe to call without `this`) ───────

void process_explorer::apply_snapshot(refresh_state& state, const system_snapshot& snap) {
  if (state.cpu_summary_label)
    state.cpu_summary_label["text"_key] = "CPU: " + format_percent(snap.system.cpu_percent);

  double mem_pct = percent_of(snap.system.mem_used_bytes, snap.system.mem_total_bytes);
  if (state.mem_summary_label) {
    state.mem_summary_label["text"_key] = "Mem: " + format_percent(mem_pct) + " (" +
                                           format_bytes(snap.system.mem_used_bytes) + " / " +
                                           format_bytes(snap.system.mem_total_bytes) + ")";
  }

  for (size_t i = 0; i < state.core_bars.size() && i < snap.system.per_core_percent.size(); ++i) {
    double pct = snap.system.per_core_percent[i];
    state.core_bars[i]["value"_key] = static_cast<float>(pct / 100.0);
    state.core_bars[i]["label"_key] = "Core " + std::to_string(i) + ": " + format_percent(pct);
  }

  push_history(state.cpu_history, static_cast<float>(snap.system.cpu_percent));
  if (state.cpu_plot_series)
    state.cpu_plot_series["ys"_key] = state.cpu_history;

  push_history(state.mem_history, static_cast<float>(mem_pct));
  if (state.mem_plot_series)
    state.mem_plot_series["ys"_key] = state.mem_history;

  update_process_table(state, snap.processes);
}

void process_explorer::push_history(std::vector<float>& history, float value) {
  static constexpr size_t kMaxHistory = 60;
  history.push_back(value);
  if (history.size() > kMaxHistory)
    history.erase(history.begin());
}

void process_explorer::update_process_table(refresh_state& state, const std::vector<process_sample>& processes) {
  if (!state.proc_table)
    return;
  auto* children_p = state.proc_table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::unordered_map<int, bool> seen;
  seen.reserve(processes.size());

  for (auto& p : processes) {
    seen[p.pid] = true;
    auto it = state.pid_to_row.find(p.pid);
    if (it == state.pid_to_row.end()) {
      row_entry entry;
      entry.row = ui_element_ptr{dynamic::instantiate("wish"_key, "TableRow"_key)};

      entry.pid_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
      entry.pid_label["text"_key] = std::to_string(p.pid);

      entry.name_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
      entry.name_label["text"_key] = p.name;

      entry.state_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
      entry.state_label["text"_key] = std::string(1, p.state);

      entry.cpu_bar = ui_element_ptr{dynamic::instantiate("wish"_key, "ProgressBar"_key)};
      entry.cpu_bar["value"_key] = static_cast<float>(p.cpu_percent / 100.0);
      entry.cpu_bar["label"_key] = format_percent(p.cpu_percent);

      entry.mem_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
      entry.mem_label["text"_key] = format_bytes(p.mem_rss_bytes);

      entry.command_label = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
      entry.command_label["text"_key] = p.command;

      register_new_element(*state.ctx, entry.row);
      register_new_element(*state.ctx, entry.pid_label);
      register_new_element(*state.ctx, entry.name_label);
      register_new_element(*state.ctx, entry.state_label);
      register_new_element(*state.ctx, entry.cpu_bar);
      register_new_element(*state.ctx, entry.mem_label);
      register_new_element(*state.ctx, entry.command_label);

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

      entry.child_key = state.next_child_key++;
      (*children)[entry.child_key] = dynamic_ptr{entry.row};
      entry.cpu_percent = p.cpu_percent;

      state.pid_to_row.emplace(p.pid, std::move(entry));
    } else {
      auto& entry = it->second;
      entry.name_label["text"_key] = p.name;
      entry.state_label["text"_key] = std::string(1, p.state);
      entry.cpu_bar["value"_key] = static_cast<float>(p.cpu_percent / 100.0);
      entry.cpu_bar["label"_key] = format_percent(p.cpu_percent);
      entry.mem_label["text"_key] = format_bytes(p.mem_rss_bytes);
      entry.command_label["text"_key] = p.command;
      entry.cpu_percent = p.cpu_percent;
    }
  }

  // Remove rows for pids that no longer exist.
  for (auto it = state.pid_to_row.begin(); it != state.pid_to_row.end();) {
    if (!seen.count(it->first)) {
      children->erase(it->second.child_key);
      it = state.pid_to_row.erase(it);
    } else {
      ++it;
    }
  }

  // Sort by CPU% descending (top's default "busiest first" ordering).
  std::vector<row_entry*> rows;
  rows.reserve(state.pid_to_row.size());
  for (auto& [pid, entry] : state.pid_to_row)
    rows.push_back(&entry);
  std::sort(rows.begin(), rows.end(), [](const row_entry* a, const row_entry* b) {
    return a->cpu_percent > b->cpu_percent;
  });
  for (size_t i = 0; i < rows.size(); ++i)
    rows[i]->row["order"_key] = static_cast<int32_t>(i);

  state.proc_table->refresh_children_order();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void process_explorer::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (id == window_id_ && event == "closed"_key) {
    stop_->store(true, std::memory_order_relaxed);
    // on_event runs outside dispatch (no session wlock held by this thread),
    // so joining here cannot deadlock against the refresh thread's own
    // wlock() acquisition.
    if (refresh_thread_.joinable())
      refresh_thread_.join();

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

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("ProcessExplorer"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Read-only, self-contained top/htop-style system monitor: CPU and memory "
                        "history graphs, per-core meters, and a live process table. All sampling "
                        "happens server-side. Listen for the 'closed' event to detect when the "
                        "user is done."));

  dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      dynamic::make_factory<process_explorer>("wish"_key, "ProcessExplorer"_key));
}

} // namespace bdg::wish
