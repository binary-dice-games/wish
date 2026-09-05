// MIT License © 2026 Binary Dice Games
/// @file dbg.cpp
/// @brief Implementation of the DebuggerFrontend form.
#include "dbg.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/dock_layout_spec.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// bison::dynamic has no initializer-list constructor (see bison_object.hpp);
// these save repeating field-by-field payload construction at each emit()
// call site (git.cpp's payload1/payload2 pattern).
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

// Invokes fn(dynamic&) for each nested-dynamic_ptr entry of the array field
// `field_key` on `parent` (docker/git's *_args array walk).
template <typename Fn>
void for_each_entry(const dynamic& parent, key_t field_key, Fn&& fn) {
  const auto* arr_f = parent.findField<dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return;
  (*arr_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto entry_ptr = f.as<dynamic_ptr>();
    if (!entry_ptr)
      return;
    fn(*entry_ptr);
  });
}

// ── Layout literals ──────────────────────────────────────────────────────────

static constexpr const char* kSourceLayout = R"({
  "type": "Window", "title": "Source", "width": 900, "height": 600, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 6,
          "children": {
            "pid":     { "type": "InputText", "hint": "PID", "width": 90 },
            "attach":  { "type": "Button", "label": "Attach", "width": 70 },
            "detach":  { "type": "Button", "label": "Detach", "width": 70, "enabled": false },
            "pause":   { "type": "Button", "label": "Pause", "width": 70, "enabled": false },
            "resume":  { "type": "Button", "label": "Continue", "width": 85, "enabled": false },
            "into":    { "type": "Button", "label": "Step Into", "width": 90, "enabled": false },
            "over":    { "type": "Button", "label": "Step Over", "width": 90, "enabled": false },
            "out":     { "type": "Button", "label": "Step Out", "width": 90, "enabled": false },
            "state":   { "type": "Label", "text": "detached" }
          }
        },
        "files": {
          "type": "TabBar",
          "id": "##dbg_source_files",
          "height": -1,
          "children": {}
        }
      }
    }
  }
})";

static constexpr const char* kThreadsLayout = R"({
  "type": "Window", "title": "Threads", "width": 500, "height": 300, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "table": {
          "type": "Table", "id": "##threads_table", "columns": 3,
          "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true, "height": -1, "outer_height": -1,
          "children": {
            "col_id":    { "type": "TableColumn", "label": "Id", "flags": "WidthFixed", "init_width": 60, "column_id": 0 },
            "col_state": { "type": "TableColumn", "label": "State", "flags": "WidthFixed", "init_width": 110, "column_id": 1 },
            "col_func":  { "type": "TableColumn", "label": "Current Function", "flags": "WidthStretch", "column_id": 2 }
          }
        }
      }
    }
  }
})";

static constexpr const char* kCallstackLayout = R"({
  "type": "Window", "title": "Call Stack", "width": 500, "height": 300, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "table": {
          "type": "Table", "id": "##callstack_table", "columns": 4,
          "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true, "height": -1, "outer_height": -1,
          "children": {
            "col_idx":  { "type": "TableColumn", "label": "#", "flags": "WidthFixed", "init_width": 36, "column_id": 0 },
            "col_func": { "type": "TableColumn", "label": "Function", "flags": "WidthStretch", "column_id": 1 },
            "col_file": { "type": "TableColumn", "label": "File", "flags": "WidthFixed", "init_width": 130, "column_id": 2 },
            "col_line": { "type": "TableColumn", "label": "Line", "flags": "WidthFixed", "init_width": 60, "column_id": 3 }
          }
        }
      }
    }
  }
})";

static constexpr const char* kWatchLayout = R"({
  "type": "Window", "title": "Watch", "width": 420, "height": 300, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "spacing": 4,
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 6,
          "children": {
            "expr": { "type": "InputText", "hint": "Expression", "width": 220 },
            "add":  { "type": "Button", "label": "Add", "width": 60 }
          }
        },
        "table": {
          "type": "Table", "id": "##watch_table", "columns": 3,
          "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true, "height": -1, "outer_height": -1,
          "children": {
            "col_name": { "type": "TableColumn", "label": "Name", "flags": "WidthFixed", "init_width": 140, "column_id": 0 },
            "col_val":  { "type": "TableColumn", "label": "Value", "flags": "WidthStretch", "column_id": 1 },
            "col_type": { "type": "TableColumn", "label": "Type", "flags": "WidthFixed", "init_width": 100, "column_id": 2 }
          }
        }
      }
    }
  }
})";

static constexpr const char* kBreakpointsLayout = R"({
  "type": "Window", "title": "Breakpoints", "width": 500, "height": 260, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "table": {
          "type": "Table", "id": "##breakpoints_table", "columns": 4,
          "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true, "height": -1, "outer_height": -1,
          "children": {
            "col_file":    { "type": "TableColumn", "label": "File", "flags": "WidthStretch", "column_id": 0 },
            "col_line":    { "type": "TableColumn", "label": "Line", "flags": "WidthFixed", "init_width": 60, "column_id": 1 },
            "col_enabled": { "type": "TableColumn", "label": "Enabled", "flags": "WidthFixed", "init_width": 70, "column_id": 2 },
            "col_actions": { "type": "TableColumn", "label": "", "flags": "WidthFixed", "init_width": 36, "column_id": 3 }
          }
        }
      }
    }
  }
})";

static constexpr const char* kOutputLayout = R"({
  "type": "Window", "title": "Output", "width": 700, "height": 260, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "table": {
          "type": "Table", "id": "##output_table", "columns": 1,
          "flags": "RowBg|ScrollY", "headers": false, "height": -1, "outer_height": -1, "auto_scroll": true,
          "children": {
            "col_text": { "type": "TableColumn", "label": "", "flags": "WidthStretch", "column_id": 0 }
          }
        }
      }
    }
  }
})";

} // namespace

// ── debugger_frontend ────────────────────────────────────────────────────────

debugger_frontend::debugger_frontend(dynamic&& base) : form(std::move(base)) {}

void debugger_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void debugger_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = children;
  parent->refresh_children_order();
}

ui_element_ptr debugger_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void debugger_frontend::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not
  // pointer-derived. Source is the main window (internal_root_key_ itself);
  // the other five windows hang off suffixed keys.
  internal_root_key_ = next_available_key("__dbg_");
  threads_root_key_ = internal_root_key_ + "_threads";
  callstack_root_key_ = internal_root_key_ + "_callstack";
  watch_root_key_ = internal_root_key_ + "_watch";
  breakpoints_root_key_ = internal_root_key_ + "_breakpoints";
  output_root_key_ = internal_root_key_ + "_output";

  build_source_window();
  build_threads_window();
  build_callstack_window();
  build_watch_window();
  build_breakpoints_window();
  build_output_window();

  using namespace dock;
  set_default_dock_layout(layout(split(
      dir::left, 0.62f,
      split(dir::down, 0.7f, area({internal_root_key_}), area({output_root_key_})),
      split(
          dir::down, 0.34f, area({threads_root_key_}),
          split(dir::down, 0.5f, area({callstack_root_key_}), split(dir::down, 0.5f, area({watch_root_key_}), area({breakpoints_root_key_})))))));
}

void debugger_frontend::build_source_window() {
  auto tree = import_json(kSourceLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  source_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.toolbar.pid", [&](const auto& e) { pid_input_ = e; });
  tree.with("vbox.toolbar.state", [&](const auto& e) { run_state_label_ = e; });

  auto bind_click = [&](const std::string& path, std::function<void()> handler) {
    tree.with(path, [&](const auto& e) { click_handlers_[wish_id_of(e)] = std::move(handler); });
  };
  bind_click("vbox.toolbar.attach", [this] {
    emit("attach_requested"_key, payload1("pid"_key, static_cast<uint32_t>(std::atoi(pid_text_.c_str()))));
  });
  bind_click("vbox.toolbar.detach", [this] { emit("detach_requested"_key); });
  bind_click("vbox.toolbar.pause", [this] { emit("pause_requested"_key); });
  bind_click("vbox.toolbar.resume", [this] { emit("resume_requested"_key); });
  bind_click("vbox.toolbar.into", [this] {
    emit("step_requested"_key, payload2("kind"_key, std::string{"into"}, "thread_id"_key, selected_thread_id_));
  });
  bind_click("vbox.toolbar.over", [this] {
    emit("step_requested"_key, payload2("kind"_key, std::string{"over"}, "thread_id"_key, selected_thread_id_));
  });
  bind_click("vbox.toolbar.out", [this] {
    emit("step_requested"_key, payload2("kind"_key, std::string{"out"}, "thread_id"_key, selected_thread_id_));
  });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

void debugger_frontend::build_threads_window() {
  auto tree = import_json(kThreadsLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  threads_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.table", [&](const auto& e) {
    threads_table_ = e;
    threads_table_id_ = wish_id_of(e);
  });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), threads_root_key_);
  sess().top_level_objects[key_t{threads_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{threads_root_key_}] = this;
  (*root_ptr)["__path__"_key] = threads_root_key_;
}

void debugger_frontend::build_callstack_window() {
  auto tree = import_json(kCallstackLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  callstack_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.table", [&](const auto& e) {
    callstack_table_ = e;
    callstack_table_id_ = wish_id_of(e);
  });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), callstack_root_key_);
  sess().top_level_objects[key_t{callstack_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{callstack_root_key_}] = this;
  (*root_ptr)["__path__"_key] = callstack_root_key_;
}

void debugger_frontend::build_watch_window() {
  auto tree = import_json(kWatchLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  watch_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.toolbar.expr", [&](const auto& e) { watch_expr_input_ = e; });
  tree.with("vbox.table", [&](const auto& e) { watch_table_ = e; });

  tree.with("vbox.toolbar.add", [&](const auto& e) {
    click_handlers_[wish_id_of(e)] = [this] {
      if (!watch_expr_text_.empty())
        emit("add_watch_requested"_key, payload1("expr"_key, watch_expr_text_));
    };
  });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), watch_root_key_);
  sess().top_level_objects[key_t{watch_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{watch_root_key_}] = this;
  (*root_ptr)["__path__"_key] = watch_root_key_;
}

void debugger_frontend::build_breakpoints_window() {
  auto tree = import_json(kBreakpointsLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  breakpoints_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.table", [&](const auto& e) { breakpoints_table_ = e; });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), breakpoints_root_key_);
  sess().top_level_objects[key_t{breakpoints_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{breakpoints_root_key_}] = this;
  (*root_ptr)["__path__"_key] = breakpoints_root_key_;
}

void debugger_frontend::build_output_window() {
  auto tree = import_json(kOutputLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  output_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.table", [&](const auto& e) { output_table_ = e; });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), output_root_key_);
  sess().top_level_objects[key_t{output_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{output_root_key_}] = this;
  (*root_ptr)["__path__"_key] = output_root_key_;
}

// ── update_threads / update_callstack ───────────────────────────────────────

dynamic debugger_frontend::do_update_threads(const dynamic& args) {
  rebuild_threads(args);
  return dynamic{};
}

void debugger_frontend::rebuild_threads(const dynamic& args) {
  if (!threads_table_)
    return;
  auto* children_p = threads_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  // Clear previously-added rows (TableColumn children, added once by
  // kThreadsLayout, are left untouched) -- same walk-and-match-on-class
  // approach as git_repo::rebuild_graph_table(), since rows here aren't
  // individually tracked by child_key.
  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    if (f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  thread_row_ids_.clear();

  // Local, freshly-zeroed counter: this table's children map was just fully
  // cleared above, so a 0-based sequence matches dynamic::size()'s "highest
  // numeric key + 1" semantics (see git.cpp's rebuild_graph_table() comment
  // on the same point) -- must not be shared with any other children map.
  size_t row_key = 0;
  for_each_entry(args, "threads"_key, [&](const dynamic& e) {
    uint32_t id = static_cast<uint32_t>(e.as<int32_t>("id"_key));
    std::string state = e.as<std::string>("state"_key);
    std::string current_function = e.as<std::string>("current_function"_key);

    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);

    bool suspended = state == "suspended";
    ui_element_ptr id_cell = make_label(std::to_string(id));
    ui_element_ptr state_cell =
        suspended ? make_label(state, "#9A6700FF", "#D29922FF") : make_label(state, "#1A7F37FF", "#3FB950FF");
    ui_element_ptr func_cell = make_label(current_function);

    set_children_list(row, {id_cell, state_cell, func_cell});
    (*children)[row_key++] = dynamic_ptr{row};
    thread_row_ids_.push_back(id);
  });
}

dynamic debugger_frontend::do_update_callstack(const dynamic& args) {
  uint32_t thread_id = static_cast<uint32_t>(args.as<int32_t>("thread_id"_key));
  // Staleness guard: discard if this snapshot no longer matches the
  // Threads window's current selection (docker's do_update_logs /
  // do_update_inspect container_id/kind+id guard, ported verbatim).
  if (!has_selected_thread_ || thread_id != selected_thread_id_)
    return dynamic{};
  rebuild_callstack(args);
  return dynamic{};
}

void debugger_frontend::rebuild_callstack(const dynamic& args) {
  if (!callstack_table_)
    return;
  auto* children_p = callstack_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    if (f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  frame_row_ids_.clear();

  size_t row_key = 0;
  for_each_entry(args, "frames"_key, [&](const dynamic& e) {
    int32_t index = e.as<int32_t>("index"_key);
    std::string function = e.as<std::string>("function"_key);
    std::string file = e.as<std::string>("file"_key);
    int32_t line = e.as<int32_t>("line"_key);

    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);

    ui_element_ptr idx_cell = make_label(std::to_string(index));
    ui_element_ptr func_cell = make_label(function);
    ui_element_ptr file_cell = make_label(file);
    ui_element_ptr line_cell = make_label(std::to_string(line));

    set_children_list(row, {idx_cell, func_cell, file_cell, line_cell});
    (*children)[row_key++] = dynamic_ptr{row};
    frame_row_ids_.push_back(index);
  });
}

// ── update_watch / update_breakpoints / update_source / append_output ──────

dynamic debugger_frontend::do_update_watch(const dynamic& args) {
  uint32_t frame_id = static_cast<uint32_t>(args.as<int32_t>("frame_id"_key));
  if (!has_selected_frame_ || frame_id != selected_frame_id_)
    return dynamic{};
  rebuild_watch(args);
  return dynamic{};
}

void debugger_frontend::rebuild_watch(const dynamic& args) {
  if (!watch_table_)
    return;
  auto* children_p = watch_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    if (f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);

  size_t row_key = 0;
  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);
    ui_element_ptr name_cell = make_label(e.as<std::string>("name"_key));
    ui_element_ptr value_cell = make_label(e.as<std::string>("value"_key));
    ui_element_ptr type_cell = make_label(e.as<std::string>("type"_key));
    set_children_list(row, {name_cell, value_cell, type_cell});
    (*children)[row_key++] = dynamic_ptr{row};
  });
}

dynamic debugger_frontend::do_update_breakpoints(const dynamic& args) {
  rebuild_breakpoints(args);
  return dynamic{};
}

void debugger_frontend::rebuild_breakpoints(const dynamic& args) {
  if (!breakpoints_table_)
    return;
  auto* children_p = breakpoints_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    if (f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);

  size_t row_key = 0;
  for_each_entry(args, "breakpoints"_key, [&](const dynamic& e) {
    std::string file = e.as<std::string>("file"_key);
    int32_t line = e.as<int32_t>("line"_key);
    bool enabled = e.as<bool>("enabled"_key);

    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);
    ui_element_ptr file_cell = make_label(file);
    ui_element_ptr line_cell = make_label(std::to_string(line));

    ui_element_ptr enabled_cell = ui_element_ptr::create("wish"_key, "Checkbox"_key);
    enabled_cell["label"_key] = std::string{};
    enabled_cell["value"_key] = enabled;
    assign_id(enabled_cell);
    click_handlers_[wish_id_of(enabled_cell)] = [this, file, line, enabled] {
      emit("toggle_breakpoint_requested"_key, payload2("path"_key, file, "line"_key, line));
    };

    ui_element_ptr actions_cell = ui_element_ptr::create("wish"_key, "MenuButton"_key);
    actions_cell["label"_key] = std::string{"..."};
    assign_id(actions_cell);
    ui_element_ptr remove_item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
    remove_item["label"_key] = std::string{"Remove"};
    assign_id(remove_item);
    click_handlers_[wish_id_of(remove_item)] = [this, file, line] {
      emit("toggle_breakpoint_requested"_key, payload2("path"_key, file, "line"_key, line));
    };
    set_children_list(actions_cell, {remove_item});

    set_children_list(row, {file_cell, line_cell, enabled_cell, actions_cell});
    (*children)[row_key++] = dynamic_ptr{row};
  });
}

dynamic debugger_frontend::do_update_source(const dynamic& /*args*/) {
  // Tab management (ensuring a TextEditor tab exists for `path`, setting its
  // breakpoint_lines/current_line fields) is deferred to PLAN.md Step 5
  // ("Source window + breakpoints end-to-end") -- Step 3's scope is the
  // scaffold plus Threads/Call Stack against a fake backend.
  return dynamic{};
}

dynamic debugger_frontend::do_append_output(const dynamic& args) {
  if (!output_table_)
    return dynamic{};
  auto* children_p = output_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return dynamic{};
  auto& children = *children_p;

  std::string text = args.as<std::string>("text"_key);
  std::string level = args.as<std::string>("level"_key);
  const char* light = "#656D76FF";
  const char* dark = "#8B949EFF";
  if (level == "warn") {
    light = "#9A6700FF";
    dark = "#D29922FF";
  } else if (level == "error") {
    light = "#CF222EFF";
    dark = "#F85149FF";
  }

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);
  ui_element_ptr text_cell = make_label("[" + level + "] " + text, light, dark);
  set_children_list(row, {text_cell});

  size_t child_key = next_output_child_key_++;
  (*children)[child_key] = dynamic_ptr{row};
  output_row_keys_.push_back(wish_id_of(row));

  if (output_row_keys_.size() > kMaxOutputRows) {
    // Evict oldest: find and erase its TableRow child (docker's Console
    // FIFO-eviction pattern, kMaxConsoleRows). Rows aren't tracked by
    // numeric child_key here, so find the oldest still-live row by its
    // recorded __wish_id and erase whichever numeric slot holds it.
    key_t oldest_id = output_row_keys_.front();
    output_row_keys_.erase(output_row_keys_.begin());
    key_t erase_key{};
    bool found = false;
    children->forEach([&](key_t k, const field& f) {
      if (found || !f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      if (f.as<dynamic_ptr>()->as<key_t>("__wish_id"_key) == oldest_id) {
        erase_key = k;
        found = true;
      }
    });
    if (found)
      children->erase(erase_key.id);
  }

  return dynamic{};
}

dynamic debugger_frontend::do_set_run_state(const dynamic& args) {
  std::string state = args.as<std::string>("state"_key);
  if (run_state_label_)
    run_state_label_["text"_key] = state;
  return dynamic{};
}

// ── Events ───────────────────────────────────────────────────────────────────

void debugger_frontend::close_all() {
  emit("closed"_key);
  remove_objects_at(threads_root_key_);
  remove_objects_at(callstack_root_key_);
  remove_objects_at(watch_root_key_);
  remove_objects_at(breakpoints_root_key_);
  remove_objects_at(output_root_key_);
  remove_internal_objects();
}

void debugger_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  if (event == "closed"_key &&
      (id == source_window_id_ || id == threads_window_id_ || id == callstack_window_id_ || id == watch_window_id_ ||
       id == breakpoints_window_id_ || id == output_window_id_)) {
    close_all();
    return;
  }

  if (event == "clicked"_key) {
    auto it = click_handlers_.find(id);
    if (it != click_handlers_.end())
      it->second();
    return;
  }

  if (event == "changed"_key) {
    if (pid_input_ && id == wish_id_of(pid_input_)) {
      pid_text_ = payload.as<std::string>("value"_key);
      return;
    }
    if (watch_expr_input_ && id == wish_id_of(watch_expr_input_)) {
      watch_expr_text_ = payload.as<std::string>("value"_key);
      return;
    }
    // Checkbox rows in the Breakpoints table also raise "changed"; their
    // handler is registered via click_handlers_ above (keyed the same way
    // regardless of event name, since the payload isn't needed there).
    auto it = click_handlers_.find(id);
    if (it != click_handlers_.end())
      it->second();
    return;
  }

  if (threads_table_ && id == threads_table_id_ && (event == "row_selected"_key || event == "row_activated"_key)) {
    int32_t index = payload.get_as<int32_t>("index"_key, -1);
    if (index < 0 || static_cast<size_t>(index) >= thread_row_ids_.size())
      return;
    selected_thread_id_ = thread_row_ids_[static_cast<size_t>(index)];
    has_selected_thread_ = true;
    emit("select_thread_requested"_key, payload1("thread_id"_key, static_cast<int32_t>(selected_thread_id_)));
    return;
  }

  if (callstack_table_ && id == callstack_table_id_ &&
      (event == "row_selected"_key || event == "row_activated"_key)) {
    int32_t index = payload.get_as<int32_t>("index"_key, -1);
    if (index < 0 || static_cast<size_t>(index) >= frame_row_ids_.size())
      return;
    int32_t frame_id = frame_row_ids_[static_cast<size_t>(index)];
    selected_frame_id_ = static_cast<uint32_t>(frame_id);
    has_selected_frame_ = true;
    emit("select_frame_requested"_key, payload1("frame_id"_key, static_cast<int32_t>(selected_frame_id_)));
    return;
  }
}

// ── Registration ─────────────────────────────────────────────────────────────

void register_dbg() {
  auto proto = dynamic_ptr{"DebuggerFrontend"_key, {}};

  auto add_method = [&](key_t name, dynamic (debugger_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                        return (static_cast<debugger_frontend&>(self).*fn)(args);
                      }});
  };
  add_method("update_threads"_key, &debugger_frontend::do_update_threads);
  add_method("update_callstack"_key, &debugger_frontend::do_update_callstack);
  add_method("update_watch"_key, &debugger_frontend::do_update_watch);
  add_method("update_breakpoints"_key, &debugger_frontend::do_update_breakpoints);
  add_method("update_source"_key, &debugger_frontend::do_update_source);
  add_method("append_output"_key, &debugger_frontend::do_append_output);
  add_method("set_run_state"_key, &debugger_frontend::do_set_run_state);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DebuggerFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Visual-Studio-style debugger GUI frontend. All process-attach / symbol-resolution / "
      "breakpoint-patching happens client-side; this form only renders whatever snapshot it was last "
      "given. Listen for the 'closed' event to detect when the user is done, and the '*_requested' "
      "events to react to user actions -- see dbg.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<debugger_frontend>("wish"_key, "DebuggerFrontend"_key));
}

} // namespace bdg::wish
