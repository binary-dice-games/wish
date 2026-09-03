// MIT License © 2026 Binary Dice Games
/// @file docker.cpp
/// @brief Implementation of the DockerFrontend form.
///
/// A close port of modules/bdg/desktop/git/server/git.cpp: inline JSON
/// window layouts + import_json(), C++-built table rows, a per-row `...`
/// MenuButton, show_confirm() via a privately-instantiated MessageBox, and
/// an id -> handler dispatch map rebuilt on every update_*. The four list
/// windows (Containers / Images / Volumes / Networks) share one
/// build_list_window() / clear_list_rows() / add_list_row() path.
#include "docker.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/forms/message_box.hpp>

#include <algorithm>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// bison::dynamic has no initializer-list constructor -- event payloads are
// built field-by-field (git.cpp's payload1/payload2).
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

// "#RRGGBBAA" light/dark pairs -- GitHub Primer tokens, the git.cpp theme_hex
// pattern. A single text_color tuned for one theme reads poorly on the other.
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kIdleLight = "#656D76FF";
constexpr const char* kIdleDark = "#8B949EFF";
constexpr const char* kWarnLight = "#9A6700FF";
constexpr const char* kWarnDark = "#D29922FF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";

// Container `docker ps` .State -> (light, dark) status-text colour.
std::pair<const char*, const char*> state_colour(const std::string& state) {
  if (state == "running")
    return {kOkLight, kOkDark};
  if (state == "paused" || state == "restarting")
    return {kWarnLight, kWarnDark};
  if (state == "dead")
    return {kBadLight, kBadDark};
  return {kIdleLight, kIdleDark}; // exited, created, removing, ...
}

// ── Window layouts ─────────────────────────────────────────────────────────
//
// Each list table carries both "height": -1 (stretch row in `vbox`) and
// "outer_height": -1 (fill that region) -- the load-bearing pair documented
// in git.cpp / tail.cpp. `vbox` is each Window's sole direct child so it
// already fills the body (the `top` module's kLayout shape), no hint needed.
//
// docker_mock.json / docker_mock.html (this directory) mirror these layouts
// as a single tabbed window -- the mockup validated in the `editor` tool
// before implementation. Keep them roughly in sync when changing columns.

static constexpr const char* kContainersLayout = R"json({
  "type": "Window", "title": "Containers", "width": 940, "height": 500,
  "pos_x": 0, "pos_y": 0, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "btn_prune":   { "type": "Button", "label": "Prune stopped...", "width": 130 },
      "filter":      { "type": "InputText", "hint": "Filter by name / image", "width": 240 },
      "state":       { "type": "Combo", "items": "All\nRunning\nStopped", "value": 0, "width": 110 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##containers_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":    { "type": "TableColumn", "label": "Name",    "flags": "WidthFixed", "init_width": 150, "column_id": 0 },
        "col_image":   { "type": "TableColumn", "label": "Image",   "flags": "WidthStretch",                    "column_id": 1 },
        "col_status":  { "type": "TableColumn", "label": "Status",  "flags": "WidthFixed", "init_width": 175, "column_id": 2 },
        "col_ports":   { "type": "TableColumn", "label": "Ports",   "flags": "WidthFixed", "init_width": 160, "column_id": 3 },
        "col_created": { "type": "TableColumn", "label": "Created", "flags": "WidthFixed", "init_width": 110, "column_id": 4 },
        "col_actions": { "type": "TableColumn", "label": "",        "flags": "WidthFixed", "init_width": 40,  "column_id": 5 }
      }
    }
  } } }
})json";

static constexpr const char* kImagesLayout = R"json({
  "type": "Window", "title": "Images", "width": 820, "height": 420,
  "pos_x": 948, "pos_y": 0, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "pull_ref":    { "type": "InputText", "hint": "repo:tag to pull", "width": 220 },
      "btn_pull":    { "type": "Button", "label": "Pull", "width": 70 },
      "btn_prune":   { "type": "Button", "label": "Prune dangling...", "width": 140 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##images_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_repo":    { "type": "TableColumn", "label": "Repository", "flags": "WidthStretch",                    "column_id": 0 },
        "col_tag":     { "type": "TableColumn", "label": "Tag",        "flags": "WidthFixed", "init_width": 110, "column_id": 1 },
        "col_id":      { "type": "TableColumn", "label": "Image ID",   "flags": "WidthFixed", "init_width": 140, "column_id": 2 },
        "col_created": { "type": "TableColumn", "label": "Created",    "flags": "WidthFixed", "init_width": 120, "column_id": 3 },
        "col_size":    { "type": "TableColumn", "label": "Size",       "flags": "WidthFixed", "init_width": 90,  "column_id": 4 },
        "col_actions": { "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 5 }
      }
    }
  } } }
})json";

static constexpr const char* kVolumesLayout = R"json({
  "type": "Window", "title": "Volumes", "width": 720, "height": 300,
  "pos_x": 0, "pos_y": 508, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "vol_name":    { "type": "InputText", "hint": "new volume name", "width": 200 },
      "btn_create":  { "type": "Button", "label": "Create", "width": 80 },
      "btn_prune":   { "type": "Button", "label": "Prune...", "width": 90 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##volumes_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":   { "type": "TableColumn", "label": "Name",       "flags": "WidthFixed", "init_width": 220, "column_id": 0 },
        "col_driver": { "type": "TableColumn", "label": "Driver",     "flags": "WidthFixed", "init_width": 90,  "column_id": 1 },
        "col_mount":  { "type": "TableColumn", "label": "Mountpoint", "flags": "WidthStretch",                    "column_id": 2 },
        "col_actions":{ "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 3 }
      }
    }
  } } }
})json";

static constexpr const char* kNetworksLayout = R"json({
  "type": "Window", "title": "Networks", "width": 720, "height": 300,
  "pos_x": 728, "pos_y": 508, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 },
      "btn_prune":   { "type": "Button", "label": "Prune...", "width": 90 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##networks_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":   { "type": "TableColumn", "label": "Name",       "flags": "WidthFixed", "init_width": 200, "column_id": 0 },
        "col_driver": { "type": "TableColumn", "label": "Driver",     "flags": "WidthFixed", "init_width": 100, "column_id": 1 },
        "col_scope":  { "type": "TableColumn", "label": "Scope",      "flags": "WidthFixed", "init_width": 100, "column_id": 2 },
        "col_id":     { "type": "TableColumn", "label": "Network ID", "flags": "WidthStretch",                    "column_id": 3 },
        "col_actions":{ "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

// Logs / Inspect are a toolbar + a single-column scrolling Table of Label
// lines (git.cpp's diff-viewer shape -- no session-sandbox file, no
// InputText max_length cap). "auto_scroll": true on the Logs table so it
// follows the newest line as `docker logs` output arrives.

static constexpr const char* kLogsLayout = R"json({
  "type": "Window", "title": "Logs", "width": 900, "height": 420,
  "pos_x": 948, "pos_y": 440, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":     { "type": "Label", "text": "(no container selected)" },
      "spring":     { "type": "Spring" },
      "follow":     { "type": "Checkbox", "label": "Follow", "value": false },
      "lines":      { "type": "InputInt", "label": "Lines", "value": 500, "step": 100, "width": 130 },
      "btn_refresh":{ "type": "Button", "label": "Refresh", "width": 90 }
    } },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##logs_table", "columns": 1,
      "flags": "ScrollY", "headers": false,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": { "col_line": { "type": "TableColumn", "flags": "WidthStretch", "column_id": 0 } }
    }
  } } }
})json";

static constexpr const char* kInspectLayout = R"json({
  "type": "Window", "title": "Inspect", "width": 820, "height": 420,
  "pos_x": 948, "pos_y": 440, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":     { "type": "Label", "text": "(nothing selected)" },
      "spring":     { "type": "Spring" },
      "btn_refresh":{ "type": "Button", "label": "Refresh", "width": 90 }
    } },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##inspect_table", "columns": 1,
      "flags": "ScrollY", "headers": false,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": { "col_line": { "type": "TableColumn", "flags": "WidthStretch", "column_id": 0 } }
    }
  } } }
})json";

// The Console window: a FIFO-capped `Table` tracing every `docker` command
// the client ran (git's "Log" window, renamed to avoid clashing with the
// container Logs window). "auto_scroll": true so it follows the newest row.

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 940, "height": 240,
  "pos_x": 0, "pos_y": 760, "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##docker_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed", "init_width": 44,  "column_id": 0 },
        "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed", "init_width": 360, "column_id": 1 },
        "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed", "init_width": 50,  "column_id": 2 },
        "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
      }
    }
  } } }
})json";

} // namespace

// ── docker_frontend ────────────────────────────────────────────────────────

docker_frontend::docker_frontend(dynamic&& base) : form(std::move(base)) {}

void docker_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void docker_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

ui_element_ptr docker_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void docker_frontend::on_init() {
  internal_root_key_ = next_available_key("__docker_");

  auto* title_f = findField<std::string>("title"_key);
  title_ = title_f ? *title_f : std::string{"Docker"};

  // Containers is the main root -- form::init() registers internal_root_key_
  // as this form's top-level object automatically. The other three windows
  // are registered by hand inside build_list_window() (git.cpp's
  // build_*_window() pattern).
  build_list_window(containers_, kContainersLayout, internal_root_key_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) {
      filter_input_ = e;
      filter_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.state", [&](const auto& e) { state_combo_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
    tree.with("vbox.toolbar.btn_prune", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        show_confirm("Remove all stopped containers (docker container prune)?", [this] {
          emit("prune_requested"_key, payload1("scope"_key, std::string{"containers"}));
        });
      };
    });
  });

  build_list_window(images_, kImagesLayout, internal_root_key_ + "_images", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.pull_ref", [&](const auto& e) {
      pull_ref_input_ = e;
      pull_ref_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
    tree.with("vbox.toolbar.btn_pull", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        if (pull_ref_text_.empty())
          return;
        emit("pull_image_requested"_key, payload1("ref"_key, pull_ref_text_));
        pull_ref_text_.clear();
        if (pull_ref_input_)
          pull_ref_input_["value"_key] = std::string{};
      };
    });
    tree.with("vbox.toolbar.btn_prune", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        show_confirm("Remove all dangling images (docker image prune)?", [this] {
          emit("prune_requested"_key, payload1("scope"_key, std::string{"images"}));
        });
      };
    });
  });

  build_list_window(volumes_, kVolumesLayout, internal_root_key_ + "_volumes", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.vol_name", [&](const auto& e) {
      volume_name_input_ = e;
      volume_name_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
    tree.with("vbox.toolbar.btn_create", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        if (volume_name_text_.empty())
          return;
        emit("create_volume_requested"_key, payload1("name"_key, volume_name_text_));
        volume_name_text_.clear();
        if (volume_name_input_)
          volume_name_input_["value"_key] = std::string{};
      };
    });
    tree.with("vbox.toolbar.btn_prune", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        show_confirm("Remove all unused local volumes (docker volume prune)?", [this] {
          emit("prune_requested"_key, payload1("scope"_key, std::string{"volumes"}));
        });
      };
    });
  });

  build_list_window(networks_, kNetworksLayout, internal_root_key_ + "_networks", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
    tree.with("vbox.toolbar.btn_prune", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        show_confirm("Remove all unused networks (docker network prune)?", [this] {
          emit("prune_requested"_key, payload1("scope"_key, std::string{"networks"}));
        });
      };
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

  inspect_root_key_ = internal_root_key_ + "_inspect";
  build_text_window(inspect_root_key_, kInspectLayout, inspect_window_id_, inspect_table_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { inspect_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_inspect_request(); };
    });
  });

  console_root_key_ = internal_root_key_ + "_console";
  build_text_window(console_root_key_, kConsoleLayout, console_window_id_, console_table_, [](ui_tree&) {});

  // Initial population is triggered client-side (run_docker() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event (git's documented initial-load-race fix).
}

void docker_frontend::build_text_window(
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

void docker_frontend::set_text_lines(
    const ui_element_ptr& table, std::vector<key_t>& line_ids, size_t& next_key, const std::string& text) {
  if (!table)
    return;
  auto* children_p = table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  // Clear prior line rows + their ctx().objects entries (git's diff-rebuild).
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

void docker_frontend::emit_logs_request() {
  if (open_logs_id_.empty())
    return;
  dynamic p;
  p["id"_key] = open_logs_id_;
  p["follow"_key] = logs_follow_;
  p["lines"_key] = logs_lines_;
  emit("logs_requested"_key, std::move(p));
}

void docker_frontend::emit_inspect_request() {
  if (open_inspect_id_.empty())
    return;
  emit("inspect_requested"_key, payload2("kind"_key, open_inspect_kind_, "id"_key, open_inspect_id_));
}

void docker_frontend::build_list_window(
    list_window& lw, const char* layout_json, const std::string& root_key,
    const std::function<void(ui_tree&)>& wire_toolbar) {
  lw.root_key = root_key;
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

// ── Confirmation modal (git_repo::show_confirm() port) ──────────────────────

void docker_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
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

void docker_frontend::clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key) {
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

void docker_frontend::add_list_row(
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
    menu_action_targets_[wish_id_of(mi)] = row_action{meta.scope, meta.key, it.action};
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

void docker_frontend::set_status(list_window& lw, const std::string& text, bool ok) {
  if (!lw.status_label)
    return;
  lw.status_label["text"_key] = text;
  lw.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  lw.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void docker_frontend::rebuild_containers(const dynamic& args) {
  clear_list_rows(containers_, container_rows_, next_container_key_);

  int running = 0, total = 0;
  for_each_entry(args, "containers"_key, [&](const dynamic& e) {
    const std::string state = e.as<std::string>("state"_key);
    auto [cl, cd] = state_colour(state);

    list_row meta;
    meta.scope = "container";
    meta.key = e.as<std::string>("id"_key);
    meta.name = e.as<std::string>("name"_key);
    meta.extra = e.as<std::string>("image"_key);
    meta.state = state;

    std::vector<ui_element_ptr> cells = {
        make_label(meta.name),
        make_label(meta.extra),
        make_label(e.as<std::string>("status"_key), cl, cd),
        make_label(e.as<std::string>("ports"_key)),
        make_label(e.as<std::string>("created"_key), kIdleLight, kIdleDark),
    };

    std::vector<menu_spec> items;
    if (state == "running")
      items = {{"Stop", "stop", true}, {"Restart", "restart", false}, {"Pause", "pause", false}, {"Kill", "kill", true}};
    else if (state == "paused")
      items = {{"Unpause", "unpause", false}, {"Stop", "stop", true}, {"Kill", "kill", true}};
    else
      items = {{"Start", "start", false}};
    items.push_back({});
    items.push_back({"Logs", "logs", false});
    items.push_back({"Inspect", "inspect", false});
    items.push_back({});
    items.push_back({"Remove", "remove", true});

    add_list_row(containers_, container_rows_, next_container_key_, std::move(meta), cells, items);
    ++total;
    if (state == "running")
      ++running;
  });

  if (containers_.table)
    containers_.table->refresh_children_order();
  apply_container_filter();

  set_status(
      containers_,
      std::to_string(total) + (total == 1 ? " container (" : " containers (") + std::to_string(running) + " running)",
      true);
}

void docker_frontend::rebuild_images(const dynamic& args) {
  clear_list_rows(images_, image_rows_, next_image_key_);

  int total = 0;
  for_each_entry(args, "images"_key, [&](const dynamic& e) {
    const std::string repo = e.as<std::string>("repository"_key);
    const std::string tag = e.as<std::string>("tag"_key);
    const bool dangling = repo == "<none>" || repo.empty();
    const char* dl = dangling ? kIdleLight : nullptr;
    const char* dd = dangling ? kIdleDark : nullptr;

    list_row meta;
    meta.scope = "image";
    meta.key = e.as<std::string>("id"_key);
    meta.name = dangling ? meta.key.substr(0, 19) : repo + ":" + tag;

    std::vector<ui_element_ptr> cells = {
        make_label(repo, dl, dd),
        make_label(tag, dl, dd),
        make_label(meta.key.substr(0, 19), kIdleLight, kIdleDark),
        make_label(e.as<std::string>("created"_key), kIdleLight, kIdleDark),
        make_label(e.as<std::string>("size"_key)),
    };
    std::vector<menu_spec> items = {{"Run", "run", false}, {"Inspect", "inspect", false}, {}, {"Remove", "remove", true}};
    add_list_row(images_, image_rows_, next_image_key_, std::move(meta), cells, items);
    ++total;
  });

  if (images_.table)
    images_.table->refresh_children_order();
  set_status(images_, std::to_string(total) + (total == 1 ? " image" : " images"), true);
}

void docker_frontend::rebuild_volumes(const dynamic& args) {
  clear_list_rows(volumes_, volume_rows_, next_volume_key_);

  int total = 0;
  for_each_entry(args, "volumes"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "volume";
    meta.key = e.as<std::string>("name"_key);
    meta.name = meta.key;

    std::vector<ui_element_ptr> cells = {
        make_label(meta.key),
        make_label(e.as<std::string>("driver"_key)),
        make_label(e.as<std::string>("mountpoint"_key), kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {{"Inspect", "inspect", false}, {}, {"Remove", "remove", true}};
    add_list_row(volumes_, volume_rows_, next_volume_key_, std::move(meta), cells, items);
    ++total;
  });

  if (volumes_.table)
    volumes_.table->refresh_children_order();
  set_status(volumes_, std::to_string(total) + (total == 1 ? " volume" : " volumes"), true);
}

void docker_frontend::rebuild_networks(const dynamic& args) {
  clear_list_rows(networks_, network_rows_, next_network_key_);

  int total = 0;
  for_each_entry(args, "networks"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "network";
    meta.key = e.as<std::string>("id"_key);
    meta.name = e.as<std::string>("name"_key);
    const bool builtin = meta.name == "bridge" || meta.name == "host" || meta.name == "none";

    std::vector<ui_element_ptr> cells = {
        make_label(meta.name),
        make_label(e.as<std::string>("driver"_key)),
        make_label(e.as<std::string>("scope"_key)),
        make_label(meta.key.substr(0, 12), kIdleLight, kIdleDark),
    };
    // The three built-in networks can't be removed -- offer Inspect only.
    std::vector<menu_spec> items = builtin
        ? std::vector<menu_spec>{{"Inspect", "inspect", false}}
        : std::vector<menu_spec>{{"Inspect", "inspect", false}, {}, {"Remove", "remove", true}};
    add_list_row(networks_, network_rows_, next_network_key_, std::move(meta), cells, items);
    ++total;
  });

  if (networks_.table)
    networks_.table->refresh_children_order();
  set_status(networks_, std::to_string(total) + (total == 1 ? " network" : " networks"), true);
}

void docker_frontend::apply_container_filter() {
  std::string needle = filter_text_;
  std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return std::tolower(ch); });

  for (auto& r : container_rows_) {
    if (!r.row)
      continue;
    bool show = true;
    if (state_filter_ == 1)
      show = r.state == "running";
    else if (state_filter_ == 2)
      show = r.state != "running";
    if (show && !needle.empty()) {
      std::string hay = r.name + " " + r.extra;
      std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char ch) { return std::tolower(ch); });
      show = hay.find(needle) != std::string::npos;
    }
    r.row["visible"_key] = show;
  }
}

// ── RMI methods ────────────────────────────────────────────────────────────

dynamic docker_frontend::do_update_containers(const dynamic& args) {
  rebuild_containers(args);
  return dynamic{};
}
dynamic docker_frontend::do_update_images(const dynamic& args) {
  rebuild_images(args);
  return dynamic{};
}
dynamic docker_frontend::do_update_volumes(const dynamic& args) {
  rebuild_volumes(args);
  return dynamic{};
}
dynamic docker_frontend::do_update_networks(const dynamic& args) {
  rebuild_networks(args);
  return dynamic{};
}

dynamic docker_frontend::do_update_logs(const dynamic& args) {
  if (args.as<std::string>("container_id"_key) != open_logs_id_)
    return dynamic{}; // stale response for a container the user navigated away from.
  if (logs_target_label_)
    logs_target_label_["text"_key] = args.as<std::string>("title"_key);
  set_text_lines(logs_table_, logs_line_ids_, next_logs_line_key_, args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic docker_frontend::do_update_inspect(const dynamic& args) {
  if (args.as<std::string>("target_id"_key) != open_inspect_id_)
    return dynamic{};
  if (inspect_target_label_)
    inspect_target_label_["text"_key] = args.as<std::string>("title"_key);
  set_text_lines(inspect_table_, inspect_line_ids_, next_inspect_line_key_, args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic docker_frontend::do_command_result(const dynamic& args) {
  const std::string command = args.as<std::string>("command"_key);
  const bool ok = args.as<bool>("ok"_key);
  const std::string output = args.as<std::string>("output"_key);
  const std::string scope =
      args.findField<std::string>("scope"_key) ? args.as<std::string>("scope"_key) : std::string{"containers"};
  list_window* lw = scope == "images" ? &images_
      : scope == "volumes"            ? &volumes_
      : scope == "networks"           ? &networks_
                                      : &containers_;
  if (ok)
    set_status(*lw, command + ": OK", true);
  else
    set_status(*lw, command + " failed: " + (output.empty() ? "unknown error" : output), false);
  return dynamic{};
}

// ── Console window (client `docker` subprocess trace) ──────────────────────

void docker_frontend::append_console_row(
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
  // via MenuItem.copy_text -- the renderer copies it straight to the OS
  // clipboard on click) and "Clear Console" (every row). git's Log window.
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

void docker_frontend::erase_console_row_objects(const console_row_entry& entry) {
  for (auto id : entry.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

void docker_frontend::clear_console_rows() {
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

dynamic docker_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      args.as<std::string>("command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      args.as<std::string>("output"_key));
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void docker_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == containers_.window_id || id == images_.window_id || id == volumes_.window_id ||
       id == networks_.window_id || id == logs_window_id_ || id == inspect_window_id_ ||
       id == console_window_id_)) {
    emit("closed"_key);
    remove_objects_at(images_.root_key);
    remove_objects_at(volumes_.root_key);
    remove_objects_at(networks_.root_key);
    remove_objects_at(logs_root_key_);
    remove_objects_at(inspect_root_key_);
    remove_objects_at(console_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    if (id == filter_input_id_) {
      filter_text_ = payload.as<std::string>("value"_key);
      apply_container_filter();
    } else if (id == state_combo_id_) {
      state_filter_ = payload.as<int32_t>("value"_key);
      apply_container_filter();
    } else if (id == pull_ref_input_id_) {
      pull_ref_text_ = payload.as<std::string>("value"_key);
    } else if (id == volume_name_input_id_) {
      volume_name_text_ = payload.as<std::string>("value"_key);
    } else if (id == logs_follow_id_) {
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

  // Look up the row's display name once (used for the Logs/Inspect target
  // labels and the confirm message).
  std::string display_name = target.key;
  for (auto* rows : {&container_rows_, &image_rows_, &volume_rows_, &network_rows_}) {
    for (auto& r : *rows)
      if (r.key == target.key && r.scope == target.scope && !r.name.empty()) {
        display_name = r.name;
        break;
      }
  }

  if (target.action == "logs") {
    open_logs_id_ = target.key;
    open_logs_name_ = display_name;
    if (logs_target_label_)
      logs_target_label_["text"_key] = display_name;
    emit_logs_request();
    return;
  }
  if (target.action == "inspect") {
    open_inspect_id_ = target.key;
    open_inspect_kind_ = target.scope;
    open_inspect_name_ = display_name;
    if (inspect_target_label_)
      inspect_target_label_["text"_key] = target.scope + ": " + display_name;
    emit_inspect_request();
    return;
  }

  const key_t event_name = target.scope == "container" ? "container_action_requested"_key
      : target.scope == "image"                        ? "image_action_requested"_key
      : target.scope == "volume"                       ? "volume_action_requested"_key
                                                       : "network_action_requested"_key;
  const key_t id_field = target.scope == "volume" ? "name"_key : "id"_key;
  auto fire = [this, event_name, id_field, target] {
    emit(event_name, payload2(id_field, target.key, "action"_key, target.action));
  };

  const bool destructive = target.action == "stop" || target.action == "kill" || target.action == "remove";
  if (!destructive) {
    fire();
    return;
  }

  std::string verb = target.action == "stop" ? "Stop" : target.action == "kill" ? "Kill" : "Remove";
  const char* noun = target.scope == "container" ? "container"
      : target.scope == "image"                  ? "image"
      : target.scope == "volume"                 ? "volume"
                                                 : "network";
  show_confirm(verb + " " + noun + " '" + display_name + "'?", fire);
}

// ── Registration ───────────────────────────────────────────────────────────

void register_docker() {
  auto proto = dynamic_ptr{"DockerFrontend"_key, {}};

  proto->addField("title"_key, field{std::string{"Docker"}});

  auto add_method = [&](key_t name, dynamic (docker_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<docker_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_containers"_key, &docker_frontend::do_update_containers);
  add_method("update_images"_key, &docker_frontend::do_update_images);
  add_method("update_volumes"_key, &docker_frontend::do_update_volumes);
  add_method("update_networks"_key, &docker_frontend::do_update_networks);
  add_method("update_logs"_key, &docker_frontend::do_update_logs);
  add_method("update_inspect"_key, &docker_frontend::do_update_inspect);
  add_method("command_result"_key, &docker_frontend::do_command_result);
  add_method("append_command_log"_key, &docker_frontend::do_append_command_log);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockerFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Docker Desktop-style GUI frontend for the local `docker` CLI. All `docker` invocation happens "
      "client-side; this form only renders whatever snapshot it was last given. Listen for the 'closed' "
      "event to detect when the user is done, and the '*_requested' events to react to user actions -- "
      "see docker.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<docker_frontend>("wish"_key, "DockerFrontend"_key));
}

} // namespace bdg::wish
