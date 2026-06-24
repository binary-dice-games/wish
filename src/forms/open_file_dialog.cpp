// MIT License © 2025 Binary Dice Games
/// @file open_file_dialog.cpp
/// @brief Implementation of the OpenFileDialog form.
#include "open_file_dialog.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <wish/file_service.hpp>
#include <wish/ui_importer.hpp>

namespace bdg::wish {

using namespace bison;

// ── Hardcoded UI layout ───────────────────────────────────────────────────────

// TableColumns are stored as NAMED children so dynamic::clear() on the
// children dynamic removes only the indexed row entries, not the columns.
// Placeholder field values (title, btn_open label) are overwritten in on_init().
static constexpr const char* kDialogLayout = R"({
  "type": "Window",
  "width": 480, "height": 360,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "path_label": { "type": "Label", "text": "" },
        "file_table": {
          "type": "Table",
          "columns": 2,
          "headers": true,
          "children": {
            "col_name": { "type": "TableColumn", "label": "Name" },
            "col_type": { "type": "TableColumn", "label": "Type" }
          }
        },
        "filename_row": {
          "type": "HorizontalLayout",
          "children": {
            "filename_input": {
              "type": "InputText", "value": "", "hint": "filename"
            }
          }
        },
        "filter_row": {
          "type": "HorizontalLayout",
          "visible": false,
          "children": {
            "filter_combo": { "type": "Combo", "label": "" }
          }
        },
        "btn_row": {
          "type": "HorizontalLayout",
          "children": {
            "btn_open":   { "type": "Button", "label": "Open"   },
            "btn_cancel": { "type": "Button", "label": "Cancel" }
          }
        }
      }
    }
  }
})";

// ── open_file_dialog ──────────────────────────────────────────────────────────

open_file_dialog::open_file_dialog(dynamic&& base)
    : form(std::move(base)) {}

// ── on_init ───────────────────────────────────────────────────────────────────

void open_file_dialog::on_init() {
  internal_root_key_ = "__form_" +
      std::to_string(reinterpret_cast<uintptr_t>(this));

  auto tree = import_json(kDialogLayout);

  // Stamp form-field values onto the imported tree.
  auto* title_f = findField("title"_key);
  (*tree[""])["title"_key] =
      title_f ? title_f->as<std::string>() : std::string{"Open File"};

  auto* confirm_f = findField("confirm_label"_key);
  if (auto it = tree.find("vbox.btn_row.btn_open"); it != tree.end()) {
    (*it->second)["label"_key] =
        confirm_f ? confirm_f->as<std::string>() : std::string{"Open"};
  }

  // Assign each imported element a bison RMI ID so the renderer can emit
  // events with the correct object ID. Mirrors the template_handler pattern.
  auto& objects = ctx().objects;
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    objects[id.id] = elem;
    (*elem)["__wish_id"_key] = id;
  }

  // Cache pointers to widgets that need runtime access.
  if (auto it = tree.find("vbox.file_table"); it != tree.end()) {
    file_table_ptr_ = it->second;
    file_table_id_  = (*it->second)["__wish_id"_key].as<key_t>();
  }
  if (auto it = tree.find("vbox.filename_row.filename_input"); it != tree.end()) {
    filename_input_ptr_ = it->second;
    filename_input_id_  = (*it->second)["__wish_id"_key].as<key_t>();
  }
  if (auto it = tree.find("vbox.filter_row"); it != tree.end())
    filter_row_ptr_ = it->second;
  if (auto it = tree.find("vbox.filter_row.filter_combo"); it != tree.end()) {
    filter_combo_ptr_ = it->second;
    filter_combo_id_  = (*it->second)["__wish_id"_key].as<key_t>();
  }
  if (auto it = tree.find("vbox.btn_row.btn_open"); it != tree.end()) {
    btn_open_id_  = (*it->second)["__wish_id"_key].as<key_t>();
    btn_open_ptr_ = it->second;
  }
  if (auto it = tree.find("vbox.btn_row.btn_cancel"); it != tree.end())
    btn_cancel_id_ = (*it->second)["__wish_id"_key].as<key_t>();

  // Merge the imported name_map into session.objects under our prefix.
  auto& objs = sess().objects;
  for (auto& [key, ptr] : tree) {
    objs[key.empty() ? internal_root_key_
                     : (internal_root_key_ + "." + key)] = ptr;
  }

  // Intercept events from internal widgets. Unrecognised events are forwarded
  // to the client via the original emit function.
  auto base_emit          = std::move(sess().emit_event);
  auto table_id           = file_table_id_;
  auto btn_open_id        = btn_open_id_;
  auto btn_cancel_id      = btn_cancel_id_;
  auto filename_input_id  = filename_input_id_;
  auto filter_combo_id    = filter_combo_id_;

  // Capture `this` by raw pointer; the lambda lives inside sess().emit_event,
  // which the server destroys before releasing ctx.objects (which holds `this`).
  sess().emit_event =
      [this, base_emit, table_id, btn_open_id, btn_cancel_id,
       filename_input_id, filter_combo_id]
      (key_t id, key_t event, dynamic payload) {
        if (id.id == table_id.id) {
          if (event == "row_selected"_key)  { on_row_selected(payload);  return; }
          if (event == "row_activated"_key) { on_row_activated(payload); return; }
        }
        if (id.id == btn_open_id.id && event == "clicked"_key) {
          on_btn_open_clicked(); return;
        }
        if (id.id == btn_cancel_id.id && event == "clicked"_key) {
          on_btn_cancel_clicked(); return;
        }
        if (id.id == filename_input_id.id && event == "changed"_key) {
          on_filename_input_changed(payload); return;
        }
        if (id.id == filter_combo_id.id && event == "changed"_key) {
          on_filter_combo_changed(payload); return;
        }
        if (base_emit)
          base_emit(id, event, std::move(payload));
      };
}

// ── Event and field handlers ──────────────────────────────────────────────────

bison::dynamic open_file_dialog::on_set(const bison::dynamic& patch) {
  if (auto* f = patch.findField("files"_key)) {
    if (f->is<dynamic_ptr>() && f->as<dynamic_ptr>())
      rebuild_file_rows(*f->as<dynamic_ptr>());
  }
  if (auto* f = patch.findField("filters"_key)) {
    if (f->is<dynamic_ptr>() && f->as<dynamic_ptr>())
      rebuild_filter_combo(*f->as<dynamic_ptr>());
  }
  if (auto* f = patch.findField("confirm_label"_key)) {
    if (f->is<std::string>() && btn_open_ptr_)
      (*btn_open_ptr_)["label"_key] = f->as<std::string>();
  }
  return patch;
}

void open_file_dialog::rebuild_file_rows(const bison::dynamic& files) {
  if (!file_table_ptr_) return;

  auto* children_f = file_table_ptr_->findField("children"_key);
  if (!children_f || !children_f->is<dynamic_ptr>()) return;
  auto& children = children_f->as<dynamic_ptr>();

  // Remove previous row entries (indexed); named TableColumn children remain.
  children->clear();

  int32_t row_idx = 0;
  files.forEach([&](key_t, const field& entry_field) {
    if (!entry_field.is<dynamic_ptr>() || !entry_field.as<dynamic_ptr>()) return;
    const auto& entry = *entry_field.as<dynamic_ptr>();

    auto name = entry.as<std::string>("name"_key);
    auto type = entry.as<std::string>("type"_key);

    auto row = std::make_shared<ui_element>(
        dynamic::instantiate("wish"_key, "TableRow"_key));
    (*row)["order"_key] = row_idx;

    auto row_children = dynamic_ptr{key_t{0U}, {}};

    auto name_lbl = std::make_shared<ui_element>(
        dynamic::instantiate("wish"_key, "Label"_key));
    (*name_lbl)["text"_key]  = name;
    (*name_lbl)["order"_key] = int32_t{0};

    auto type_lbl = std::make_shared<ui_element>(
        dynamic::instantiate("wish"_key, "Label"_key));
    (*type_lbl)["text"_key]  = type;
    (*type_lbl)["order"_key] = int32_t{1};

    (*row_children)[size_t{0}] = dynamic_ptr{name_lbl};
    (*row_children)[size_t{1}] = dynamic_ptr{type_lbl};
    (*row)["children"_key] = row_children;

    (*children)[size_t{static_cast<size_t>(row_idx)}] = dynamic_ptr{row};
    ++row_idx;
  });
}

void open_file_dialog::rebuild_filter_combo(const bison::dynamic& filters) {
  if (!filter_combo_ptr_ || !filter_row_ptr_) return;

  std::string items;
  bool first = true;
  filters.forEach([&](key_t, const field& f) {
    if (!f.is<std::string>()) return;
    if (!first) items += '\n';
    items += f.as<std::string>();
    first = false;
  });

  bool has_filters = !items.empty();
  (*filter_row_ptr_)["visible"_key] = has_filters;
  (*filter_combo_ptr_)["items"_key] = items;
}

void open_file_dialog::on_filename_input_changed(const bison::dynamic& payload) {
  if (auto* f = payload.findField("value"_key)) {
    if (f->is<std::string>())
      (*this)["filename"_key] = f->as<std::string>();
  }
}

void open_file_dialog::on_filter_combo_changed(const bison::dynamic& payload) {
  if (auto* f = payload.findField("value"_key)) {
    if (f->is<int32_t>())
      selected_filter_idx_ = f->as<int32_t>();
  }
}

void open_file_dialog::on_row_selected(const bison::dynamic& payload) {
  int32_t idx = payload.as<int32_t>("index"_key);

  auto* files_f = findField("files"_key);
  if (!files_f || !files_f->is<dynamic_ptr>() || !files_f->as<dynamic_ptr>())
    return;
  const auto& files = *files_f->as<dynamic_ptr>();

  if (static_cast<size_t>(idx) >= files.size()) return;

  const auto& entry_field = files.at(size_t{static_cast<size_t>(idx)});
  if (!entry_field.is<dynamic_ptr>() || !entry_field.as<dynamic_ptr>()) return;
  const auto& entry = *entry_field.as<dynamic_ptr>();

  auto name = entry.as<std::string>("name"_key);

  // Update the form's public filename field.
  (*this)["filename"_key] = name;

  // Mirror the value into the internal InputText widget.
  if (filename_input_ptr_)
    (*filename_input_ptr_)["value"_key] = name;
}

void open_file_dialog::on_btn_open_clicked() {
  auto* fn_f = findField("filename"_key);
  if (!fn_f || !fn_f->is<std::string>()) return;
  const auto& filename = fn_f->as<std::string>();

  auto resolved = file_service::resolve_path(
      filename, sess().resource_dir, sess().allow_absolute_paths);
  if (resolved.empty()) return;

  bison::dynamic payload;
  payload["path"_key] = filename;
  emit("on_open"_key, std::move(payload));
  // Remove the internal window from session.objects so the dialog disappears,
  // matching the conventional "close on confirm" behavior of a file picker.
  remove_internal_objects();
}

void open_file_dialog::on_btn_cancel_clicked() {
  emit("on_cancel"_key);
  remove_internal_objects();
}

void open_file_dialog::on_row_activated(const bison::dynamic& payload) {
  int32_t idx = payload.as<int32_t>("index"_key);

  auto* files_f = findField("files"_key);
  if (!files_f || !files_f->is<dynamic_ptr>() || !files_f->as<dynamic_ptr>())
    return;
  const auto& files = *files_f->as<dynamic_ptr>();

  if (static_cast<size_t>(idx) >= files.size()) return;

  const auto& entry_field = files.at(size_t{static_cast<size_t>(idx)});
  if (!entry_field.is<dynamic_ptr>() || !entry_field.as<dynamic_ptr>()) return;
  const auto& entry = *entry_field.as<dynamic_ptr>();

  auto name = entry.as<std::string>("name"_key);
  auto type = entry.as<std::string>("type"_key);

  if (type == "dir") {
    bison::dynamic nav;
    nav["name"_key] = name;
    nav["type"_key] = std::string{"dir"};
    emit("on_navigate"_key, std::move(nav));
  } else {
    auto resolved = file_service::resolve_path(
        name, sess().resource_dir, sess().allow_absolute_paths);
    if (resolved.empty()) return;
    bison::dynamic open;
    open["path"_key] = name;
    emit("on_open"_key, std::move(open));
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_open_file_dialog() {
  auto proto = dynamic_ptr{"OpenFileDialog"_key, {}};

  proto->addField("title"_key, field{std::string{"Open File"},
    attr<DisplayName>("Title"),
    attr<Description>("Dialog window title."),
    attr<Category>("Appearance")});

  proto->addField("files"_key, field{dynamic_ptr{key_t{0U}, {}},
    attr<DisplayName>("Files"),
    attr<Description>(
        "Ordered list of entries. Each entry is a dynamic with "
        "name (string) and type (\"file\" or \"dir\")."),
    attr<Category>("Data")});

  proto->addField("filename"_key, field{std::string{""},
    attr<DisplayName>("Filename"),
    attr<Description>(
        "Current value of the filename input. Client may preset it; "
        "the form updates it as the user types or clicks a row."),
    attr<Category>("Data")});

  proto->addField("filters"_key, field{dynamic_ptr{key_t{0U}, {}},
    attr<DisplayName>("Filters"),
    attr<Description>(
        "Optional list of filter strings shown in a combo box "
        "(e.g. {0: \"*.txt\", 1: \"*.md\"}). "
        "Empty means no filter UI is shown."),
    attr<Category>("Data")});

  proto->addField("confirm_label"_key, field{std::string{"Open"},
    attr<DisplayName>("Confirm Label"),
    attr<Description>("Label on the confirm button."),
    attr<Category>("Appearance")});

  // The __setter hook intercepts every set() call to synchronize internal
  // widgets (Table rows, btn_open label) with updated field values.
  proto->addMethod("__setter"_key, bison::method{
    [](dynamic& s, const dynamic& p) -> dynamic {
      return static_cast<open_file_dialog&>(s).on_set(p);
    }
  });

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("OpenFileDialog"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "High-level file-picker dialog. The client supplies the file list "
      "and reacts to on_open, on_navigate, and on_cancel events."));
  dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      dynamic::make_factory<open_file_dialog>(
          "wish"_key, "OpenFileDialog"_key));
}

}  // namespace bdg::wish
