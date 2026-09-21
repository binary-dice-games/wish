// MIT License © 2026 Binary Dice Games
/// @file curl.cpp
/// @brief Implementation of the CurlFrontend form.
///
/// A close sibling of modules/bdg/dev/docker/server/docker.cpp: inline
/// JSON window layouts + import_json(), C++-built table rows, a per-row
/// `...` MenuButton, show_confirm() via a privately-instantiated
/// MessageBox, and an id -> handler dispatch map. Diverges from
/// docker/kubectl in one place: the `kv_table` plumbing (Params/Headers/
/// Form-body/Environment-variables) builds *editable* rows whose current
/// values are read directly off the live widgets rather than mirrored in
/// a separate snapshot, and rows are added/removed individually instead
/// of being fully rebuilt on every change -- see curl.hpp's class comment
/// and DESIGN.md "Editable key-value tables".
#include "curl.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/dock_layout_spec.hpp>
#include <ui/forms/message_box.hpp>

#include <cstdio>
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

// "#RRGGBBAA" light/dark pairs -- GitHub Primer tokens, docker.cpp's
// theme_hex pattern.
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kIdleLight = "#656D76FF";
constexpr const char* kIdleDark = "#8B949EFF";
constexpr const char* kWarnLight = "#9A6700FF";
constexpr const char* kWarnDark = "#D29922FF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";

constexpr const char* kMethods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
constexpr size_t kNumMethods = 7;
constexpr const char* kBodyModes[] = {"none", "raw", "json", "form"};
constexpr size_t kNumBodyModes = 4;
constexpr const char* kAuthModes[] = {"none", "basic", "bearer"};
constexpr size_t kNumAuthModes = 3;

int32_t index_of(const char* const* arr, size_t n, const std::string& v) {
  for (size_t i = 0; i < n; ++i)
    if (v == arr[i])
      return static_cast<int32_t>(i);
  return 0;
}

std::string format_ms(float ms) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(ms < 10.0f ? 1 : 0);
  oss << ms << " ms";
  return oss.str();
}

std::string format_bytes(float bytes) {
  const char* units[] = {"B", "KB", "MB", "GB"};
  size_t u = 0;
  double v = bytes;
  while (v >= 1024.0 && u < 3) {
    v /= 1024.0;
    ++u;
  }
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(u == 0 ? 0 : 1);
  oss << v << " " << units[u];
  return oss.str();
}

// ── Window layouts ──────────────────────────────────────────────────────
//
// curl_mock.json / curl_mock.html (this directory) mirror these layouts
// as a single tabbed window -- the mockup validated in the `editor` tool
// before implementation.

static constexpr const char* kRequestLayout = R"json({
  "type": "Window", "title": "Request", "width": 900, "height": 700,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "method": { "type": "Combo", "items": "GET\nPOST\nPUT\nPATCH\nDELETE\nHEAD\nOPTIONS", "value": 0, "width": 100 },
      "url":    { "type": "InputText", "hint": "https://api.example.com/v1/users", "width": -1, "max_length": 4096 },
      "env":    { "type": "Combo", "items": "No Environment", "value": 0, "width": 160 },
      "follow": { "type": "Checkbox", "label": "Follow redirects", "value": true },
      "btn_send": { "type": "Button", "label": "Send", "width": 90 }
    } },
    "save_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "save_label": { "type": "Label", "text": "Save as:" },
      "save_name":  { "type": "InputText", "hint": "request name", "width": 200 },
      "save_collection": { "type": "InputText", "hint": "collection (default: Default)", "width": 220 },
      "btn_save":   { "type": "Button", "label": "Save", "width": 80 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "tabs": { "type": "TabBar", "id": "##curl_request_tabs", "height": -1, "children": {
      "params_tab": { "type": "TabItem", "label": "Params", "children": {
        "p_toolbar": { "type": "HorizontalLayout", "children": { "btn_add": { "type": "Button", "label": "+ Add Param", "width": 120 } } },
        "p_table": {
          "type": "Table", "id": "##params_table", "columns": 4,
          "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
          "height": 260, "outer_height": 260,
          "children": {
            "col_enabled": { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 30, "column_id": 0 },
            "col_key":     { "type": "TableColumn", "label": "Key",   "flags": "WidthStretch",                    "column_id": 1 },
            "col_value":   { "type": "TableColumn", "label": "Value", "flags": "WidthStretch",                    "column_id": 2 },
            "col_remove":  { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 34, "column_id": 3 }
          }
        }
      } },
      "headers_tab": { "type": "TabItem", "label": "Headers", "children": {
        "h_toolbar": { "type": "HorizontalLayout", "children": { "btn_add": { "type": "Button", "label": "+ Add Header", "width": 120 } } },
        "h_table": {
          "type": "Table", "id": "##headers_table", "columns": 4,
          "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
          "height": 260, "outer_height": 260,
          "children": {
            "col_enabled": { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 30, "column_id": 0 },
            "col_key":     { "type": "TableColumn", "label": "Key",   "flags": "WidthStretch",                    "column_id": 1 },
            "col_value":   { "type": "TableColumn", "label": "Value", "flags": "WidthStretch",                    "column_id": 2 },
            "col_remove":  { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 34, "column_id": 3 }
          }
        }
      } },
      "body_tab": { "type": "TabItem", "label": "Body", "children": {
        "mode": { "type": "Combo", "items": "None\nRaw\nJSON\nForm URL-Encoded", "value": 0, "width": 200 },
        "raw_box": { "type": "VerticalLayout", "visible": false, "children": {
          "text": { "type": "InputText", "multiline": true, "height": 260, "width": -1, "max_length": 65536, "hint": "request body" }
        } },
        "form_box": { "type": "VerticalLayout", "visible": false, "children": {
          "f_toolbar": { "type": "HorizontalLayout", "children": { "btn_add": { "type": "Button", "label": "+ Add Field", "width": 120 } } },
          "f_table": {
            "type": "Table", "id": "##body_form_table", "columns": 4,
            "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
            "height": 220, "outer_height": 220,
            "children": {
              "col_enabled": { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 30, "column_id": 0 },
              "col_key":     { "type": "TableColumn", "label": "Key",   "flags": "WidthStretch",                    "column_id": 1 },
              "col_value":   { "type": "TableColumn", "label": "Value", "flags": "WidthStretch",                    "column_id": 2 },
              "col_remove":  { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 34, "column_id": 3 }
            }
          }
        } }
      } },
      "auth_tab": { "type": "TabItem", "label": "Auth", "children": {
        "mode": { "type": "Combo", "items": "None\nBasic Auth\nBearer Token", "value": 0, "width": 200 },
        "basic_box": { "type": "VerticalLayout", "visible": false, "children": {
          "username": { "type": "InputText", "label": "Username", "width": 300 },
          "password": { "type": "InputText", "label": "Password", "width": 300, "flags": "Password" }
        } },
        "bearer_box": { "type": "VerticalLayout", "visible": false, "children": {
          "token": { "type": "InputText", "label": "Token", "width": 400, "max_length": 8192 }
        } }
      } }
    } }
  } } }
})json";

static constexpr const char* kResponseLayout = R"json({
  "type": "Window", "title": "Response", "width": 900, "height": 620,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "status": { "type": "Label", "text": "(no request sent yet)" },
    "sep": { "type": "Separator" },
    "tabs": { "type": "TabBar", "id": "##curl_response_tabs", "height": -1, "children": {
      "body_tab": { "type": "TabItem", "label": "Body", "children": {
        "editor": { "type": "TextEditor", "file_path": "", "language": "none", "read_only": true, "height": 0, "width": 0 }
      } },
      "headers_tab": { "type": "TabItem", "label": "Headers", "children": {
        "h_table": {
          "type": "Table", "id": "##response_headers_table", "columns": 2,
          "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
          "height": -1, "outer_height": -1,
          "children": {
            "col_name":  { "type": "TableColumn", "label": "Name",  "flags": "WidthFixed",   "init_width": 220, "column_id": 0 },
            "col_value": { "type": "TableColumn", "label": "Value", "flags": "WidthStretch",                     "column_id": 1 }
          }
        }
      } }
    } }
  } } }
})json";

static constexpr const char* kHistoryLayout = R"json({
  "type": "Window", "title": "History", "width": 860, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_clear": { "type": "Button", "label": "Clear History", "width": 120 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##history_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1,
      "children": {
        "col_method": { "type": "TableColumn", "label": "Method", "flags": "WidthFixed",   "init_width": 70,  "column_id": 0 },
        "col_url":    { "type": "TableColumn", "label": "URL",    "flags": "WidthStretch",                     "column_id": 1 },
        "col_status": { "type": "TableColumn", "label": "Status", "flags": "WidthFixed",   "init_width": 70,  "column_id": 2 },
        "col_time":   { "type": "TableColumn", "label": "Time",   "flags": "WidthFixed",   "init_width": 80,  "column_id": 3 },
        "col_when":   { "type": "TableColumn", "label": "When",   "flags": "WidthFixed",   "init_width": 140, "column_id": 4 },
        "col_actions":{ "type": "TableColumn", "label": "",       "flags": "WidthFixed",   "init_width": 40,  "column_id": 5 }
      }
    }
  } } }
})json";

static constexpr const char* kCollectionsLayout = R"json({
  "type": "Window", "title": "Collections", "width": 860, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##collections_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1,
      "children": {
        "col_collection": { "type": "TableColumn", "label": "Collection", "flags": "WidthFixed",   "init_width": 140, "column_id": 0 },
        "col_name":       { "type": "TableColumn", "label": "Name",       "flags": "WidthFixed",   "init_width": 160, "column_id": 1 },
        "col_method":     { "type": "TableColumn", "label": "Method",     "flags": "WidthFixed",   "init_width": 70,  "column_id": 2 },
        "col_url":        { "type": "TableColumn", "label": "URL",        "flags": "WidthStretch",                     "column_id": 3 },
        "col_actions":    { "type": "TableColumn", "label": "",           "flags": "WidthFixed",   "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

static constexpr const char* kEnvironmentsLayout = R"json({
  "type": "Window", "title": "Environments", "width": 780, "height": 620,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "new_name": { "type": "InputText", "hint": "new environment name", "width": 220 },
      "btn_new":  { "type": "Button", "label": "New Environment", "width": 140 }
    } },
    "table": {
      "type": "Table", "id": "##environments_table", "columns": 3,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": 180, "outer_height": 180,
      "children": {
        "col_name":   { "type": "TableColumn", "label": "Name",      "flags": "WidthStretch",                   "column_id": 0 },
        "col_count":  { "type": "TableColumn", "label": "Variables", "flags": "WidthFixed",   "init_width": 90, "column_id": 1 },
        "col_actions":{ "type": "TableColumn", "label": "",          "flags": "WidthFixed",   "init_width": 40, "column_id": 2 }
      }
    },
    "sep": { "type": "Separator" },
    "editor_label": { "type": "Label", "text": "(select an environment to edit its variables)" },
    "var_toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_add_var":  { "type": "Button", "label": "+ Add Variable", "width": 130 },
      "btn_save_vars":{ "type": "Button", "label": "Save Variables", "width": 130 }
    } },
    "vars_table": {
      "type": "Table", "id": "##env_vars_table", "columns": 3,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1,
      "children": {
        "col_key":    { "type": "TableColumn", "label": "Key",   "flags": "WidthStretch",                 "column_id": 0 },
        "col_value":  { "type": "TableColumn", "label": "Value", "flags": "WidthStretch",                 "column_id": 1 },
        "col_remove": { "type": "TableColumn", "label": "",      "flags": "WidthFixed",   "init_width": 34, "column_id": 2 }
      }
    }
  } } }
})json";

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 900, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##curl_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed",   "init_width": 44,  "column_id": 0 },
        "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed",   "init_width": 400, "column_id": 1 },
        "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed",   "init_width": 50,  "column_id": 2 },
        "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
      }
    }
  } } }
})json";

} // namespace

// ── curl_frontend ──────────────────────────────────────────────────────

curl_frontend::curl_frontend(dynamic&& base) : form(std::move(base)) {}

void curl_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void curl_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

ui_element_ptr curl_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void curl_frontend::build_window(
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

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), root_key);
  sess().top_level_objects[key_t{root_key}] = root_ptr;
  sess().top_level_handlers[key_t{root_key}] = this;
  (*root_ptr)["__path__"_key] = root_key;
}

void curl_frontend::build_list_window(
    list_window& lw, const char* layout_json, const std::string& root_key,
    const std::function<void(ui_tree&)>& wire_toolbar) {
  lw.root_key = root_key;
  build_window(layout_json, root_key, lw.window_id, [&](ui_tree& tree) {
    tree.with("vbox.status", [&](const auto& e) { lw.status_label = e; });
    tree.with("vbox.table", [&](const auto& e) { lw.table = e; });
    wire_toolbar(tree);
  });
}

void curl_frontend::on_init() {
  internal_root_key_ = next_available_key("__curl_");

  auto* title_f = findField<std::string>("title"_key);
  title_ = title_f ? *title_f : std::string{"Curl"};

  // ── Request window (main root) ────────────────────────────────────────
  {
    auto tree = import_json(kRequestLayout);
    auto& c = ctx();
    for (auto& [key, elem] : tree) {
      key_t id = rmi::shared::generate_id();
      c.put_object(id, elem);
      elem["__wish_id"_key] = id;
    }
    request_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();

    tree.with("vbox.toolbar.method", [&](const auto& e) {
      method_combo_ = e;
      method_combo_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.url", [&](const auto& e) {
      url_input_ = e;
      url_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.env", [&](const auto& e) {
      env_combo_ = e;
      env_combo_id_ = wish_id_of(e);
    });
    tree.with("vbox.toolbar.follow", [&](const auto& e) { follow_redirects_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_send", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("send_requested"_key, collect_request_state()); };
    });

    tree.with("vbox.save_bar.save_name", [&](const auto& e) { save_name_input_ = e; });
    tree.with("vbox.save_bar.save_collection", [&](const auto& e) { save_collection_input_ = e; });
    tree.with("vbox.save_bar.btn_save", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        std::string name = save_name_input_ ? save_name_input_->as<std::string>("value"_key) : std::string{};
        if (name.empty())
          return;
        std::string collection =
            save_collection_input_ ? save_collection_input_->as<std::string>("value"_key) : std::string{};
        dynamic p = collect_request_state();
        p["name"_key] = name;
        p["collection"_key] = collection.empty() ? std::string{"Default"} : collection;
        emit("save_request_requested"_key, std::move(p));
      };
    });

    tree.with("vbox.status", [&](const auto& e) { status_line_label_ = e; });

    tree.with("vbox.tabs.params_tab.p_toolbar.btn_add", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { kv_table_add_row(params_); };
    });
    tree.with("vbox.tabs.params_tab.p_table", [&](const auto& e) {
      params_.table = e;
      params_.has_enabled = true;
    });

    tree.with("vbox.tabs.headers_tab.h_toolbar.btn_add", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { kv_table_add_row(headers_); };
    });
    tree.with("vbox.tabs.headers_tab.h_table", [&](const auto& e) {
      headers_.table = e;
      headers_.has_enabled = true;
    });

    tree.with("vbox.tabs.body_tab.mode", [&](const auto& e) {
      body_mode_combo_ = e;
      body_mode_combo_id_ = wish_id_of(e);
    });
    tree.with("vbox.tabs.body_tab.raw_box", [&](const auto& e) { body_raw_box_ = e; });
    tree.with("vbox.tabs.body_tab.raw_box.text", [&](const auto& e) { body_text_input_ = e; });
    tree.with("vbox.tabs.body_tab.form_box", [&](const auto& e) { body_form_box_ = e; });
    tree.with("vbox.tabs.body_tab.form_box.f_toolbar.btn_add", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { kv_table_add_row(body_form_); };
    });
    tree.with("vbox.tabs.body_tab.form_box.f_table", [&](const auto& e) {
      body_form_.table = e;
      body_form_.has_enabled = true;
    });

    tree.with("vbox.tabs.auth_tab.mode", [&](const auto& e) {
      auth_mode_combo_ = e;
      auth_mode_combo_id_ = wish_id_of(e);
    });
    tree.with("vbox.tabs.auth_tab.basic_box", [&](const auto& e) { auth_basic_box_ = e; });
    tree.with("vbox.tabs.auth_tab.basic_box.username", [&](const auto& e) { auth_username_input_ = e; });
    tree.with("vbox.tabs.auth_tab.basic_box.password", [&](const auto& e) { auth_password_input_ = e; });
    tree.with("vbox.tabs.auth_tab.bearer_box", [&](const auto& e) { auth_bearer_box_ = e; });
    tree.with("vbox.tabs.auth_tab.bearer_box.token", [&](const auto& e) { auth_token_input_ = e; });

    sess().ui_objects.merge(std::move(tree), internal_root_key_);
  }

  // ── Response window ──────────────────────────────────────────────────
  response_root_key_ = internal_root_key_ + "_response";
  build_window(kResponseLayout, response_root_key_, response_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.status", [&](const auto& e) { response_status_label_ = e; });
    tree.with("vbox.tabs.body_tab.editor", [&](const auto& e) { response_body_editor_ = e; });
    tree.with("vbox.tabs.headers_tab.h_table", [&](const auto& e) { response_headers_table_ = e; });
  });

  // ── History window ───────────────────────────────────────────────────
  history_root_key_ = internal_root_key_ + "_history";
  build_list_window(history_, kHistoryLayout, history_root_key_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.btn_clear", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("clear_history_requested"_key); };
    });
  });

  // ── Collections window ───────────────────────────────────────────────
  collections_root_key_ = internal_root_key_ + "_collections";
  build_list_window(collections_, kCollectionsLayout, collections_root_key_, [](ui_tree&) {});

  // ── Environments window ──────────────────────────────────────────────
  environments_root_key_ = internal_root_key_ + "_environments";
  build_list_window(environments_, kEnvironmentsLayout, environments_root_key_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.new_name", [&](const auto& e) { env_new_name_input_ = e; });
    tree.with("vbox.toolbar.btn_new", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        std::string name = env_new_name_input_ ? env_new_name_input_->as<std::string>("value"_key) : std::string{};
        if (name.empty())
          return;
        emit("new_environment_requested"_key, payload1("name"_key, name));
        if (env_new_name_input_)
          env_new_name_input_["value"_key] = std::string{};
      };
    });
    tree.with("vbox.editor_label", [&](const auto& e) { env_editor_label_ = e; });
    tree.with("vbox.var_toolbar.btn_add_var", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        if (open_environment_id_.empty())
          return;
        kv_table_add_row(env_vars_);
      };
    });
    tree.with("vbox.var_toolbar.btn_save_vars", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        if (open_environment_id_.empty())
          return;
        dynamic p;
        p["id"_key] = open_environment_id_;
        p["vars"_key] = kv_table_read(env_vars_);
        emit("save_environment_vars_requested"_key, std::move(p));
      };
    });
    tree.with("vbox.vars_table", [&](const auto& e) {
      env_vars_.table = e;
      env_vars_.has_enabled = false;
    });
  });

  // ── Console window ───────────────────────────────────────────────────
  console_root_key_ = internal_root_key_ + "_console";
  build_window(kConsoleLayout, console_root_key_, console_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.table", [&](const auto& e) { console_table_ = e; });
  });

  // Seed the first-run arrangement: Request over a Console strip on the
  // left, Response/History/Collections/Environments tabbed together on
  // the right. Owned by imgui.ini after the first run.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "curl_dock", "Curl",
        layout(
            split(
                dir::left, 0.55f,
                split(
                    dir::down, 0.22f, area({console_root_key_}), area({internal_root_key_}, internal_root_key_)),
                area(
                    {response_root_key_, history_root_key_, collections_root_key_, environments_root_key_},
                    response_root_key_)),
            /*version=*/1, /*target=*/"curl_dock")));
  }

  // Initial population (History / Collections / Environments) is
  // triggered client-side once every handler is wired -- never via an
  // on_init()-emitted event (docker's documented initial-load-race fix).
}

// ── Editable key/value tables ────────────────────────────────────────────

void curl_frontend::kv_table_add_row(kv_table& kv, const std::string& key, const std::string& value, bool enabled) {
  if (!kv.table)
    return;
  auto* children_p = kv.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  kv_row r;
  std::vector<ui_element_ptr> cells;

  if (kv.has_enabled) {
    ui_element_ptr box = ui_element_ptr::create("wish"_key, "Checkbox"_key);
    box["value"_key] = enabled;
    assign_id(box);
    r.enabled_box = box;
    cells.push_back(box);
  }

  ui_element_ptr key_in = ui_element_ptr::create("wish"_key, "InputText"_key);
  key_in["value"_key] = key;
  key_in["hint"_key] = std::string{"key"};
  key_in["width"_key] = -1.0f;
  key_in["max_length"_key] = int32_t{1024};
  assign_id(key_in);
  r.key_input = key_in;
  cells.push_back(key_in);

  ui_element_ptr val_in = ui_element_ptr::create("wish"_key, "InputText"_key);
  val_in["value"_key] = value;
  val_in["hint"_key] = std::string{"value"};
  val_in["width"_key] = -1.0f;
  val_in["max_length"_key] = int32_t{8192};
  assign_id(val_in);
  r.value_input = val_in;
  cells.push_back(val_in);

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);

  ui_element_ptr remove_btn = ui_element_ptr::create("wish"_key, "Button"_key);
  remove_btn["label"_key] = std::string{"x"};
  remove_btn["width"_key] = 26.0f;
  assign_id(remove_btn);
  r.remove_button_id = wish_id_of(remove_btn);
  kv_remove_targets_[r.remove_button_id] = &kv;
  cells.push_back(remove_btn);

  std::vector<key_t> obj_ids;
  for (auto& c : cells)
    obj_ids.push_back(wish_id_of(c));
  obj_ids.push_back(wish_id_of(row));

  set_children_list(row, cells);
  r.row = row;
  r.child_key = kv.next_key++;
  r.object_ids = std::move(obj_ids);
  (*children)[r.child_key] = dynamic_ptr{row};
  kv.rows.push_back(std::move(r));
  kv.table->refresh_children_order();
}

void curl_frontend::kv_table_remove_row(kv_table& kv, key_t remove_button_id) {
  if (!kv.table)
    return;
  auto* children_p = kv.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto it = kv.rows.begin(); it != kv.rows.end(); ++it) {
    if (it->remove_button_id != remove_button_id)
      continue;
    children->erase(it->child_key);
    for (auto id : it->object_ids)
      ctx().objects.erase(id.id);
    kv_remove_targets_.erase(remove_button_id);
    kv.rows.erase(it);
    break;
  }
  kv.table->refresh_children_order();
}

void curl_frontend::kv_table_clear(kv_table& kv) {
  if (!kv.table)
    return;
  auto* children_p = kv.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto& r : kv.rows) {
    children->erase(r.child_key);
    for (auto id : r.object_ids)
      ctx().objects.erase(id.id);
    kv_remove_targets_.erase(r.remove_button_id);
  }
  kv.rows.clear();
  kv.next_key = 0;
}

void curl_frontend::kv_table_load(kv_table& kv, const dynamic& args, key_t field_key) {
  kv_table_clear(kv);
  for_each_entry(args, field_key, [&](const dynamic& e) {
    std::string k = e.as<std::string>("key"_key);
    std::string v = e.as<std::string>("value"_key);
    bool en = true;
    if (kv.has_enabled) {
      if (const auto* ef = e.findField<bool>("enabled"_key))
        en = *ef;
    }
    kv_table_add_row(kv, k, v, en);
  });
  if (kv.table)
    kv.table->refresh_children_order();
}

dynamic_ptr curl_frontend::kv_table_read(const kv_table& kv) const {
  auto arr = std::make_shared<dynamic>();
  size_t i = 0;
  for (auto& r : kv.rows) {
    auto e = std::make_shared<dynamic>();
    (*e)["key"_key] = r.key_input ? r.key_input->as<std::string>("value"_key) : std::string{};
    (*e)["value"_key] = r.value_input ? r.value_input->as<std::string>("value"_key) : std::string{};
    if (kv.has_enabled)
      (*e)["enabled"_key] = r.enabled_box ? r.enabled_box->as<bool>("value"_key) : true;
    (*arr)[i++] = dynamic_ptr{e};
  }
  return dynamic_ptr{arr};
}

// ── Generic read-only list-window plumbing (History / Collections / Environments) ──

void curl_frontend::clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key) {
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

void curl_frontend::add_list_row(
    list_window& lw, std::vector<list_row>& rows, size_t& next_key, list_row&& meta,
    const std::vector<ui_element_ptr>& cells, const std::vector<menu_spec>& items, const std::string& scope) {
  if (!lw.table)
    return;
  auto* children_p = lw.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::vector<key_t> obj_ids;
  for (auto& cell : cells)
    obj_ids.push_back(wish_id_of(cell));

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);
  obj_ids.push_back(wish_id_of(row));

  ui_element_ptr menu = ui_element_ptr::create("wish"_key, "MenuButton"_key);
  menu["label"_key] = std::string{"..."};
  assign_id(menu);
  obj_ids.push_back(wish_id_of(menu));

  std::vector<ui_element_ptr> menu_kids;
  for (auto& it : items) {
    ui_element_ptr mi = ui_element_ptr::create("wish"_key, "MenuItem"_key);
    mi["label"_key] = it.label;
    assign_id(mi);
    obj_ids.push_back(wish_id_of(mi));
    menu_action_targets_[wish_id_of(mi)] = row_action{scope, meta.id, it.action};
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

void curl_frontend::set_status(list_window& lw, const std::string& text, bool ok) {
  if (!lw.status_label)
    return;
  lw.status_label["text"_key] = text;
  lw.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  lw.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

// ── Confirmation modal (docker_frontend::show_confirm() port) ───────────

void curl_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
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

// ── Request builder state ────────────────────────────────────────────────

dynamic curl_frontend::collect_request_state() const {
  dynamic d;
  int32_t m_idx = method_combo_ ? method_combo_->as<int32_t>("value"_key) : 0;
  if (m_idx < 0 || static_cast<size_t>(m_idx) >= kNumMethods)
    m_idx = 0;
  d["method"_key] = std::string{kMethods[m_idx]};
  d["url"_key] = url_input_ ? url_input_->as<std::string>("value"_key) : std::string{};
  d["params"_key] = kv_table_read(params_);
  d["headers"_key] = kv_table_read(headers_);

  int32_t bm_idx = body_mode_combo_ ? body_mode_combo_->as<int32_t>("value"_key) : 0;
  if (bm_idx < 0 || static_cast<size_t>(bm_idx) >= kNumBodyModes)
    bm_idx = 0;
  d["body_mode"_key] = std::string{kBodyModes[bm_idx]};
  d["body_text"_key] = body_text_input_ ? body_text_input_->as<std::string>("value"_key) : std::string{};
  d["form_fields"_key] = kv_table_read(body_form_);

  int32_t am_idx = auth_mode_combo_ ? auth_mode_combo_->as<int32_t>("value"_key) : 0;
  if (am_idx < 0 || static_cast<size_t>(am_idx) >= kNumAuthModes)
    am_idx = 0;
  d["auth_mode"_key] = std::string{kAuthModes[am_idx]};
  d["auth_username"_key] = auth_username_input_ ? auth_username_input_->as<std::string>("value"_key) : std::string{};
  d["auth_password"_key] = auth_password_input_ ? auth_password_input_->as<std::string>("value"_key) : std::string{};
  d["auth_token"_key] = auth_token_input_ ? auth_token_input_->as<std::string>("value"_key) : std::string{};

  d["follow_redirects"_key] = follow_redirects_;

  int32_t env_idx = env_combo_ ? env_combo_->as<int32_t>("value"_key) : 0;
  std::string env_id;
  if (env_idx >= 1 && static_cast<size_t>(env_idx - 1) < environment_ids_.size())
    env_id = environment_ids_[static_cast<size_t>(env_idx - 1)];
  d["environment"_key] = env_id;

  return d;
}

void curl_frontend::set_status_line(const std::string& text, const char* light, const char* dark) {
  if (status_line_label_) {
    status_line_label_["text"_key] = text;
    status_line_label_["text_color_light"_key] = std::string{light};
    status_line_label_["text_color_dark"_key] = std::string{dark};
  }
  if (response_status_label_) {
    response_status_label_["text"_key] = text;
    response_status_label_["text_color_light"_key] = std::string{light};
    response_status_label_["text_color_dark"_key] = std::string{dark};
  }
}

void curl_frontend::apply_body_mode_visibility(int32_t idx) {
  const bool show_raw = idx == 1 || idx == 2; // raw, json
  const bool show_form = idx == 3;            // form
  if (body_raw_box_)
    body_raw_box_["visible"_key] = show_raw;
  if (body_form_box_)
    body_form_box_["visible"_key] = show_form;
}

void curl_frontend::apply_auth_mode_visibility(int32_t idx) {
  if (auth_basic_box_)
    auth_basic_box_["visible"_key] = idx == 1;
  if (auth_bearer_box_)
    auth_bearer_box_["visible"_key] = idx == 2;
}

void curl_frontend::rebuild_response_headers(const dynamic& args) {
  if (!response_headers_table_)
    return;
  auto* children_p = response_headers_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto id : response_header_row_ids_)
    ctx().objects.erase(id.id);
  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  response_header_row_ids_.clear();
  next_response_header_key_ = 0;

  for_each_entry(args, "headers"_key, [&](const dynamic& e) {
    std::vector<ui_element_ptr> cells = {
        make_label(e.as<std::string>("key"_key)), make_label(e.as<std::string>("value"_key))};
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    assign_id(row);
    for (auto& c : cells)
      response_header_row_ids_.push_back(wish_id_of(c));
    response_header_row_ids_.push_back(wish_id_of(row));
    set_children_list(row, cells);
    (*children)[next_response_header_key_++] = dynamic_ptr{row};
  });
  response_headers_table_->refresh_children_order();
}

// ── RMI methods ────────────────────────────────────────────────────────

dynamic curl_frontend::do_update_response(const dynamic& args) {
  const bool ok = args.as<bool>("ok"_key);
  const std::string error = args.findField<std::string>("error"_key) ? args.as<std::string>("error"_key) : std::string{};

  if (!ok && !error.empty()) {
    set_status_line("Request failed: " + error, kBadLight, kBadDark);
    rebuild_response_headers(dynamic{});
    if (response_body_editor_)
      response_body_editor_["file_path"_key] = std::string{};
    return dynamic{};
  }

  const int32_t code = args.as<int32_t>("status_code"_key);
  const std::string status_text = args.as<std::string>("status_text"_key);
  const float time_ms = args.as<float>("time_ms"_key);
  const float size_bytes = args.as<float>("size_bytes"_key);

  const char* cl = kIdleLight;
  const char* cd = kIdleDark;
  if (code >= 200 && code < 300) {
    cl = kOkLight;
    cd = kOkDark;
  } else if (code >= 300 && code < 400) {
    cl = kWarnLight;
    cd = kWarnDark;
  } else if (code >= 400) {
    cl = kBadLight;
    cd = kBadDark;
  }

  std::ostringstream oss;
  oss << code << " " << status_text << "    " << format_ms(time_ms) << "    " << format_bytes(size_bytes);
  set_status_line(oss.str(), cl, cd);

  rebuild_response_headers(args);

  if (response_body_editor_) {
    const std::string body_file =
        args.findField<std::string>("body_file"_key) ? args.as<std::string>("body_file"_key) : std::string{};
    const bool is_json = args.findField<bool>("body_is_json"_key) ? args.as<bool>("body_is_json"_key) : false;
    response_body_editor_["file_path"_key] = body_file;
    response_body_editor_["language"_key] = std::string{is_json ? "json" : "none"};
  }
  return dynamic{};
}

dynamic curl_frontend::do_update_request_builder(const dynamic& args) {
  if (method_combo_)
    method_combo_["value"_key] = index_of(kMethods, kNumMethods, args.as<std::string>("method"_key));
  if (url_input_)
    url_input_["value"_key] = args.as<std::string>("url"_key);
  kv_table_load(params_, args, "params"_key);
  kv_table_load(headers_, args, "headers"_key);

  const int32_t body_idx = index_of(kBodyModes, kNumBodyModes, args.as<std::string>("body_mode"_key));
  if (body_mode_combo_)
    body_mode_combo_["value"_key] = body_idx;
  if (body_text_input_)
    body_text_input_["value"_key] = args.as<std::string>("body_text"_key);
  kv_table_load(body_form_, args, "form_fields"_key);

  const int32_t auth_idx = index_of(kAuthModes, kNumAuthModes, args.as<std::string>("auth_mode"_key));
  if (auth_mode_combo_)
    auth_mode_combo_["value"_key] = auth_idx;
  if (auth_username_input_)
    auth_username_input_["value"_key] = args.as<std::string>("auth_username"_key);
  if (auth_password_input_)
    auth_password_input_["value"_key] = args.as<std::string>("auth_password"_key);
  if (auth_token_input_)
    auth_token_input_["value"_key] = args.as<std::string>("auth_token"_key);

  apply_body_mode_visibility(body_idx);
  apply_auth_mode_visibility(auth_idx);
  return dynamic{};
}

dynamic curl_frontend::do_update_history(const dynamic& args) {
  clear_list_rows(history_, history_rows_, next_history_key_);

  int total = 0;
  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    const std::string id = e.as<std::string>("id"_key);
    const std::string method = e.as<std::string>("method"_key);
    const std::string url = e.as<std::string>("url"_key);
    const int32_t code = e.as<int32_t>("status_code"_key);
    const bool ok = e.as<bool>("ok"_key);
    const float time_ms = e.as<float>("time_ms"_key);
    const std::string when = e.as<std::string>("timestamp"_key);
    const char* cl = ok ? kOkLight : kBadLight;
    const char* cd = ok ? kOkDark : kBadDark;

    list_row meta;
    meta.id = id;
    std::vector<ui_element_ptr> cells = {
        make_label(method),
        make_label(url),
        make_label(code > 0 ? std::to_string(code) : std::string{"--"}, cl, cd),
        make_label(format_ms(time_ms)),
        make_label(when, kIdleLight, kIdleDark),
    };
    add_list_row(history_, history_rows_, next_history_key_, std::move(meta), cells, {{"Load", "load"}}, "history");
    ++total;
  });

  if (history_.table)
    history_.table->refresh_children_order();
  set_status(history_, std::to_string(total) + (total == 1 ? " request" : " requests"), true);
  return dynamic{};
}

dynamic curl_frontend::do_update_collections(const dynamic& args) {
  clear_list_rows(collections_, collection_rows_, next_collection_key_);

  int total = 0;
  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    list_row meta;
    meta.id = e.as<std::string>("id"_key);
    std::vector<ui_element_ptr> cells = {
        make_label(e.as<std::string>("collection"_key)),
        make_label(e.as<std::string>("name"_key)),
        make_label(e.as<std::string>("method"_key)),
        make_label(e.as<std::string>("url"_key)),
    };
    add_list_row(
        collections_, collection_rows_, next_collection_key_, std::move(meta), cells,
        {{"Load", "load"}, {"Duplicate", "duplicate"}, {"Delete", "delete"}}, "collections");
    ++total;
  });

  if (collections_.table)
    collections_.table->refresh_children_order();
  set_status(collections_, std::to_string(total) + (total == 1 ? " saved request" : " saved requests"), true);
  return dynamic{};
}

dynamic curl_frontend::do_update_environments(const dynamic& args) {
  clear_list_rows(environments_, environment_rows_, next_environment_key_);
  environment_ids_.clear();
  std::string items = "No Environment";

  int total = 0;
  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    const std::string id = e.as<std::string>("id"_key);
    const std::string name = e.as<std::string>("name"_key);
    const int32_t count = e.as<int32_t>("var_count"_key);
    environment_ids_.push_back(id);
    items += "\n" + name;

    list_row meta;
    meta.id = id;
    std::vector<ui_element_ptr> cells = {make_label(name), make_label(std::to_string(count))};
    add_list_row(
        environments_, environment_rows_, next_environment_key_, std::move(meta), cells,
        {{"Edit", "edit"}, {"Delete", "delete"}}, "environments");
    ++total;
  });

  if (environments_.table)
    environments_.table->refresh_children_order();
  set_status(environments_, std::to_string(total) + (total == 1 ? " environment" : " environments"), true);

  if (env_combo_) {
    int32_t cur = env_combo_->as<int32_t>("value"_key);
    env_combo_["items"_key] = items;
    if (cur < 0 || static_cast<size_t>(cur) > environment_ids_.size())
      cur = 0;
    env_combo_["value"_key] = cur;
  }
  return dynamic{};
}

dynamic curl_frontend::do_update_environment_vars(const dynamic& args) {
  open_environment_id_ = args.as<std::string>("environment_id"_key);
  if (env_editor_label_) {
    env_editor_label_["text"_key] = open_environment_id_.empty()
        ? std::string{"(select an environment to edit its variables)"}
        : "Variables for: " + args.as<std::string>("name"_key);
  }
  kv_table_load(env_vars_, args, "vars"_key);
  return dynamic{};
}

// ── Console window (client `curl` subprocess trace) ──────────────────────

void curl_frontend::append_console_row(
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
      wish_id_of(row),        wish_id_of(cell_seq),      wish_id_of(cell_command), wish_id_of(cell_exit),
      wish_id_of(cell_output), wish_id_of(context_menu), wish_id_of(copy_item),    wish_id_of(clear_item)};
  (*children)[entry.child_key] = dynamic_ptr{row};
  console_rows_.push_back(std::move(entry));

  if (console_rows_.size() > kMaxConsoleRows) {
    erase_console_row_objects(console_rows_.front());
    children->erase(console_rows_.front().child_key);
    console_rows_.pop_front();
  }
  console_table_->refresh_children_order();
}

void curl_frontend::erase_console_row_objects(const console_row_entry& entry) {
  for (auto id : entry.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

void curl_frontend::clear_console_rows() {
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
  next_console_child_key_ = 0;
  console_table_->refresh_children_order();
}

dynamic curl_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      args.as<std::string>("command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      args.as<std::string>("output"_key));
  return dynamic{};
}

// ── Event routing ─────────────────────────────────────────────────────

void curl_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  if (event == "closed"_key &&
      (id == request_window_id_ || id == response_window_id_ || id == history_.window_id ||
       id == collections_.window_id || id == environments_.window_id || id == console_window_id_)) {
    emit("closed"_key);
    remove_objects_at(response_root_key_);
    remove_objects_at(history_root_key_);
    remove_objects_at(collections_root_key_);
    remove_objects_at(environments_root_key_);
    remove_objects_at(console_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    if (id == body_mode_combo_id_) {
      apply_body_mode_visibility(payload.as<int32_t>("value"_key));
    } else if (id == auth_mode_combo_id_) {
      apply_auth_mode_visibility(payload.as<int32_t>("value"_key));
    } else if (id == follow_redirects_id_) {
      follow_redirects_ = payload.as<bool>("value"_key);
    }
    return;
  }

  if (event != "clicked"_key)
    return;

  if (auto ch = click_handlers_.find(id); ch != click_handlers_.end()) {
    ch->second();
    return;
  }

  if (auto kv_it = kv_remove_targets_.find(id); kv_it != kv_remove_targets_.end()) {
    kv_table_remove_row(*kv_it->second, id);
    return;
  }

  auto mi = menu_action_targets_.find(id);
  if (mi == menu_action_targets_.end())
    return;
  const row_action target = mi->second;

  if (target.scope == "history") {
    if (target.action == "load")
      emit("load_history_requested"_key, payload1("id"_key, target.id));
    return;
  }

  if (target.scope == "collections") {
    if (target.action == "load") {
      emit("load_request_requested"_key, payload1("id"_key, target.id));
    } else if (target.action == "duplicate") {
      emit("duplicate_request_requested"_key, payload1("id"_key, target.id));
    } else if (target.action == "delete") {
      show_confirm(
          "Delete this saved request?", [this, id = target.id] { emit("delete_request_requested"_key, payload1("id"_key, id)); });
    }
    return;
  }

  if (target.scope == "environments") {
    if (target.action == "edit") {
      emit("select_environment_requested"_key, payload1("id"_key, target.id));
    } else if (target.action == "delete") {
      show_confirm("Delete this environment?", [this, id = target.id] {
        emit("delete_environment_requested"_key, payload1("id"_key, id));
      });
    }
    return;
  }
}

// ── Registration ─────────────────────────────────────────────────────

void register_curl() {
  auto proto = dynamic_ptr{"CurlFrontend"_key, {}};

  proto->addField("title"_key, field{std::string{"Curl"}});

  auto add_method = [&](key_t name, dynamic (curl_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<curl_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_response"_key, &curl_frontend::do_update_response);
  add_method("update_request_builder"_key, &curl_frontend::do_update_request_builder);
  add_method("update_history"_key, &curl_frontend::do_update_history);
  add_method("update_collections"_key, &curl_frontend::do_update_collections);
  add_method("update_environments"_key, &curl_frontend::do_update_environments);
  add_method("update_environment_vars"_key, &curl_frontend::do_update_environment_vars);
  add_method("append_command_log"_key, &curl_frontend::do_append_command_log);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("CurlFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Postman-style GUI frontend for the local `curl` binary. All `curl` invocation happens client-side; "
      "this form only renders whatever snapshot it was last given. Listen for the 'closed' event to detect "
      "when the user is done, and the '*_requested' events to react to user actions -- see curl.hpp's class "
      "doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<curl_frontend>("wish"_key, "CurlFrontend"_key));
}

} // namespace bdg::wish
