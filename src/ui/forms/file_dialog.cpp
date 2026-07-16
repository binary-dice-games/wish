// MIT License © 2025 Binary Dice Games
/// @file file_dialog.cpp
/// @brief Implementation of the FileDialog form.
#include "file_dialog.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cctype>
#include <regex>

namespace bdg::wish {

using namespace bison;

namespace {
template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// Maps a file/directory entry to the embedded icon (under
// resources/embedded/icons/) shown to its left in the Name column, mirroring
// the file-type icons in Windows' Open File dialog. Falls back to the
// generic "file" icon for extensions not called out below.
std::string icon_for_entry(const std::string& name, const std::string& type) {
  if (type == "dir")
    return "folder";

  auto dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 == name.size())
    return "file";
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  static const std::vector<std::string> kImageExts{"png", "jpg", "jpeg", "gif", "bmp", "webp", "tga", "svg"};
  static const std::vector<std::string> kAudioExts{"mp3", "wav", "ogg", "flac", "m4a", "aac"};
  static const std::vector<std::string> kCodeExts{
      "cpp", "hpp", "c", "h", "cc", "cs", "py", "js", "ts", "java", "go", "rs",
      "sh", "json", "yaml", "yml", "xml", "html", "css"};
  static const std::vector<std::string> kDocumentExts{"txt", "md", "pdf", "doc", "docx", "rtf", "log"};

  auto contains = [&](const std::vector<std::string>& exts) {
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
  };

  if (contains(kImageExts))
    return "image";
  if (contains(kAudioExts))
    return "audio";
  if (contains(kCodeExts))
    return "code";
  if (contains(kDocumentExts))
    return "document";
  return "file";
}
} // namespace

// ── Hardcoded UI layout ───────────────────────────────────────────────────────

// TableColumns are stored as NAMED children so dynamic::clear() on the
// children dynamic removes only the indexed row entries, not the columns.
// Placeholder field values (title, btn_open label) are overwritten in on_init().
// ImGuiTableFlags: Resizable(1) + RowBg(64) + BordersInnerH(128) +
//   BordersOuterH(256) + ScrollY(1<<25=33554432) = 33554881
// ImGuiTableColumnFlags: WidthFixed(1<<4=16)
// ImGuiInputTextFlags: EnterReturnsTrue=32
static constexpr const char* kDialogLayout = R"({
  "type": "Window",
  "width": 520, "height": 420, "modal": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "path_input": {
          "type": "InputText", "value": "", "hint": "path",
          "max_length": 1024, "flags": 32, "width": -1.0
        },
        "file_table": {
          "type": "Table",
          "columns": 2,
          "flags": 33554881,
          "outer_height": 260.0,
          "headers": true,
          "children": {
            "col_name": { "type": "TableColumn", "label": "Name",
                          "flags": 16, "init_width": 340 },
            "col_type": { "type": "TableColumn", "label": "Type",
                          "flags": 16, "init_width": 80  }
          }
        },
        "filename_input": {
          "type": "InputText", "value": "", "hint": "filename",
          "max_length": 1024, "width": -1.0
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
          "spacing": 8.0, "align": "right",
          "children": {
            "btn_open":   { "type": "Button", "label": "Open",   "width": 100 },
            "btn_cancel": { "type": "Button", "label": "Cancel", "width": 100 }
          }
        }
      }
    }
  }
})";

// ── file_dialog ───────────────────────────────────────────────────────────────

file_dialog::file_dialog(dynamic&& base) : form(std::move(base)) {}

// ── on_init ───────────────────────────────────────────────────────────────────

void file_dialog::on_init() {
  // See internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__form_file_dialog_");

  auto tree = import_json(kDialogLayout);

  // Stamp form-field values onto the imported tree.
  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Open File"};

  auto* confirm_f = findField<std::string>("confirm_label"_key);
  tree.with(
      "vbox.btn_row.btn_open", [&](const auto& e) { e["label"_key] = confirm_f ? *confirm_f : std::string{"Open"}; });

  // Assign each imported element a bison RMI ID so the renderer can emit
  // events with the correct object ID. Mirrors the ui_template pattern.
  // put_object() files each one under the current request's group (see
  // rmi::context::current_group) so they're cleaned up together with the
  // rest of this form when relayed through rmi::bridge.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();

  // Cache pointers to widgets that need runtime access.
  tree.with("vbox.file_table", [&](const auto& e) {
    file_table_ptr_ = e;
    file_table_id_ = wish_id_of(e);
  });
  tree.with("vbox.path_input", [&](const auto& e) {
    path_input_ptr_ = e;
    path_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.filename_input", [&](const auto& e) {
    filename_input_ptr_ = e;
    filename_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.filter_row", [&](const auto& e) { filter_row_ptr_ = e; });
  tree.with("vbox.filter_row.filter_combo", [&](const auto& e) {
    filter_combo_ptr_ = e;
    filter_combo_id_ = wish_id_of(e);
  });
  tree.with("vbox.btn_row.btn_open", [&](const auto& e) {
    btn_open_ptr_ = e;
    btn_open_id_ = wish_id_of(e);
  });
  tree.with("vbox.btn_row.btn_cancel", [&](const auto& e) { btn_cancel_id_ = wish_id_of(e); });

  // Merge the imported tree into context.ui_objects under our prefix.
  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

// ── Event routing ─────────────────────────────────────────────────────────────

void file_dialog::on_event(key_t id, key_t event, const dynamic& payload) {
  // The internal Window's own "closed" event fires once render_window()
  // confirms ImGui actually closed the popup requested via request_close()
  // below -- only now is it safe to tear down the tree (see
  // request_close()'s doc comment for why this can't happen immediately on
  // the click that triggers it). Mirrors message_box::on_event().
  if (id == window_id_ && event == "closed"_key) {
    remove_internal_objects();
    return;
  }

  if (id == file_table_id_) {
    if (event == "row_selected"_key) {
      on_row_selected(payload);
      return;
    }
    if (event == "row_activated"_key) {
      on_row_activated(payload);
      return;
    }
  }
  if (id == path_input_id_ && event == "changed"_key) {
    on_path_input_changed(payload);
    return;
  }
  if (id == btn_open_id_ && event == "clicked"_key) {
    on_btn_open_clicked();
    return;
  }
  if (id == btn_cancel_id_ && event == "clicked"_key) {
    on_btn_cancel_clicked();
    return;
  }
  if (id == filename_input_id_ && event == "changed"_key) {
    on_filename_input_changed(payload);
    return;
  }
  if (id == filter_combo_id_ && event == "changed"_key) {
    on_filter_combo_changed(payload);
    return;
  }
}

// ── Event and field handlers ──────────────────────────────────────────────────

bison::dynamic file_dialog::on_set(const bison::dynamic& patch) {
  if (auto* v = patch.findField<std::string>("path"_key); v && path_input_ptr_)
    path_input_ptr_["value"_key] = *v;
  // Process filters before files so the first rebuild already applies them.
  if (auto* v = patch.findField<dynamic_ptr>("filters"_key); v && *v)
    rebuild_filter_combo(**v);
  if (auto* v = patch.findField<dynamic_ptr>("files"_key); v && *v) {
    cached_files_ = *v;
    rebuild_file_rows(*cached_files_);
  }
  if (auto* v = patch.findField<std::string>("confirm_label"_key); v && btn_open_ptr_)
    btn_open_ptr_["label"_key] = *v;
  return patch;
}

void file_dialog::rebuild_file_rows(const bison::dynamic& files) {
  if (!file_table_ptr_)
    return;

  auto* children_p = file_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p)
    return;
  auto& children = *children_p;

  // Compile the active filter regex (empty string = match all).
  std::optional<std::regex> active_re;
  if (selected_filter_idx_ >= 0 && selected_filter_idx_ < static_cast<int32_t>(filter_regexes_.size())) {
    const auto& re_str = filter_regexes_[static_cast<size_t>(selected_filter_idx_)];
    if (!re_str.empty()) {
      try {
        active_re.emplace(re_str, std::regex::icase);
      } catch (...) {
      }
    }
  }

  // Remove previous row entries (indexed); named TableColumn children remain.
  children->clear();
  row_to_file_idx_.clear();

  int32_t row_idx = 0;
  size_t orig_idx = 0;
  files.forEach([&](key_t, const field& entry_field) {
    size_t this_orig = orig_idx++;
    auto* ep = entry_field.get<dynamic_ptr>();
    if (!ep || !*ep)
      return;
    const auto& entry = **ep;

    auto name = entry.as<std::string>("name"_key);
    auto type = entry.as<std::string>("type"_key);

    // Directories are always shown; files must match the active regex filter.
    if (type != "dir" && active_re && !std::regex_search(name, *active_re))
      return;

    row_to_file_idx_.push_back(this_orig);

    ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
    row["order"_key] = row_idx;

    auto row_children = dynamic_ptr{key_t{0U}, {}};

    // Name column shows a small type icon ahead of the label, Windows-Explorer
    // style. The icon deliberately does NOT set an explicit "width"/"height"
    // -- see render_image()'s "__auto_size_to_font__" handling
    // (imgui_ui_renderer.cpp) for why: it sizes itself to the current font's
    // line height at render time instead, both so the icon tracks
    // --font_size automatically and so it doesn't trip render_horizontal_layout()'s
    // width-hint BeginChild wrapping (which, for these row-generated icons
    // with no per-row __path__/__wish_id of their own, would collide every
    // row onto the same BeginChild ID).
    ui_element_ptr icon_row{dynamic::instantiate("wish"_key, "HorizontalLayout"_key)};
    icon_row["spacing"_key] = 6.0f;
    icon_row["order"_key] = int32_t{0};

    ui_element_ptr icon_img{dynamic::instantiate("wish"_key, "Image"_key)};
    icon_img["src"_key] = "res/icons/" + icon_for_entry(name, type) + ".png";
    icon_img["__auto_size_to_font__"_key] = true;
    // The icon PNGs are white/monochrome so they can be tinted; without
    // this they're invisible against the light theme's white background
    // (see render_image()'s "__tint_to_text_color__" handling).
    icon_img["__tint_to_text_color__"_key] = true;
    icon_img["order"_key] = int32_t{0};

    ui_element_ptr name_lbl{dynamic::instantiate("wish"_key, "Label"_key)};
    name_lbl["text"_key] = name;
    name_lbl["order"_key] = int32_t{1};

    auto icon_row_children = dynamic_ptr{key_t{0U}, {}};
    (*icon_row_children)[size_t{0}] = dynamic_ptr{icon_img};
    (*icon_row_children)[size_t{1}] = dynamic_ptr{name_lbl};
    icon_row["children"_key] = icon_row_children;

    ui_element_ptr type_lbl{dynamic::instantiate("wish"_key, "Label"_key)};
    type_lbl["text"_key] = type;
    type_lbl["order"_key] = int32_t{1};

    (*row_children)[size_t{0}] = dynamic_ptr{icon_row};
    (*row_children)[size_t{1}] = dynamic_ptr{type_lbl};
    row["children"_key] = row_children;

    (*children)[size_t{static_cast<size_t>(row_idx)}] = dynamic_ptr{row};
    ++row_idx;
  });

  // Rebuild the sorted-key cache so for_each_child_ordered finds the new rows.
  file_table_ptr_->refresh_children_order();
}

void file_dialog::rebuild_filter_combo(const bison::dynamic& filters) {
  if (!filter_combo_ptr_ || !filter_row_ptr_)
    return;

  std::string items;
  bool first = true;
  filter_regexes_.clear();

  // Each entry is a dynamic with "label" (string, shown in combo) and an
  // optional "regex" (string, applied to filenames; empty means match all).
  filters.forEach([&](key_t, const field& f) {
    auto* ep = f.get<dynamic_ptr>();
    if (!ep || !*ep)
      return;
    const auto& entry = **ep;
    auto label = entry.as<std::string>("label"_key);
    auto regex = entry.as<std::string>("regex"_key);
    if (!first)
      items += '\n';
    items += label;
    filter_regexes_.push_back(std::move(regex));
    first = false;
  });

  bool has_filters = !items.empty();
  filter_row_ptr_["visible"_key] = has_filters;
  filter_combo_ptr_["items"_key] = items;

  // Reset to first filter; re-apply only if files have already been received.
  selected_filter_idx_ = 0;
  if (cached_files_ && cached_files_->size() > 0)
    rebuild_file_rows(*cached_files_);
}

void file_dialog::on_filename_input_changed(const bison::dynamic& payload) {
  if (auto* v = payload.findField<std::string>("value"_key))
    (*this)["filename"_key] = *v;
}

void file_dialog::on_path_input_changed(const bison::dynamic& payload) {
  // Fired only on Enter (EnterReturnsTrue flag is set on path_input).
  // Emit on_navigate with type="path" so the client can jump to the typed dir.
  if (auto* v = payload.findField<std::string>("value"_key)) {
    (*this)["path"_key] = *v;
    bison::dynamic nav;
    nav["name"_key] = *v;
    nav["type"_key] = std::string{"path"};
    emit("on_navigate"_key, std::move(nav));
  }
}

void file_dialog::on_filter_combo_changed(const bison::dynamic& payload) {
  if (auto* v = payload.findField<int32_t>("value"_key)) {
    selected_filter_idx_ = *v;
    if (cached_files_)
      rebuild_file_rows(*cached_files_);
  }
}

void file_dialog::on_row_selected(const bison::dynamic& payload) {
  int32_t idx = payload.as<int32_t>("index"_key);
  if (idx < 0 || static_cast<size_t>(idx) >= row_to_file_idx_.size())
    return;

  if (!cached_files_)
    return;
  const auto& files = *cached_files_;
  size_t orig = row_to_file_idx_[static_cast<size_t>(idx)];
  if (orig >= files.size())
    return;

  auto* ep = files.at(orig).get<dynamic_ptr>();
  if (!ep || !*ep)
    return;
  const auto& entry = **ep;

  auto name = entry.as<std::string>("name"_key);

  // Update the form's public filename field.
  (*this)["filename"_key] = name;

  // Mirror the value into the internal InputText widget.
  if (filename_input_ptr_)
    filename_input_ptr_["value"_key] = name;
}

void file_dialog::on_btn_open_clicked() {
  auto* fn_f = findField<std::string>("filename"_key);
  if (!fn_f)
    return;
  const auto& filename = *fn_f;

  // Validate the path inside a scoped rlock; release before calling
  // remove_internal_objects() which needs the wlock.
  {
    auto sess = context_rlock{*sync_ctx_};
    auto resolved = file_service::resolve_path(filename, sess->resource_dir, sess->allow_absolute_paths);
    if (resolved.empty())
      return;
  }

  bison::dynamic payload;
  payload["path"_key] = filename;
  emit("on_open"_key, std::move(payload));
  // Close the modal so the dialog disappears, matching the conventional
  // "close on confirm" behavior of a file picker.
  request_close();
}

void file_dialog::on_btn_cancel_clicked() {
  emit("on_cancel"_key);
  request_close();
}

void file_dialog::request_close() {
  auto set_flag = [this](context& s) {
    auto it = s.ui_objects.find(internal_root_key_);
    if (it != s.ui_objects.end() && it->second)
      (*it->second)["__request_close__"_key] = true;
  };
  // Mirrors remove_internal_objects()'s own dispatch/non-dispatch branching
  // (form.cpp): on_event() is documented to run outside the session lock,
  // so sess() (which requires an active dispatch) cannot be used here.
  if (detail::current_context) {
    set_flag(*detail::current_context);
  } else {
    auto lock = context_wlock{*sync_ctx_};
    set_flag(*lock);
  }
}

void file_dialog::on_row_activated(const bison::dynamic& payload) {
  int32_t idx = payload.as<int32_t>("index"_key);
  if (idx < 0 || static_cast<size_t>(idx) >= row_to_file_idx_.size())
    return;

  if (!cached_files_)
    return;
  const auto& files = *cached_files_;
  size_t orig = row_to_file_idx_[static_cast<size_t>(idx)];
  if (orig >= files.size())
    return;

  auto* ep = files.at(orig).get<dynamic_ptr>();
  if (!ep || !*ep)
    return;
  const auto& entry = **ep;

  auto name = entry.as<std::string>("name"_key);
  auto type = entry.as<std::string>("type"_key);

  if (type == "dir") {
    bison::dynamic nav;
    nav["name"_key] = name;
    nav["type"_key] = std::string{"dir"};
    emit("on_navigate"_key, std::move(nav));
  } else {
    // Validate inside a scoped rlock; release before remove_internal_objects()
    // which needs the wlock -- matches on_btn_open_clicked().
    {
      auto sess = context_rlock{*sync_ctx_};
      auto resolved = file_service::resolve_path(name, sess->resource_dir, sess->allow_absolute_paths);
      if (resolved.empty())
        return;
    }
    bison::dynamic open;
    open["path"_key] = name;
    emit("on_open"_key, std::move(open));
    // Double-click confirms the selection just like the Open button: close
    // the dialog so the client doesn't have to click Cancel afterward.
    request_close();
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_file_dialog() {
  auto proto = dynamic_ptr{"FileDialog"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Open File"},
          attr<DisplayName>("Title"),
          attr<Description>("Dialog window title."),
          attr<Category>("Appearance")});

  proto->addField(
      "files"_key,
      field{
          dynamic_ptr{key_t{0U}, {}},
          attr<DisplayName>("Files"),
          attr<Description>("Ordered list of entries. Each entry is a dynamic with "
                            "name (string) and type (\"file\" or \"dir\")."),
          attr<Category>("Data")});

  proto->addField(
      "filename"_key,
      field{
          std::string{""},
          attr<DisplayName>("Filename"),
          attr<Description>("Current value of the filename input. Client may preset it; "
                            "the form updates it as the user types or clicks a row."),
          attr<Category>("Data")});

  proto->addField(
      "filters"_key,
      field{
          dynamic_ptr{key_t{0U}, {}},
          attr<DisplayName>("Filters"),
          attr<Description>("Optional list of filter strings shown in a combo box "
                            "(e.g. {0: \"*.txt\", 1: \"*.md\"}). "
                            "Empty means no filter UI is shown."),
          attr<Category>("Data")});

  proto->addField(
      "confirm_label"_key,
      field{
          std::string{"Open"},
          attr<DisplayName>("Confirm Label"),
          attr<Description>("Label on the confirm button (e.g. \"Open\" or \"Save\")."),
          attr<Category>("Appearance")});

  proto->addField(
      "path"_key,
      field{
          std::string{""},
          attr<DisplayName>("Path"),
          attr<Description>("Current directory path shown in the editable path bar. "
                            "Client should update this when handling on_navigate. "
                            "User may also type a path directly and press Enter."),
          attr<Category>("Data")});

  // The __setter hook intercepts every set() call to synchronize internal
  // widgets (Table rows, path bar, btn_open label) with updated field values.
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<file_dialog&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("FileDialog"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("High-level file-picker dialog. Use confirm_label to switch between "
                        "Open and Save As modes. The client supplies the file list and reacts "
                        "to on_open, on_navigate, and on_cancel events."));
  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<file_dialog>("wish"_key, "FileDialog"_key));
}

} // namespace bdg::wish
