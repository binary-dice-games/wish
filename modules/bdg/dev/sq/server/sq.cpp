// MIT License © 2026 Binary Dice Games
/// @file sq.cpp
/// @brief Implementation of the SqFrontend form.
///
/// A port of the docker module's server form (inline JSON window layouts +
/// import_json(), C++-built table rows, MessageBox confirm, an id -> handler
/// dispatch map) to a query tool: the Results table's columns are built at
/// runtime per result, and the Navigator is a TreeNode hierarchy.
#include "sq.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/dock_layout_spec.hpp>
#include <ui/forms/message_box.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

template <typename T>
dynamic payload1(key_t k, T v) {
  dynamic d;
  d[k] = std::move(v);
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

// String at @p field_key of @p d, or "" if absent / another type.
std::string str_of(const dynamic& d, key_t field_key) {
  const auto* f = d.findField<std::string>(field_key);
  return f ? *f : std::string{};
}

// "#RRGGBBAA" light/dark pairs -- GitHub Primer tokens, as in docker.cpp.
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kIdleLight = "#656D76FF";
constexpr const char* kIdleDark = "#8B949EFF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";
constexpr const char* kKeyLight = "#9A6700FF";
constexpr const char* kKeyDark = "#D29922FF";

constexpr size_t kMaxCellChars = 300;
constexpr int32_t kRowLimits[] = {100, 500, 1000, 5000};

// ── Window layouts ─────────────────────────────────────────────────────────
//
// As in docker.cpp: no pos_x/pos_y (windows dock into the ambient dockspace),
// "width"/"height" are the floating size, and every table carries the
// load-bearing "height": -1 + "outer_height": -1 pair.

static constexpr const char* kEditorLayout = R"json({
  "type": "Window", "title": "SQL Editor", "width": 900, "height": 300,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_run":  { "type": "Button", "label": "Run", "width": 70 },
      "conn":     { "type": "Combo", "items": "(no connection)", "value": 0, "width": 220 },
      "rows":     { "type": "Combo", "items": "100 rows\n500 rows\n1000 rows\n5000 rows", "value": 1, "width": 110 },
      "btn_refresh": { "type": "Button", "label": "Refresh", "width": 80 }
    } },
    "status": { "type": "Label", "text": "Read-only queries only: SELECT / WITH / VALUES / SHOW / DESCRIBE / EXPLAIN." },
    "sql": { "type": "TextEditor", "language": "sql", "file_path": "", "width": -1, "height": -1 }
  } } }
})json";

static constexpr const char* kResultsLayout = R"json({
  "type": "Window", "title": "Results", "width": 900, "height": 360,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "export": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_export": { "type": "Button", "label": "Export CSV", "width": 100 },
      "path":      { "type": "InputText", "hint": "CSV file to write, e.g. /tmp/result.csv", "width": 380 },
      "overwrite": { "type": "Checkbox", "label": "Overwrite", "value": false }
    } },
    "status": { "type": "Label", "text": "Run a query to see its results." },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##sq_results_0", "columns": 1,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false, "children": {}
    }
  } } }
})json";

static constexpr const char* kConnectionsLayout = R"json({
  "type": "Window", "title": "Connections", "width": 760, "height": 340,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "new_row1": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "driver": { "type": "Combo", "items": "(auto-detect)", "value": 0, "width": 150 },
      "handle": { "type": "InputText", "hint": "@handle (optional)", "width": 150 }
    } },
    "new_row_loc": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "location": { "type": "InputText", "hint": "postgres://user@host/db  or  /path/file.db", "width": -1 }
    } },
    "new_row2": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "password": { "type": "InputText", "hint": "password (optional)", "flags": "Password", "width": 170 },
      "btn_add":  { "type": "Button", "label": "Add connection", "width": 130 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##sq_connections_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_state":  { "type": "TableColumn", "label": "State",    "flags": "WidthFixed", "init_width": 60,  "column_id": 0 },
        "col_handle": { "type": "TableColumn", "label": "Handle",   "flags": "WidthFixed", "init_width": 150, "column_id": 1 },
        "col_driver": { "type": "TableColumn", "label": "Driver",   "flags": "WidthFixed", "init_width": 90,  "column_id": 2 },
        "col_loc":    { "type": "TableColumn", "label": "Location", "flags": "WidthStretch",                   "column_id": 3 },
        "col_act":    { "type": "TableColumn", "label": "",         "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

static constexpr const char* kNavigatorLayout = R"json({
  "type": "Window", "title": "Navigator", "width": 360, "height": 520,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "tree": { "type": "VerticalLayout", "children": {} }
  } } }
})json";

static constexpr const char* kStructureLayout = R"json({
  "type": "Window", "title": "Structure", "width": 700, "height": 320,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "title": { "type": "Label", "text": "(select a table in the Navigator and click Structure)" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##sq_structure_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_pos":  { "type": "TableColumn", "label": "#",          "flags": "WidthFixed", "init_width": 36,  "column_id": 0 },
        "col_name": { "type": "TableColumn", "label": "Column",     "flags": "WidthFixed", "init_width": 180, "column_id": 1 },
        "col_type": { "type": "TableColumn", "label": "Type",       "flags": "WidthFixed", "init_width": 130, "column_id": 2 },
        "col_pk":   { "type": "TableColumn", "label": "Key",        "flags": "WidthFixed", "init_width": 46,  "column_id": 3 },
        "col_null": { "type": "TableColumn", "label": "Nullable",   "flags": "WidthFixed", "init_width": 70,  "column_id": 4 },
        "col_ref":  { "type": "TableColumn", "label": "References", "flags": "WidthStretch",                   "column_id": 5 }
      }
    }
  } } }
})json";

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 900, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##sq_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
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

// Live text of an InputText widget ("" when the widget is not built yet).
std::string text_of(const ui_element_ptr& input) {
  return input ? input->as<std::string>("value"_key) : std::string{};
}

std::string join_lines(const std::vector<std::string>& items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i)
    out += (i ? "\n" : "") + items[i];
  return out;
}

} // namespace

// ── sq_frontend ────────────────────────────────────────────────────────────

sq_frontend::sq_frontend(dynamic&& base) : form(std::move(base)) {}

std::string sq_frontend::quote_identifier(const std::string& driver, const std::string& name) {
  char open = '"', close = '"';
  if (driver == "mysql" || driver == "mariadb") {
    open = close = '`';
  } else if (driver == "sqlserver") {
    open = '[';
    close = ']';
  }
  std::string out(1, open);
  for (char c : name) {
    out += c;
    if (c == close)
      out += c; // "" / `` / ]] escapes the quote character
  }
  out += close;
  return out;
}

void sq_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void sq_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto list = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*list)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = list;
  parent->refresh_children_order();
}

ui_element_ptr sq_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void sq_frontend::set_status(const ui_element_ptr& label, const std::string& text, bool ok) {
  if (!label)
    return;
  label["text"_key] = text;
  label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

void sq_frontend::build_window(
    const char* layout_json, const std::string& root_key, key_t& window_id_out,
    const std::function<void(ui_tree&)>& wire) {
  auto tree = import_json(layout_json);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
  window_id_out = (*tree[""])["__wish_id"_key].as<key_t>();
  wire(tree);

  const bool is_main = root_key == internal_root_key_;
  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), root_key);
  if (!is_main) {
    sess().top_level_objects[key_t{root_key}] = root_ptr;
    sess().top_level_handlers[key_t{root_key}] = this;
    (*root_ptr)["__path__"_key] = root_key;
  }
}

void sq_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
  dynamic params;
  params["title"_key] = std::string{"Confirm"};
  params["message"_key] = message;
  params["icon"_key] = std::string{"warning"};
  params["buttons"_key] = std::string{"yes_no"};

  dialog_ = instantiate_child_form<message_box>(
      "MessageBox"_key, std::move(params),
      [on_confirm = std::move(on_confirm)](key_t /*event_name*/, const dynamic& payload) {
        if (payload.as<std::string>("button"_key) == "yes")
          on_confirm();
      });
}

void sq_frontend::release_ids(std::vector<key_t>& ids) {
  for (auto id : ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
  ids.clear();
}

ui_element_ptr sq_frontend::make_row(
    std::vector<key_t>& ids, const std::vector<ui_element_ptr>& cells,
    const std::vector<std::pair<std::string, std::function<void()>>>& menu) {
  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);
  ids.push_back(wish_id_of(row));

  std::vector<ui_element_ptr> kids = cells;
  for (auto& cell : cells)
    ids.push_back(wish_id_of(cell));

  if (!menu.empty()) {
    ui_element_ptr button = ui_element_ptr::create("wish"_key, "MenuButton"_key);
    button["label"_key] = std::string{"..."};
    assign_id(button);
    ids.push_back(wish_id_of(button));
    std::vector<ui_element_ptr> items;
    for (auto& [label, fn] : menu) {
      ui_element_ptr mi = ui_element_ptr::create("wish"_key, "MenuItem"_key);
      mi["label"_key] = label;
      assign_id(mi);
      ids.push_back(wish_id_of(mi));
      click_handlers_[wish_id_of(mi)] = fn;
      items.push_back(mi);
    }
    set_children_list(button, items);
    kids.push_back(button);
  }
  set_children_list(row, kids);
  return row;
}

void sq_frontend::replace_rows(
    const ui_element_ptr& table, std::vector<key_t>& ids, const std::vector<ui_element_ptr>& rows) {
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
  release_ids(ids);

  size_t next = 0;
  for (auto& row : rows)
    (*children)[next++] = dynamic_ptr{row};
  table->refresh_children_order();
}

// ── on_init ────────────────────────────────────────────────────────────────

void sq_frontend::on_init() {
  internal_root_key_ = next_available_key("__sq_");
  // Copied once here (inside dispatch) so the editor-file helpers also work
  // from event handlers that run outside it.
  resource_dir_ = sess().resource_dir;
  allow_absolute_paths_ = sess().allow_absolute_paths;

  build_window(kEditorLayout, internal_root_key_, editor_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.sql", [&](const auto& e) { sql_input_ = e; });
    tree.with("vbox.toolbar.conn", [&](const auto& e) {
      conn_combo_ = e;
      conn_combo_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.rows", [&](const auto& e) { rows_combo_ = e; });
    tree.with("vbox.status", [&](const auto& e) { editor_status_ = e; });
    tree.with("vbox.toolbar.btn_run", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        emit_query(read_sql());
      };
    });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
  });

  set_sql_text("");

  connections_root_key_ = internal_root_key_ + "_connections";
  build_window(kConnectionsLayout, connections_root_key_, connections_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.status", [&](const auto& e) { connections_status_ = e; });
    tree.with("vbox.table", [&](const auto& e) { connections_table_ = e; });
    tree.with("vbox.new_row1.driver", [&](const auto& e) { driver_combo_ = e; });
    tree.with("vbox.new_row1.handle", [&](const auto& e) { handle_input_ = e; });
    tree.with("vbox.new_row_loc.location", [&](const auto& e) { location_input_ = e; });
    tree.with("vbox.new_row2.password", [&](const auto& e) { password_input_ = e; });
    tree.with("vbox.new_row2.btn_add", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        const std::string location = text_of(location_input_);
        if (location.empty()) {
          set_status(connections_status_, "Enter a location (connection string or file path).", false);
          return;
        }
        const int32_t di = driver_combo_->as<int32_t>("value"_key);
        dynamic p;
        p["handle"_key] = text_of(handle_input_);
        p["location"_key] = location;
        p["driver"_key] = di > 0 && static_cast<size_t>(di) < driver_types_.size() ? driver_types_[di] : std::string{};
        p["password"_key] = text_of(password_input_);
        emit("add_connection_requested"_key, std::move(p));
        password_input_["value"_key] = std::string{}; // never keep the password in the widget
      };
    });
  });

  structure_root_key_ = internal_root_key_ + "_structure";
  build_window(kStructureLayout, structure_root_key_, structure_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.title", [&](const auto& e) { structure_title_ = e; });
    tree.with("vbox.table", [&](const auto& e) { structure_table_ = e; });
  });

  navigator_root_key_ = internal_root_key_ + "_navigator";
  build_window(kNavigatorLayout, navigator_root_key_, navigator_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.tree", [&](const auto& e) { nav_box_ = e; });
  });

  console_root_key_ = internal_root_key_ + "_console";
  build_window(kConsoleLayout, console_root_key_, console_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.table", [&](const auto& e) { console_table_ = e; });
  });

  results_root_key_ = internal_root_key_ + "_results";
  build_window(kResultsLayout, results_root_key_, results_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.status", [&](const auto& e) { results_status_ = e; });
    tree.with("vbox.table", [&](const auto& e) { results_table_ = e; });
    tree.with("vbox.export.path", [&](const auto& e) { export_path_input_ = e; });
    tree.with("vbox.export.overwrite", [&](const auto& e) { overwrite_checkbox_ = e; });
    tree.with("vbox.export.btn_export", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        dynamic p;
        p["path"_key] = text_of(export_path_input_);
        p["overwrite"_key] = overwrite_checkbox_ && overwrite_checkbox_->as<bool>("value"_key);
        emit("export_requested"_key, std::move(p));
      };
    });
  });

  rebuild_navigator("No active database.");

  // First-run arrangement (owned by imgui.ini afterwards): a DBeaver-like
  // left column -- Connections over Navigator/Structure tabs -- and on the
  // right the SQL Editor over the Results/Console tabs.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "sq_dock", "SQ",
        layout(
            split(
                dir::left, 0.30f,
                split(
                    dir::up, 0.36f,
                    area({connections_root_key_}),
                    area({structure_root_key_, navigator_root_key_}, navigator_root_key_)),
                split(
                    dir::up, 0.40f,
                    area({internal_root_key_}),
                    area({console_root_key_, results_root_key_}, results_root_key_))),
            /*version=*/1, /*target=*/"sq_dock")));
  }

  // Initial population is triggered client-side (run_sq() calls
  // source->refresh_all() after wiring every handler), never via an
  // on_init()-emitted event (git's documented initial-load-race fix).
}

// ── Connections ────────────────────────────────────────────────────────────

dynamic sq_frontend::do_update_drivers(const dynamic& args) {
  driver_types_.assign(1, std::string{});
  std::vector<std::string> labels = {"(auto-detect)"};
  for_each_entry(args, "drivers"_key, [&](const dynamic& e) {
    driver_types_.push_back(str_of(e, "type"_key));
    labels.push_back(str_of(e, "type"_key) + "  -  " + str_of(e, "description"_key));
  });
  if (driver_combo_) {
    driver_combo_["items"_key] = join_lines(labels);
    driver_combo_["value"_key] = int32_t{0};
  }
  return dynamic{};
}

dynamic sq_frontend::do_update_connections(const dynamic& args) {
  active_handle_ = str_of(args, "active"_key);
  handles_.clear();
  std::vector<ui_element_ptr> rows;
  std::vector<key_t> ids;

  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    const std::string handle = str_of(e, "handle"_key);
    const std::string driver = str_of(e, "driver"_key);
    handles_.push_back(handle);
    const bool active = handle == active_handle_;
    if (active)
      active_driver_ = driver;

    std::vector<ui_element_ptr> cells = {
        active ? make_label("active", kOkLight, kOkDark) : make_label("", kIdleLight, kIdleDark),
        make_label(handle),
        make_label(driver),
        make_label(str_of(e, "location"_key), kIdleLight, kIdleDark),
    };
    std::vector<std::pair<std::string, std::function<void()>>> menu;
    if (!active)
      menu.push_back({"Connect", [this, handle] { emit("activate_requested"_key, payload1("handle"_key, handle)); }});
    menu.push_back({"Ping", [this, handle] { emit("ping_requested"_key, payload1("handle"_key, handle)); }});
    menu.push_back({"Remove...", [this, handle] {
                      show_confirm(
                          "Remove connection " + handle + " from sq? The database itself is not touched.",
                          [this, handle] {
                            emit("remove_connection_requested"_key, payload1("handle"_key, handle));
                          });
                    }});
    rows.push_back(make_row(ids, cells, menu));
  });
  replace_rows(connections_table_, connection_ids_, rows); // releases the old rows' ids
  connection_ids_ = std::move(ids);

  // Editor picker.
  if (conn_combo_) {
    int32_t sel = 0;
    for (size_t i = 0; i < handles_.size(); ++i)
      if (handles_[i] == active_handle_)
        sel = static_cast<int32_t>(i);
    conn_combo_["items"_key] = handles_.empty() ? std::string{"(no connection)"} : join_lines(handles_);
    conn_combo_["value"_key] = sel;
  }
  return dynamic{};
}

// ── Navigator / Structure ──────────────────────────────────────────────────

dynamic sq_frontend::do_update_schema(const dynamic& args) {
  tables_.clear();
  schema_handle_ = str_of(args, "handle"_key);
  active_driver_ = schema_handle_.empty() ? active_driver_ : str_of(args, "driver"_key);
  schema_product_ = str_of(args, "product"_key);

  for_each_entry(args, "tables"_key, [&](const dynamic& t) {
    table_info ti;
    ti.name = str_of(t, "name"_key);
    ti.type = str_of(t, "type"_key);
    ti.rows = t.as<int32_t>("rows"_key);
    for_each_entry(t, "columns"_key, [&](const dynamic& c) {
      column_info ci;
      ci.name = str_of(c, "name"_key);
      ci.type = str_of(c, "type"_key);
      ci.pk = c.as<bool>("pk"_key);
      ci.nullable = c.as<bool>("nullable"_key);
      ci.fk = str_of(c, "fk"_key);
      ti.columns.push_back(std::move(ci));
    });
    tables_.push_back(std::move(ti));
  });

  rebuild_navigator(str_of(args, "error"_key));

  // Reset the Structure window; it refers to the previous database's table.
  if (structure_title_)
    structure_title_["text"_key] = std::string{"(select a table in the Navigator and click Structure)"};
  replace_rows(structure_table_, structure_ids_, {});
  return dynamic{};
}

void sq_frontend::rebuild_navigator(const std::string& heading) {
  release_ids(nav_ids_);
  if (!nav_box_)
    return;

  auto track = [&](const ui_element_ptr& el) {
    assign_id(el);
    nav_ids_.push_back(wish_id_of(el));
    return el;
  };
  auto node = [&](const std::string& label, bool open, bool leaf) {
    ui_element_ptr n = ui_element_ptr::create("wish"_key, "TreeNode"_key);
    n["label"_key] = label;
    n["open"_key] = open;
    n["leaf"_key] = leaf;
    return track(n);
  };

  if (schema_handle_.empty() || !heading.empty()) {
    ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
    l["text"_key] = heading.empty() ? std::string{"No active database."} : heading;
    if (!heading.empty()) {
      l["text_color_light"_key] = std::string{kBadLight};
      l["text_color_dark"_key] = std::string{kBadDark};
    }
    track(l);
    set_children_list(nav_box_, {l});
    return;
  }

  std::string root_label = schema_handle_ + "  (" + active_driver_;
  if (!schema_product_.empty())
    root_label += " - " + schema_product_;
  root_label += ")";
  ui_element_ptr root = node(root_label, true, false);

  auto section = [&](const char* title, const char* type) {
    std::vector<ui_element_ptr> table_nodes;
    for (size_t ti = 0; ti < tables_.size(); ++ti) {
      const auto& t = tables_[ti];
      if (t.type != type)
        continue;
      std::string label = t.name;
      if (t.rows >= 0)
        label += "  [" + std::to_string(t.rows) + " rows]";
      ui_element_ptr tn = node(label, false, false);

      ui_element_ptr actions = ui_element_ptr::create("wish"_key, "HorizontalLayout"_key);
      actions["spacing"_key] = 4.0f;
      track(actions);
      ui_element_ptr b_data = ui_element_ptr::create("wish"_key, "Button"_key);
      b_data["label"_key] = std::string{"View data"};
      track(b_data);
      click_handlers_[wish_id_of(b_data)] = [this, ti] {
        const std::string sql = "SELECT * FROM " + quote_identifier(active_driver_, tables_[ti].name);
        set_sql_text(sql); // shown in the editor, DBeaver-style
        emit_query(sql);
      };
      ui_element_ptr b_struct = ui_element_ptr::create("wish"_key, "Button"_key);
      b_struct["label"_key] = std::string{"Structure"};
      track(b_struct);
      click_handlers_[wish_id_of(b_struct)] = [this, ti] { show_structure(ti); };
      set_children_list(actions, {b_data, b_struct});

      std::vector<ui_element_ptr> kids = {actions};
      for (const auto& c : t.columns) {
        std::string cl = c.name + " : " + c.type;
        if (c.pk)
          cl += "  [PK]";
        if (!c.fk.empty())
          cl += "  -> " + c.fk;
        kids.push_back(node(cl, false, true));
      }
      set_children_list(tn, kids);
      table_nodes.push_back(tn);
    }
    ui_element_ptr sn = node(std::string{title} + " (" + std::to_string(table_nodes.size()) + ")", true, false);
    set_children_list(sn, table_nodes);
    return sn;
  };

  set_children_list(root, {section("Tables", "table"), section("Views", "view")});
  set_children_list(nav_box_, {root});
}

void sq_frontend::show_structure(size_t table_index) {
  if (table_index >= tables_.size())
    return;
  const auto& t = tables_[table_index];
  if (structure_title_) {
    std::string title = t.name + "  (" + t.type;
    if (t.rows >= 0)
      title += ", " + std::to_string(t.rows) + " rows";
    title += ")";
    structure_title_["text"_key] = title;
  }
  std::vector<key_t> ids;
  std::vector<ui_element_ptr> rows;
  size_t pos = 0;
  for (const auto& c : t.columns) {
    rows.push_back(make_row(
        ids,
        {make_label(std::to_string(++pos), kIdleLight, kIdleDark),
         c.pk ? make_label(c.name, kKeyLight, kKeyDark) : make_label(c.name),
         make_label(c.type),
         c.pk ? make_label("PK", kKeyLight, kKeyDark) : make_label(""),
         make_label(c.nullable ? "yes" : "no", kIdleLight, kIdleDark),
         make_label(c.fk, kIdleLight, kIdleDark)}));
  }
  replace_rows(structure_table_, structure_ids_, rows); // releases the old rows' ids
  structure_ids_ = std::move(ids);
}

// ── Results ────────────────────────────────────────────────────────────────

// The TextEditor edits a file in the session sandbox and writes every edit
// back to it, so the query text is read from (and set by writing) that file.
// A fresh file name per set_sql_text() call makes the widget reload it.
std::string sq_frontend::read_sql() {
  if (!sql_input_)
    return {};
  const auto* path = sql_input_->findField<std::string>("file_path"_key);
  if (!path || path->empty())
    return {};
  auto full = file_service::resolve_path(*path, resource_dir_, allow_absolute_paths_);
  if (full.empty())
    return {};
  std::ifstream f(full, std::ios::binary);
  return f ? std::string{std::istreambuf_iterator<char>(f), {}} : std::string{};
}

void sq_frontend::set_sql_text(const std::string& sql) {
  if (!sql_input_)
    return;
  namespace fs = std::filesystem;
  const std::string name = "sq_query_" + std::to_string(++sql_file_seq_) + ".sql";
  auto full = file_service::resolve_path(name, resource_dir_, allow_absolute_paths_);
  if (full.empty())
    return;
  {
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    f << sql;
  }
  if (const auto* old = sql_input_->findField<std::string>("file_path"_key); old && !old->empty()) {
    std::error_code ec;
    fs::remove(file_service::resolve_path(*old, resource_dir_, allow_absolute_paths_), ec);
  }
  sql_input_["file_path"_key] = name;
}

void sq_frontend::emit_query(const std::string& sql) {
  if (sql.find_first_not_of(" \t\r\n") == std::string::npos) {
    set_status(editor_status_, "Type a query first.", false);
    return;
  }
  int32_t idx = rows_combo_ ? rows_combo_->as<int32_t>("value"_key) : 1;
  idx = std::clamp(idx, int32_t{0}, static_cast<int32_t>(std::size(kRowLimits)) - 1);
  set_status(editor_status_, "Running...", true);
  dynamic p;
  p["sql"_key] = sql;
  p["max_rows"_key] = kRowLimits[idx];
  emit("query_requested"_key, std::move(p));
}

dynamic sq_frontend::do_update_result(const dynamic& args) {
  release_ids(result_ids_);

  auto clear_table = [&] {
    if (results_table_)
      set_children_list(results_table_, {});
  };

  if (!args.as<bool>("ok"_key)) {
    clear_table();
    set_status(results_status_, "Query failed.", false);
    set_status(editor_status_, str_of(args, "error"_key), false);
    return dynamic{};
  }

  std::vector<std::string> names;
  for_each_entry(args, "columns"_key, [&](const dynamic& c) { names.push_back(str_of(c, "name"_key)); });

  std::vector<ui_element_ptr> kids;
  auto add_column = [&](const std::string& label, float width, int32_t id) {
    ui_element_ptr col = ui_element_ptr::create("wish"_key, "TableColumn"_key);
    col["label"_key] = label;
    col["flags"_key] = std::string{"WidthFixed"};
    col["init_width"_key] = width;
    col["column_id"_key] = id;
    assign_id(col);
    result_ids_.push_back(wish_id_of(col));
    kids.push_back(col);
  };
  add_column("#", 56.0f, 0);
  for (size_t i = 0; i < names.size(); ++i)
    add_column(names[i], 150.0f, static_cast<int32_t>(i + 1));

  size_t row_no = 0;
  const auto* arr_f = args.findField<dynamic_ptr>("rows"_key);
  if (arr_f && *arr_f) {
    (*arr_f)->forEach([&](key_t, const field& f) {
      if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      std::vector<ui_element_ptr> cells = {make_label(std::to_string(++row_no), kIdleLight, kIdleDark)};
      size_t col = 0;
      f.as<dynamic_ptr>()->forEach([&](key_t, const field& cf) {
        if (col++ >= names.size() || !cf.is<std::string>())
          return;
        const std::string& v = cf.as<std::string>();
        if (v == kNullCell) {
          cells.push_back(make_label("NULL", kIdleLight, kIdleDark));
        } else if (v.size() > kMaxCellChars) {
          cells.push_back(make_label(v.substr(0, kMaxCellChars) + "..."));
        } else {
          cells.push_back(make_label(v));
        }
      });
      while (cells.size() < names.size() + 1)
        cells.push_back(make_label(""));
      kids.push_back(make_row(result_ids_, cells));
    });
  }

  // A fresh ImGui table id per result: ImGui keeps per-table column state
  // (widths, order) keyed by id, which must not leak between results with
  // different column sets.
  results_table_["id"_key] = "##sq_results_" + std::to_string(++result_seq_);
  results_table_["columns"_key] = static_cast<int32_t>(names.size() + 1);
  set_children_list(results_table_, kids);

  const int32_t total = args.as<int32_t>("total_rows"_key);
  std::string status = std::to_string(total) + (total == 1 ? " row" : " rows");
  if (args.as<bool>("truncated"_key))
    status += " (showing the first " + std::to_string(row_no) + "; Export CSV writes all of them)";
  if (names.empty())
    status = "0 rows (no columns reported for an empty result)";
  status += " in " + std::to_string(args.as<int32_t>("elapsed_ms"_key)) + " ms";
  set_status(results_status_, status, true);
  set_status(editor_status_, status, true);
  return dynamic{};
}

dynamic sq_frontend::do_command_result(const dynamic& args) {
  const std::string scope = str_of(args, "scope"_key);
  set_status(
      scope == "connections" ? connections_status_ : scope == "results" ? results_status_ : editor_status_,
      str_of(args, "message"_key), args.as<bool>("ok"_key));
  return dynamic{};
}

dynamic sq_frontend::do_show_unavailable(const dynamic& args) {
  const std::string message = str_of(args, "message"_key);
  set_status(editor_status_, message, false);
  set_status(connections_status_, message, false);
  dynamic params;
  params["title"_key] = std::string{"sq is not installed"};
  params["message"_key] = message;
  params["icon"_key] = std::string{"error"};
  params["buttons"_key] = std::string{"ok"};
  dialog_ = instantiate_child_form<message_box>("MessageBox"_key, std::move(params), [](key_t, const dynamic&) {});
  return dynamic{};
}

// ── Console ────────────────────────────────────────────────────────────────

void sq_frontend::append_console_row(
    const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
  if (!console_table_)
    return;
  auto* children_p = console_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  const char* cl = ok ? kOkLight : kBadLight;
  const char* cd = ok ? kOkDark : kBadDark;
  std::vector<key_t> ids;
  ui_element_ptr row = make_row(
      ids,
      {make_label(std::to_string(++console_seq_), kIdleLight, kIdleDark), make_label(command, cl, cd),
       make_label(std::to_string(exit_code), cl, cd), make_label(output, cl, cd)});

  console_row_entry entry;
  entry.child_key = next_console_child_key_++;
  entry.object_ids = std::move(ids);
  (*children)[entry.child_key] = dynamic_ptr{row};
  console_rows_.push_back(std::move(entry));

  if (console_rows_.size() > kMaxConsoleRows) {
    for (auto id : console_rows_.front().object_ids)
      ctx().objects.erase(id.id);
    children->erase(console_rows_.front().child_key);
    console_rows_.pop_front();
  }
  console_table_->refresh_children_order();
}

dynamic sq_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      str_of(args, "command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      str_of(args, "output"_key));
  return dynamic{};
}

// ── Events ─────────────────────────────────────────────────────────────────

void sq_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  if (event == "closed"_key &&
      (id == editor_window_id_ || id == connections_window_id_ || id == navigator_window_id_ ||
       id == structure_window_id_ || id == results_window_id_ || id == console_window_id_)) {
    emit("closed"_key);
    remove_objects_at(connections_root_key_);
    remove_objects_at(navigator_root_key_);
    remove_objects_at(structure_root_key_);
    remove_objects_at(results_root_key_);
    remove_objects_at(console_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    if (id == conn_combo_id_) {
      const int32_t idx = payload.as<int32_t>("value"_key);
      if (idx >= 0 && static_cast<size_t>(idx) < handles_.size() && handles_[idx] != active_handle_)
        emit("activate_requested"_key, payload1("handle"_key, handles_[idx]));
    }
    return;
  }

  if (event != "clicked"_key)
    return;
  if (auto ch = click_handlers_.find(id); ch != click_handlers_.end())
    ch->second();
}

// ── Registration ───────────────────────────────────────────────────────────

void register_sq() {
  auto proto = dynamic_ptr{"SqFrontend"_key, {}};

  auto add_method = [&](key_t name, dynamic (sq_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<sq_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_connections"_key, &sq_frontend::do_update_connections);
  add_method("update_drivers"_key, &sq_frontend::do_update_drivers);
  add_method("update_schema"_key, &sq_frontend::do_update_schema);
  add_method("update_result"_key, &sq_frontend::do_update_result);
  add_method("command_result"_key, &sq_frontend::do_command_result);
  add_method("append_command_log"_key, &sq_frontend::do_append_command_log);
  add_method("show_unavailable"_key, &sq_frontend::do_show_unavailable);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("SqFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "DBeaver-style, query-only GUI frontend for the local `sq` CLI. All `sq` invocation happens client-side; "
      "this form only renders whatever snapshot it was last given. Listen for the 'closed' event to detect when "
      "the user is done, and the '*_requested' events to react to user actions -- see sq.hpp's class doc comment "
      "for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<sq_frontend>("wish"_key, "SqFrontend"_key));
}

} // namespace bdg::wish
