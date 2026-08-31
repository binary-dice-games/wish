// MIT License © 2025 Binary Dice Games
/// @file editor.cpp
/// @brief Implementation of the Editor form.
#include "editor.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/forms/message_box.hpp>
#include <ui/ui_importer.hpp>
#include <ui/ui_schema_help.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// True when @p path names a YAML source file (case-insensitive `.yaml` /
// `.yml` extension). Anything else -- including no extension -- is treated
// as JSON, matching the module's historical JSON-only default.
bool is_yaml_source_path(const std::string& path) {
  std::string ext = std::filesystem::path{path}.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".yaml" || ext == ".yml";
}

// Event names emitted by wish's built-in element renderers (see
// src/imgui/imgui_*_renderer.cpp) aren't registered in bison's key-name
// registry (they're plain "_key" literals, not "_rkey"), so there is no
// generic hash -> string reversal available. This table covers the known
// vocabulary; anything else falls back to a numeric placeholder rather than
// silently dropping the log entry.
const std::unordered_map<uint32_t, std::string>& known_event_names() {
  static const std::unordered_map<uint32_t, std::string> table = {
      {"clicked"_key.id, "clicked"},
      {"changed"_key.id, "changed"},
      {"saved"_key.id, "saved"},
      {"closed"_key.id, "closed"},
      {"selected"_key.id, "selected"},
      {"sorted"_key.id, "sorted"},
      {"toggled"_key.id, "toggled"},
      {"row_selected"_key.id, "row_selected"},
      {"row_activated"_key.id, "row_activated"},
  };
  return table;
}

std::string event_name_string(key_t event) {
  auto& table = known_event_names();
  if (auto it = table.find(event.id); it != table.end())
    return it->second;
  return "event#" + std::to_string(event.id);
}

std::string describe_field(const field& f) {
  if (f.is<bool>())
    return f.as<bool>() ? "true" : "false";
  if (f.is<int32_t>())
    return std::to_string(f.as<int32_t>());
  if (f.is<float>())
    return std::to_string(f.as<float>());
  if (f.is<std::string>())
    return "\"" + f.as<std::string>() + "\"";
  return "?";
}

// Payload field names used across wish's built-in element renderers (see the
// `payload["..."_key] = ...` call sites in src/imgui/imgui_ui_renderer.cpp).
// Same "_key, not "_rkey"" limitation as event names above -- there's no
// generic way to enumerate a dynamic's fields by name, so known candidates
// are checked explicitly; anything else is silently omitted rather than
// guessed at.
std::string format_payload(const dynamic& payload) {
  static const std::pair<const char*, key_t> known_fields[] = {
      {"value", "value"_key},
      {"checked", "checked"_key},
      {"open", "open"_key},
      {"text", "text"_key},
      {"selected", "selected"_key},
      {"index", "index"_key},
      {"file_path", "file_path"_key},
      {"column_id", "column_id"_key},
      {"ascending", "ascending"_key},
  };
  std::string out;
  for (auto& [name, key] : known_fields) {
    auto* f = payload.findField(key);
    if (!f)
      continue;
    if (!out.empty())
      out += ", ";
    out += std::string(name) + "=" + describe_field(*f);
  }
  return out.empty() ? std::string{} : " {" + out + "}";
}

// Builds one field's Description-column text: its own description prefixed
// by any range/enum annotations, in the same bracket format the previous
// single-Label help panel used (required/category now get their own
// column/marker instead -- see append_help_row()).
std::string format_field_description(const element_field_info& f) {
  std::string out;
  if (f.range)
    out += "[" + std::to_string(f.range->first) + "-" + std::to_string(f.range->second) + "] ";
  if (!f.enum_values.empty()) {
    out += f.is_enum_flags ? "[flags: " : "[enum: ";
    for (size_t i = 0; i < f.enum_values.size(); ++i) {
      if (i)
        out += " | ";
      out += f.enum_values[i];
    }
    out += "] ";
  }
  out += f.description;
  return out;
}

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// log's "flags": "ScrollY" -- combined with outer_height, this clips the
// log to a fixed-size scrolling region instead of letting an ever-growing
// row count push the whole Editor window taller.
//
// Closing with unsaved edits is confirmed via a privately-instantiated
// MessageBox form (form::instantiate_child_form(), same pattern as git's/
// top's confirm dialogs) -- not an inline panel -- so there is no
// close-confirmation widget in this layout.

// Uses a custom raw-string delimiter (R"json(...)json" instead of R"(...)")
// because the content below contains a literal `)"` (in "Filename: (none)"),
// which would otherwise terminate a plain R"(...)" raw string early and
// silently corrupt the rest of the translation unit.
static constexpr const char* kEditorLayout = R"json({
  "type": "Window",
  "title": "Editor",
  "width": 900,
  "height": 720,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "path_label": { "type": "Label", "text": "Filename: (none)" },
        "banner": { "type": "Label", "text": "" },
        "editor_row": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "height": -1,
          "children": {
            "source": { "type": "TextEditor", "language": "json", "width": -1, "height": -1, "wish_ui_schema": true }
          }
        }
      }
    }
  }
})json";

// The event log window: a second, independently dockable top-level Window
// (see log_root_key_'s doc comment) showing every event a preview widget
// fires. "outer_height": -1 on "log" bottom-aligns the table to fill this
// window's own available height (extern/imgui/imgui_tables.cpp's
// BeginTable() doc comment) instead of the fixed 200px strip it used to
// clip to inside the shared chrome window -- ImGuiTableFlags_ScrollY
// (flags: 33554432) and the auto-scroll-to-bottom-on-growth behavior below
// both still work, since they only need a bounded (not necessarily fixed)
// outer size.
static constexpr const char* kLogWindowLayout = R"json({
  "type": "Window",
  "title": "Event Log",
  "pos_x": 20,
  "pos_y": 760,
  "width": 900,
  "height": 260,
  "children": {
    "log": {
      "type": "Table",
      "id": "##editor_log",
      "columns": 2,
      "headers": true,
      "outer_height": -1,
      "flags": "ScrollY",
      "children": {
        "col_seq":   { "type": "TableColumn", "label": "#" },
        "col_event": { "type": "TableColumn", "label": "Event" }
      }
    }
  }
})json";

// The Help window: a second, independently dockable top-level Window (see
// help_root_key_'s doc comment) showing whatever element type encloses the
// source editor's cursor -- a title/description pair above a 3-column field
// table (Field / Category / Description), rebuilt by update_help_panel().
// "flags": "Resizable|RowBg|BordersV" on "fields"; col_field/col_category's
// "flags": "WidthFixed", col_desc's "flags": "WidthStretch" (fills whatever
// width remains).
static constexpr const char* kHelpWindowLayout = R"json({
  "type": "Window",
  "title": "Help",
  "pos_x": 930,
  "pos_y": 20,
  "width": 380,
  "height": 680,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "class_name": { "type": "Label", "text": "", "font_size": 20 },
        "class_desc": { "type": "Label", "text": "", "wrap": true },
        "sep": { "type": "Separator" },
        "fields": {
          "type": "Table",
          "id": "##help_fields",
          "columns": 3,
          "headers": true,
          "flags": "Resizable|RowBg|BordersV",
          "children": {
            "col_field":    { "type": "TableColumn", "label": "Field", "column_id": 0, "flags": "WidthFixed", "init_width": 150 },
            "col_category": { "type": "TableColumn", "label": "Category", "column_id": 1, "flags": "WidthFixed", "init_width": 90 },
            "col_desc":     { "type": "TableColumn", "label": "Description", "column_id": 2, "flags": "WidthStretch" }
          }
        }
      }
    }
  }
})json";

// Accent color for the field-name column's text (a light blue, readable on
// both the dark and light theme presets since Label.text_color is applied
// via PushStyleColor -- an explicit override, not a theme-derived one).
static constexpr const char* kHelpFieldNameColor = "#7EC8FFFF";

// ── editor ───────────────────────────────────────────────────────────────────

editor::editor(dynamic&& base) : form(std::move(base)) {}

void editor::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__editor_");
  mock_root_key_ = internal_root_key_ + "_mock";
  help_root_key_ = internal_root_key_ + "_help";
  log_root_key_ = internal_root_key_ + "_log";

  auto tree = import_json(kEditorLayout);

  // Assign every chrome element a bison RMI ID, mirroring bc/nano.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.editor_row.source", [&](const auto& e) {
    source_editor_id_ = wish_id_of(e);
    source_editor_ptr_ = e;
  });
  tree.with("vbox.banner", [&](const auto& e) { banner_ptr_ = e; });
  tree.with("vbox.path_label", [&](const auto& e) { path_label_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);

  // Help window: a second, independent top-level Window -- form::init()
  // (called right after on_init() returns) only auto-registers a single
  // top_level_objects entry keyed on internal_root_key_, so this is
  // registered by hand here, the same way try_reparse() does for the
  // preview's mock_root_key_ and mc's confirm dialog does for
  // its own secondary root (see remove_objects_at()'s doc comment).
  auto help_tree = import_json(kHelpWindowLayout);
  for (auto& [key, elem] : help_tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
  help_tree.with("vbox.class_name", [&](const auto& e) { help_class_name_ptr_ = e; });
  help_tree.with("vbox.class_desc", [&](const auto& e) { help_class_desc_ptr_ = e; });
  help_tree.with("vbox.fields", [&](const auto& e) { help_table_ptr_ = e; });

  ui_element_ptr help_root_ptr = help_tree[""];
  sess().ui_objects.merge(std::move(help_tree), help_root_key_);
  sess().top_level_objects[key_t{help_root_key_}] = help_root_ptr;
  sess().top_level_handlers[key_t{help_root_key_}] = this;
  (*help_root_ptr)["__path__"_key] = help_root_key_;

  // Event log window: a third, independent top-level Window, registered
  // the same manual way as the Help window above.
  auto log_tree = import_json(kLogWindowLayout);
  for (auto& [key, elem] : log_tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
  log_tree.with("log", [&](const auto& e) { log_table_ptr_ = e; });

  ui_element_ptr log_root_ptr = log_tree[""];
  sess().ui_objects.merge(std::move(log_tree), log_root_key_);
  sess().top_level_objects[key_t{log_root_key_}] = log_root_ptr;
  sess().top_level_handlers[key_t{log_root_key_}] = this;
  (*log_root_ptr)["__path__"_key] = log_root_key_;
}

// ── set_source / reparse ─────────────────────────────────────────────────────

dynamic editor::do_set_source(const dynamic& args) {
  current_source_path_ = args.as<std::string>("path"_key);
  if (auto* dp = args.findField<std::string>("display_path"_key); dp && !dp->empty())
    display_path_ = *dp;
  // Pick the importer, source-panel syntax highlighting, and cursor-context
  // scanner from the file extension: the real local name if the client sent
  // one, else the sandbox name. Both formats get the same live preview,
  // schema autocomplete, and cursor-tracked Help panel.
  source_is_yaml_ = is_yaml_source_path(display_path_.empty() ? current_source_path_ : display_path_);
  if (source_editor_ptr_) {
    (*source_editor_ptr_)["file_path"_key] = current_source_path_;
    (*source_editor_ptr_)["language"_key] = std::string{source_is_yaml_ ? "yaml" : "json"};
    (*source_editor_ptr_)["wish_ui_schema"_key] = true;
  }
  // The sandbox now matches disk (initial load, or the client re-uploading
  // after an external edit) -- any prior in-editor edit is superseded.
  dirty_ = false;
  update_path_label();
  try_reparse();
  return dynamic{};
}

dynamic editor::do_mark_saved(const dynamic& /*args*/) {
  dirty_ = false;
  update_path_label();
  if (pending_close_after_save_) {
    pending_close_after_save_ = false;
    request_close();
  }
  return dynamic{};
}

void editor::with_session(const std::function<void(context&)>& fn) {
  if (detail::current_context) {
    // Called within dispatch (do_set_source): wlock already held.
    fn(*detail::current_context);
  } else {
    // Called from on_event(), which the render loop invokes outside the
    // session lock: acquire it ourselves.
    auto lock = context_wlock{*sync_ctx_};
    fn(*lock);
  }
}

void editor::try_reparse() {
  if (current_source_path_.empty())
    return;

  std::filesystem::path resolved;
  with_session([&](context& s) {
    resolved = file_service::resolve_path(current_source_path_, s.resource_dir, s.allow_absolute_paths);
  });
  if (resolved.empty()) {
    set_banner("invalid or unsafe source path: " + current_source_path_);
    return;
  }

  std::ifstream f(resolved, std::ios::binary);
  std::string content{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{}};
  // Cached unconditionally (even on a parse failure below) so the help panel
  // reflects exactly what's on screen mid-edit, same "no flicker" spirit as
  // the preview itself.
  current_source_content_ = content;

  ui_tree mock;
  try {
    mock = source_is_yaml_ ? import_yaml(content) : import_json(content);
  } catch (const std::runtime_error& e) {
    set_banner(e.what());
    return;
  }

  // Success: swap in the new preview subtree. Only reached once the new
  // tree has fully parsed, so a failed parse (handled above) never touches
  // the previous, still-valid preview -- this is what prevents flicker.
  clear_mock();

  auto& c = ctx();
  for (auto& [path, elem] : mock) {
    // Reuse the previous reparse's root id so ImGui's own per-window state
    // (position, size, focus -- keyed off this id, see mock_window_id_'s
    // doc comment) survives across reparses instead of resetting every time
    // the user edits the source.
    key_t id;
    if (path.empty() && mock_window_id_.id) {
      id = mock_window_id_;
    } else {
      id = rmi::shared::generate_id();
      if (path.empty())
        mock_window_id_ = id;
    }
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
    mock_id_to_path_[id.id] = path.empty() ? std::string{"root"} : path;
    // Reapply the highlight to whichever element still has this path in the
    // freshly-rebuilt tree -- try_reparse() tears down and rebuilds the
    // *entire* preview subtree, so any field set on the old tree is gone.
    if (highlighted_path_ && *highlighted_path_ == path)
      elem["__wish_highlight__"_key] = true;
  }

  ui_element_ptr root_ptr = mock[""];
  with_session([&](context& s) {
    s.ui_objects.merge(std::move(mock), mock_root_key_);
    s.top_level_objects[key_t{mock_root_key_}] = root_ptr;
    s.top_level_handlers[key_t{mock_root_key_}] = this;
  });

  set_banner("");
}

void editor::clear_mock() {
  if (mock_root_key_.empty())
    return;
  with_session([&](context& s) {
    s.top_level_objects.erase(key_t{mock_root_key_});
    s.top_level_handlers.erase(key_t{mock_root_key_});
    const std::string dot = mock_root_key_ + ".";
    for (auto it = s.ui_objects.begin(); it != s.ui_objects.end();) {
      if (it->first == mock_root_key_ || it->first.rfind(dot, 0) == 0)
        it = s.ui_objects.erase(it);
      else
        ++it;
    }
  });
  mock_id_to_path_.clear();
  // Deliberately NOT resetting highlighted_path_ here: clear_mock() is
  // called at the *start* of every try_reparse() (before rebuilding), and
  // highlighted_path_ must survive that so the reapplication loop below can
  // reapply it to the fresh tree. request_close() resets it explicitly once
  // the preview is gone for good.
}

void editor::set_banner(const std::string& text) {
  if (banner_ptr_)
    (*banner_ptr_)["text"_key] = text;
}

void editor::update_path_label() {
  if (!path_label_ptr_)
    return;
  std::string text = "Filename: " + (display_path_.empty() ? std::string{"(none)"} : display_path_);
  if (dirty_)
    text += " [MODIFIED]";
  (*path_label_ptr_)["text"_key] = text;
}

void editor::update_help_panel(int32_t line, int32_t column) {
  text_pos pos{static_cast<size_t>(line), static_cast<size_t>(column)};
  auto ctx = source_is_yaml_ ? scan_cursor_context_yaml(current_source_content_, pos)
                             : scan_cursor_context(current_source_content_, pos);

  update_highlight(ctx.element_path);

  // Nothing to rebuild if the enclosing type hasn't actually changed since
  // the last call -- avoids reallocating every row's RMI id/ctx().objects
  // entry on every single cursor move within the same element.
  if (ctx.enclosing_type == last_help_type_)
    return;
  last_help_type_ = ctx.enclosing_type;

  clear_help_rows();
  if (help_class_name_ptr_)
    (*help_class_name_ptr_)["text"_key] = "";
  if (help_class_desc_ptr_)
    (*help_class_desc_ptr_)["text"_key] = "";

  if (ctx.enclosing_type.empty())
    return;
  auto found = find_ui_element_class(ctx.enclosing_type);
  if (!found)
    return;

  if (help_class_name_ptr_)
    (*help_class_name_ptr_)["text"_key] = found->display_name;
  if (help_class_desc_ptr_)
    (*help_class_desc_ptr_)["text"_key] = found->description;
  for (const auto& f : found->fields)
    append_help_row(f.display_name, f.required, f.category, format_field_description(f));
  if (help_table_ptr_)
    help_table_ptr_->refresh_children_order();
}

void editor::clear_help_rows() {
  if (help_table_ptr_) {
    if (auto* children_p = help_table_ptr_->findField<dynamic_ptr>("children"_key); children_p && *children_p) {
      auto& children = *children_p;
      for (auto& row : help_rows_) {
        children->erase(row.child_key);
        ctx().objects.erase(row.row_id.id);
        ctx().objects.erase(row.field_cell_id.id);
        ctx().objects.erase(row.category_cell_id.id);
        ctx().objects.erase(row.desc_cell_id.id);
      }
    }
    help_table_ptr_->refresh_children_order();
  }
  help_rows_.clear();
  next_help_child_key_ = 0;
}

void editor::append_help_row(
    const std::string& field_name, bool required, const std::string& category, const std::string& description) {
  if (!help_table_ptr_)
    return;
  auto* children_p = help_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  row["order"_key] = static_cast<int32_t>(next_help_child_key_);

  ui_element_ptr cell_field = ui_element_ptr::create("wish"_key, "Label"_key);
  cell_field["text"_key] = field_name + (required ? " *" : "");
  cell_field["text_color"_key] = std::string{kHelpFieldNameColor};
  cell_field["order"_key] = int32_t{0};
  key_t cell_field_id = rmi::shared::generate_id();
  ctx().put_object(cell_field_id, cell_field);
  cell_field["__wish_id"_key] = cell_field_id;

  ui_element_ptr cell_category = ui_element_ptr::create("wish"_key, "Label"_key);
  cell_category["text"_key] = category.empty() ? std::string{"-"} : category;
  cell_category["order"_key] = int32_t{1};
  key_t cell_category_id = rmi::shared::generate_id();
  ctx().put_object(cell_category_id, cell_category);
  cell_category["__wish_id"_key] = cell_category_id;

  ui_element_ptr cell_desc = ui_element_ptr::create("wish"_key, "Label"_key);
  cell_desc["text"_key] = description;
  cell_desc["wrap"_key] = true;
  cell_desc["order"_key] = int32_t{2};
  key_t cell_desc_id = rmi::shared::generate_id();
  ctx().put_object(cell_desc_id, cell_desc);
  cell_desc["__wish_id"_key] = cell_desc_id;

  auto row_children = dynamic_ptr{key_t{0U}, {}};
  (*row_children)[size_t{0}] = dynamic_ptr{cell_field};
  (*row_children)[size_t{1}] = dynamic_ptr{cell_category};
  (*row_children)[size_t{2}] = dynamic_ptr{cell_desc};
  row["children"_key] = row_children;
  row->refresh_children_order();

  key_t row_id = rmi::shared::generate_id();
  ctx().put_object(row_id, row);
  row["__wish_id"_key] = row_id;

  size_t child_key = next_help_child_key_++;
  (*children)[child_key] = dynamic_ptr{row};
  help_rows_.push_back({child_key, row_id, cell_field_id, cell_category_id, cell_desc_id});
}

void editor::update_highlight(const std::optional<std::string>& new_path) {
  if (new_path == highlighted_path_)
    return;

  with_session([&](context& s) {
    auto set_flag = [&](const std::string& path, bool value) {
      std::string key = path.empty() ? mock_root_key_ : (mock_root_key_ + "." + path);
      auto it = s.ui_objects.find(key);
      if (it != s.ui_objects.end() && it->second)
        (*it->second)["__wish_highlight__"_key] = value;
    };
    if (highlighted_path_)
      set_flag(*highlighted_path_, false);
    if (new_path)
      set_flag(*new_path, true);
  });

  highlighted_path_ = new_path;
}

void editor::show_close_confirm() {
  dynamic params;
  params["title"_key] = std::string{"Unsaved changes"};
  params["message"_key] = "Save changes to " +
      (display_path_.empty() ? std::string{"this file"} : display_path_) + " before closing?";
  params["icon"_key] = std::string{"warning"};
  params["buttons"_key] = std::string{"yes_no_cancel"};

  // Privately-instantiated MessageBox, same pattern as git's/top's confirm
  // dialogs (form::instantiate_child_form()). Overwriting close_dialog_ is
  // safe even if one is somehow still open -- the stale instance's
  // destructor tears down its own internal objects.
  //   Yes -> save, then close once the client reports the save landed
  //          (do_mark_saved -> request_close via pending_close_after_save_)
  //   No  -> discard and close now
  //   Cancel / window-X -> keep editing (MessageBox closes itself)
  //
  // The callback must NOT destroy close_dialog_ (e.g. via request_close()
  // resetting it): message_box::on_event() calls its OWN request_close()
  // right after emit()-ing "on_result", so freeing the box here would be a
  // use-after-free of the frame we're returning into. request_close() only
  // tears down the editor's chrome -- the MessageBox tears itself down.
  close_dialog_ = instantiate_child_form<message_box>(
      "MessageBox"_key, std::move(params), [this](key_t /*event_name*/, const dynamic& payload) {
        const std::string button = payload.as<std::string>("button"_key);
        if (button == "yes") {
          pending_close_after_save_ = true;
          emit("on_source_saved"_key);
        } else if (button == "no") {
          request_close();
        }
      });
}

void editor::request_close() {
  clear_mock();
  highlighted_path_.reset();
  emit("closed"_key);
  remove_internal_objects();
  // remove_internal_objects() only cleans up internal_root_key_ (the main
  // chrome window) -- the Help and Event Log windows are separate
  // top-level roots and need their own explicit teardown, same as
  // mc's confirm dialog (see remove_objects_at()'s doc comment).
  remove_objects_at(help_root_key_);
  remove_objects_at(log_root_key_);
}

// ── Event log ─────────────────────────────────────────────────────────────────

void editor::append_log_row(const std::string& text) {
  if (!log_table_ptr_)
    return;
  auto* children_p = log_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  row["order"_key] = static_cast<int32_t>(next_log_child_key_);

  ui_element_ptr cell_seq = ui_element_ptr::create("wish"_key, "Label"_key);
  cell_seq["text"_key] = std::to_string(++log_seq_);
  cell_seq["order"_key] = int32_t{0};
  key_t cell_seq_id = rmi::shared::generate_id();
  ctx().put_object(cell_seq_id, cell_seq);
  cell_seq["__wish_id"_key] = cell_seq_id;

  ui_element_ptr cell_text = ui_element_ptr::create("wish"_key, "Label"_key);
  cell_text["text"_key] = text;
  cell_text["order"_key] = int32_t{1};
  key_t cell_text_id = rmi::shared::generate_id();
  ctx().put_object(cell_text_id, cell_text);
  cell_text["__wish_id"_key] = cell_text_id;

  auto row_children = dynamic_ptr{key_t{0U}, {}};
  (*row_children)[size_t{0}] = dynamic_ptr{cell_seq};
  (*row_children)[size_t{1}] = dynamic_ptr{cell_text};
  row["children"_key] = row_children;
  row->refresh_children_order();

  key_t row_id = rmi::shared::generate_id();
  ctx().put_object(row_id, row);
  row["__wish_id"_key] = row_id;

  size_t child_key = next_log_child_key_++;
  (*children)[child_key] = dynamic_ptr{row};
  log_rows_.push_back({child_key, row_id, cell_seq_id, cell_text_id});

  if (log_rows_.size() > kMaxLogRows) {
    auto& oldest = log_rows_.front();
    children->erase(oldest.child_key);
    ctx().objects.erase(oldest.row_id.id);
    ctx().objects.erase(oldest.cell_seq_id.id);
    ctx().objects.erase(oldest.cell_text_id.id);
    log_rows_.pop_front();
  }

  log_table_ptr_->refresh_children_order();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void editor::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    if (dirty_)
      show_close_confirm();
    else
      request_close();
    return;
  }

  if (id == source_editor_id_) {
    if (event == "changed"_key) {
      dirty_ = true;
      update_path_label();
      try_reparse();
      return;
    }
    if (event == "saved"_key) {
      emit("on_source_saved"_key);
      return;
    }
    if (event == "cursor_moved"_key) {
      update_help_panel(payload.as<int32_t>("line"_key), payload.as<int32_t>("column"_key));
      return;
    }
    return;
  }

  // Anything else fired within our subtrees is either a preview widget's
  // event -- log it -- or a stale id from a subtree that was just replaced
  // by try_reparse(), which is silently dropped.
  auto it = mock_id_to_path_.find(id.id);
  if (it == mock_id_to_path_.end())
    return;

  append_log_row(it->second + " " + event_name_string(event) + format_payload(payload));
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_editor() {
  auto proto = dynamic_ptr{"Editor"_key, {}};

  proto->addMethod("set_source"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<editor&>(self).do_set_source(args);
                   }});
  proto->addMethod("mark_saved"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<editor&>(self).do_mark_saved(args);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Editor"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Live JSON UI mock editor: a syntax-highlighted source panel next to a "
                        "continuously re-parsed, live-instantiated preview and an event log. "
                        "The client owns the local JSON file; call set_source(path, display_path) "
                        "after every upload_file, both at startup and whenever the local file "
                        "changes outside the tool. Listen for 'on_source_saved' (Ctrl+S, or a "
                        "confirmed close with unsaved edits) to download and persist the sandbox "
                        "file to the local path, then call mark_saved(). Listen for 'closed' to "
                        "detect when the user is actually done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<editor>("wish"_key, "Editor"_key));
}

} // namespace bdg::wish
