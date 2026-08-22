// MIT License © 2025 Binary Dice Games
/// @file zip.cpp
/// @brief Implementation of the Zip form.
///
/// This file has no filesystem or miniz dependency at all: every file this
/// form "browses" lives on the client's machine, not the server's, so all
/// actual compress/extract/list-contents work happens in
/// modules/bdg/desktop/zip/client/zip.cpp instead. See
/// zip.hpp's class doc comment for the full client/server handshake.
#include "zip.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"
#include "ui/forms/file_browser_utils.hpp"

#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

// Space-saved percentage for the View Contents table's Ratio column, e.g.
// compressed to 25% of the original size shows as "75%". "-" for a
// zero-byte (or directory) entry, where a ratio is meaningless.
std::string format_ratio(std::uint64_t uncompressed, std::uint64_t compressed) {
  if (uncompressed == 0)
    return "-";
  double ratio = 100.0 * (1.0 - static_cast<double>(compressed) / static_cast<double>(uncompressed));
  if (ratio < 0.0)
    ratio = 0.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(0) << ratio << "%";
  return oss.str();
}

// format_bytes() is duplicated here rather than shared, the same way
// tree.cpp's server and client copies of it are duplicated -- see
// that module's own precedent.
std::string format_bytes(uintmax_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << kUnits[unit];
  return oss.str();
}

// Strips a case-insensitive trailing ".zip" for the Extract prompt's default
// destination folder name (e.g. "Photos.ZIP" -> "Photos"). Returns `name`
// unchanged if it doesn't end in ".zip".
std::string strip_zip_suffix(const std::string& name) {
  if (name.size() > 4) {
    std::string ext = name.substr(name.size() - 4);
    for (auto& c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".zip")
      return name.substr(0, name.size() - 4);
  }
  return name;
}

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// Single-panel client-machine browser -- mirrors tree.cpp's local
// (left) panel. file_table's "flags": "Resizable|Sortable|RowBg|BordersH|
// ScrollY" -- unlike mc's tables, no BordersV (this is a single, full-width
// panel with no sibling table to visually separate from). InputText
// EnterReturnsTrue so the path bar only fires "changed" on Enter.
static constexpr const char* kLayout = R"json({
  "type": "Window",
  "width": 640, "height": 520,
  "closable": true,
  "children": {
    "main": {
      "type": "VerticalLayout",
      "children": {
        "path_input": { "type": "InputText", "hint": "Local path...", "value": "", "flags": "EnterReturnsTrue", "width": -1 },
        "selected_label": { "type": "Label", "text": "Selected: (none)" },
        "file_table": {
          "type": "Table", "columns": 3, "headers": true,
          "flags": "Resizable|Sortable|RowBg|BordersH|ScrollY", "outer_width": 0, "outer_height": 300,
          "children": {
            "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
            "col_size":     { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 90, "column_id": 1 },
            "col_modified": { "type": "TableColumn", "label": "Modified", "flags": "WidthFixed", "init_width": 130, "column_id": 2 }
          }
        },
        "btn_sep": { "type": "Separator" },
        "btn_row": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "children": {
            "btn_compress": { "type": "Button", "label": "Compress...", "width": 120, "height": 32 },
            "btn_extract":  { "type": "Button", "label": "Extract", "width": 100, "height": 32 },
            "btn_view":     { "type": "Button", "label": "View Contents", "width": 130, "height": 32 },
            "btn_refresh":  { "type": "Button", "label": "Refresh", "width": 90, "height": 32 }
          }
        },
        "status_sep": { "type": "Separator" },
        "status": { "type": "Label", "text": "Ready." }
      }
    }
  }
})json";

// Reused for both the Compress (archive name) and Extract (destination
// folder name) first step -- title/message/OK-label are stamped from
// pending_action in show_prompt(), mirroring message_box.cpp's one-layout-
// per-preset idiom collapsed to a single reusable layout instead (there are
// only two presets and they differ solely by text, not structure).
static constexpr const char* kPromptLayout = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "message": { "type": "Label", "text": "" },
    "name_input": { "type": "InputText", "value": "", "width": 300 },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_ok": { "type": "Button", "label": "", "height": 32 },
      "btn_cancel": { "type": "Button", "label": "Cancel", "height": 32 }
    } }
  }
})";

// Second step for both actions when the name typed at the prompt already
// exists (per the cached listing) -- mirrors tree.cpp's
// overwrite-confirm dialog.
static constexpr const char* kConfirmLayout = R"({
  "type": "Window", "title": "Confirm Overwrite", "modal": true,
  "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "message": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_yes": { "type": "Button", "label": "Overwrite", "height": 32 },
      "btn_no": { "type": "Button", "label": "Cancel", "height": 32 }
    } }
  }
})";

// Read-only archive listing. flags "Resizable|RowBg|BordersH|ScrollY" is
// the main table's flags minus Sortable -- this table has no click-to-sort
// handler, same omission tree.cpp's own comment calls out for FileDialog's
// table.
static constexpr const char* kContentsLayout = R"json({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "width": 480, "height": 420,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "summary": { "type": "Label", "text": "" },
        "contents_table": {
          "type": "Table", "columns": 4, "headers": true,
          "flags": "Resizable|RowBg|BordersH|ScrollY", "outer_width": 0, "outer_height": 300,
          "children": {
            "col_name":       { "type": "TableColumn", "label": "Name" },
            "col_size":       { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 80 },
            "col_compressed": { "type": "TableColumn", "label": "Compressed", "flags": "WidthFixed", "init_width": 90 },
            "col_ratio":      { "type": "TableColumn", "label": "Ratio", "flags": "WidthFixed", "init_width": 70 }
          }
        },
        "sep": { "type": "Separator" },
        "btn_row": { "type": "HorizontalLayout", "spacing": 8, "align": "right", "children": {
          "btn_close": { "type": "Button", "label": "Close", "width": 100, "height": 32 }
        } }
      }
    }
  }
})json";

// ── zip ──────────────────────────────────────────────────────────────────

zip::zip(dynamic&& base) : form(std::move(base)) {}

void zip::on_init() {
  internal_root_key_ = next_available_key("__zip_");

  auto tree = import_json(kLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Zip"};

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("main.path_input", [&](const auto& e) {
    path_input_ptr_ = e;
    path_input_id_ = wish_id_of(e);
  });
  tree.with("main.selected_label", [&](const auto& e) { selected_label_ptr_ = e; });
  tree.with("main.file_table", [&](const auto& e) {
    file_table_ptr_ = e;
    file_table_id_ = wish_id_of(e);
  });
  tree.with("main.btn_row.btn_compress", [&](const auto& e) { btn_compress_id_ = wish_id_of(e); });
  tree.with("main.btn_row.btn_extract", [&](const auto& e) { btn_extract_id_ = wish_id_of(e); });
  tree.with("main.btn_row.btn_view", [&](const auto& e) { btn_view_id_ = wish_id_of(e); });
  tree.with("main.btn_row.btn_refresh", [&](const auto& e) { btn_refresh_id_ = wish_id_of(e); });
  tree.with("main.status", [&](const auto& e) { status_label_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);

  // Unlike mc's sandbox panel, this form has no filesystem of its
  // own to populate the table from -- it starts empty until the client's
  // initial update_listing() call arrives.
}

// ── Table population and sorting ─────────────────────────────────────────────

void zip::fill_table(const std::vector<file_row>& entries, int32_t selected_index) {
  if (!file_table_ptr_)
    return;
  auto* children_p = file_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  children->clear();

  int32_t idx = 0;
  for (auto& entry : entries) {
    ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
    row["order"_key] = idx;
    row["selected"_key] = idx == selected_index;

    auto make_label = [&](const std::string& text, int32_t order) {
      ui_element_ptr lbl{dynamic::instantiate("wish"_key, "Label"_key)};
      lbl["text"_key] = text;
      lbl["order"_key] = order;
      return lbl;
    };

    std::string display_name = entry.name == ".." ? std::string{".. [Up]"}
        : entry.type == "dir"                     ? ("[" + entry.name + "]")
                                                    : entry.name;

    ui_element_ptr name_cell = make_name_cell(entry.name, entry.type, display_name);

    auto row_children = dynamic_ptr{key_t{0U}, {}};
    (*row_children)[size_t{0}] = dynamic_ptr{name_cell};
    (*row_children)[size_t{1}] = dynamic_ptr{make_label(entry.type == "dir" ? std::string{} : entry.size, 1)};
    (*row_children)[size_t{2}] = dynamic_ptr{make_label(entry.modified, 2)};
    row["children"_key] = row_children;

    (*children)[static_cast<size_t>(idx)] = dynamic_ptr{row};
    ++idx;
  }
  file_table_ptr_->refresh_children_order();
}

void zip::fill_contents_table(const ui_element_ptr& table, const std::vector<archive_entry>& entries) const {
  if (!table)
    return;
  auto* children_p = table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  int32_t idx = 0;
  for (auto& entry : entries) {
    ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
    row["order"_key] = idx;

    ui_element_ptr name_cell = make_name_cell(entry.name, entry.is_dir ? "dir" : "file", entry.name);

    auto make_label = [&](const std::string& text, int32_t order) {
      ui_element_ptr lbl{dynamic::instantiate("wish"_key, "Label"_key)};
      lbl["text"_key] = text;
      lbl["order"_key] = order;
      return lbl;
    };

    std::string size_text = entry.is_dir ? std::string{} : format_bytes(entry.uncompressed_size);
    std::string compressed_text = entry.is_dir ? std::string{} : format_bytes(entry.compressed_size);
    std::string ratio_text = entry.is_dir ? std::string{} : format_ratio(entry.uncompressed_size, entry.compressed_size);

    auto row_children = dynamic_ptr{key_t{0U}, {}};
    (*row_children)[size_t{0}] = dynamic_ptr{name_cell};
    (*row_children)[size_t{1}] = dynamic_ptr{make_label(size_text, 1)};
    (*row_children)[size_t{2}] = dynamic_ptr{make_label(compressed_text, 2)};
    (*row_children)[size_t{3}] = dynamic_ptr{make_label(ratio_text, 3)};
    row["children"_key] = row_children;

    (*children)[static_cast<size_t>(idx)] = dynamic_ptr{row};
    ++idx;
  }
  table->refresh_children_order();
}

void zip::sort_entries(std::vector<file_row>& entries, int32_t sort_column_id, bool ascending) const {
  // A leading ".." entry (the client's own "up" row) is never part of the sort.
  size_t begin = !entries.empty() && entries[0].name == ".." ? 1 : 0;

  auto key_less = [&](const file_row& a, const file_row& b) {
    switch (sort_column_id) {
      case 1: // Size -- numeric, not lexicographic.
        return parse_display_size(a.size) < parse_display_size(b.size);
      case 2: // Modified -- client-formatted "%Y-%m-%d %H:%M" sorts
              // correctly as a plain string.
        return ascii_ci_less(a.modified, b.modified);
      default: // Name (0), and any unrecognized column_id.
        return ascii_ci_less(a.name, b.name);
    }
  };
  std::stable_sort(entries.begin() + static_cast<ptrdiff_t>(begin), entries.end(), [&](auto& a, auto& b) {
    return ascending ? key_less(a, b) : key_less(b, a);
  });
}

void zip::on_table_sorted(const dynamic& payload) {
  auto* col_f = payload.findField<int32_t>("column_id"_key);
  auto* asc_f = payload.findField<bool>("ascending"_key);
  if (!col_f || !asc_f)
    return;
  sort_column_id_ = *col_f;
  sort_ascending_ = *asc_f;
  sort_entries(entries_, sort_column_id_, sort_ascending_);
  fill_table(entries_);
}

void zip::set_status(const std::string& message) {
  (*this)["status"_key] = message;
  if (status_label_ptr_)
    status_label_ptr_["text"_key] = message;
}

bool zip::is_zip_name(const std::string& name) const {
  if (name.size() < 4)
    return false;
  std::string ext = name.substr(name.size() - 4);
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".zip";
}

std::string zip::cached_entry_type(const std::string& name) const {
  for (auto& e : entries_)
    if (e.name == name)
      return e.type;
  return {};
}

// ── RMI methods ───────────────────────────────────────────────────────────────

dynamic zip::do_update_listing(const dynamic& args) {
  path_ = args.as<std::string>("path"_key);
  entries_.clear();

  if (auto* files_f = args.findField<dynamic_ptr>("files"_key); files_f && *files_f) {
    (*files_f)->forEach([&](key_t, const field& f) {
      auto* ep = f.get<dynamic_ptr>();
      if (!ep || !*ep)
        return;
      const auto& e = **ep;
      file_row row;
      row.name = e.as<std::string>("name"_key);
      row.type = e.as<std::string>("type"_key);
      row.size = e.as<std::string>("size"_key);
      row.modified = e.as<std::string>("modified"_key);
      entries_.push_back(std::move(row));
    });
  }

  sort_entries(entries_, sort_column_id_, sort_ascending_);
  fill_table(entries_);
  if (path_input_ptr_)
    path_input_ptr_["value"_key] = path_;
  (*this)["path"_key] = path_;
  set_status("Ready.");

  selected_name_.clear();
  selected_type_.clear();
  if (selected_label_ptr_)
    selected_label_ptr_["text"_key] = "Selected: (none)";
  return dynamic{};
}

dynamic zip::do_show_contents(const dynamic& args) {
  auto name = args.as<std::string>("name"_key);

  std::vector<archive_entry> archive_entries;
  if (auto* entries_f = args.findField<dynamic_ptr>("entries"_key); entries_f && *entries_f) {
    (*entries_f)->forEach([&](key_t, const field& f) {
      auto* ep = f.get<dynamic_ptr>();
      if (!ep || !*ep)
        return;
      const auto& e = **ep;
      archive_entry entry;
      entry.name = e.as<std::string>("name"_key);
      entry.is_dir = e.as<std::string>("type"_key) == "dir";
      entry.uncompressed_size = static_cast<std::uint64_t>(e.as<int32_t>("uncompressed_size"_key));
      entry.compressed_size = static_cast<std::uint64_t>(e.as<int32_t>("compressed_size"_key));
      archive_entries.push_back(std::move(entry));
    });
  }

  show_contents_dialog(name, archive_entries);
  return dynamic{};
}

// ── on_set ────────────────────────────────────────────────────────────────────

dynamic zip::on_set(const dynamic& patch) {
  if (auto* v = patch.findField<std::string>("status"_key); v && status_label_ptr_)
    status_label_ptr_["text"_key] = *v;
  return patch;
}

// ── Name/destination prompt dialog ───────────────────────────────────────────

void zip::show_prompt(pending_action action, const std::string& source_name, const std::string& default_value) {
  // Called from on_event(), outside dispatch -- mirrors
  // tree.cpp's show_overwrite_confirm() for the same reason: sess()
  // would throw here, so this acquires context_wlock directly.
  prompt_action_ = action;
  prompt_source_name_ = source_name;
  prompt_value_ = default_value;

  auto tree = import_json(kPromptLayout);
  std::string title = action == pending_action::compress ? "Compress" : "Extract";
  std::string message = action == pending_action::compress ? ("Create archive from \"" + source_name + "\":")
                                                             : ("Extract \"" + source_name + "\" to folder:");
  std::string ok_label = action == pending_action::compress ? "Create" : "Extract";

  (*tree[""])["title"_key] = title;
  tree.with("message", [&](const auto& e) { e["text"_key] = message; });
  tree.with("name_input", [&](const auto& e) { e["value"_key] = default_value; });
  tree.with("buttons.btn_ok", [&](const auto& e) { e["label"_key] = ok_label; });

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  prompt_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("name_input", [&](const auto& e) { prompt_input_id_ = wish_id_of(e); });
  tree.with("buttons.btn_ok", [&](const auto& e) { prompt_ok_id_ = wish_id_of(e); });
  tree.with("buttons.btn_cancel", [&](const auto& e) { prompt_cancel_id_ = wish_id_of(e); });

  auto lock = context_wlock{*sync_ctx_};
  context& s = *lock;
  for (int i = 0;; ++i) {
    std::string candidate = "__zip_prompt_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      prompt_root_key_ = candidate;
      break;
    }
  }

  s.ui_objects.merge(std::move(tree), prompt_root_key_);
  auto it = s.ui_objects.find(prompt_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{prompt_root_key_}] = it->second;
    (*it->second)["__path__"_key] = prompt_root_key_;
    s.top_level_handlers[key_t{prompt_root_key_}] = this;
  }
}

void zip::request_close_prompt() {
  request_close_at(prompt_root_key_);
}

void zip::remove_prompt_objects() {
  remove_objects_at(prompt_root_key_);
  prompt_root_key_.clear();
}

void zip::emit_action_request(
    pending_action action, const std::string& source_name, const std::string& target_name) {
  dynamic req;
  req["path"_key] = path_;
  if (action == pending_action::compress) {
    req["source_name"_key] = source_name;
    req["archive_name"_key] = target_name;
    emit("on_compress_requested"_key, std::move(req));
  } else if (action == pending_action::extract) {
    req["zip_name"_key] = source_name;
    req["dest_name"_key] = target_name;
    emit("on_extract_requested"_key, std::move(req));
  }
}

void zip::on_prompt_confirmed() {
  std::string value = prompt_value_;
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();

  if (value.empty() || value.find('/') != std::string::npos || value.find('\\') != std::string::npos ||
      value == "." || value == "..") {
    set_status("Invalid name.");
    return;
  }

  if (prompt_action_ == pending_action::compress && value == prompt_source_name_) {
    set_status("Archive name must differ from the source.");
    return;
  }

  std::string existing_type = cached_entry_type(value);
  if (prompt_action_ == pending_action::compress && existing_type == "dir") {
    set_status("Cannot overwrite a folder.");
    return;
  }
  if (prompt_action_ == pending_action::extract && existing_type == "file") {
    set_status("A file with that name already exists.");
    return;
  }

  if (!existing_type.empty()) {
    // Name collides with something in the last-reported listing -- ask
    // before overwriting/merging (this form only has that cached listing to
    // check against; the client re-validates against the real filesystem
    // when it actually performs the operation).
    confirm_action_ = prompt_action_;
    confirm_source_name_ = prompt_source_name_;
    confirm_target_name_ = value;
    request_close_prompt();
    std::string message = prompt_action_ == pending_action::compress
        ? ("\"" + value + "\" already exists. Overwrite it?")
        : ("\"" + value + "\" already exists. Merge and overwrite its contents?");
    show_overwrite_confirm(message);
    return;
  }

  pending_action action = prompt_action_;
  std::string source_name = prompt_source_name_;
  request_close_prompt();
  emit_action_request(action, source_name, value);
  set_status(action == pending_action::compress ? "Compressing..." : "Extracting...");
}

// ── Overwrite confirmation dialog ────────────────────────────────────────────

void zip::show_overwrite_confirm(const std::string& message) {
  auto tree = import_json(kConfirmLayout);
  tree.with("message", [&](const auto& e) { e["text"_key] = message; });

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  confirm_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("buttons.btn_yes", [&](const auto& e) { confirm_yes_id_ = wish_id_of(e); });
  tree.with("buttons.btn_no", [&](const auto& e) { confirm_no_id_ = wish_id_of(e); });

  auto lock = context_wlock{*sync_ctx_};
  context& s = *lock;
  for (int i = 0;; ++i) {
    std::string candidate = "__zip_confirm_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      confirm_root_key_ = candidate;
      break;
    }
  }

  s.ui_objects.merge(std::move(tree), confirm_root_key_);
  auto it = s.ui_objects.find(confirm_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{confirm_root_key_}] = it->second;
    (*it->second)["__path__"_key] = confirm_root_key_;
    s.top_level_handlers[key_t{confirm_root_key_}] = this;
  }
}

void zip::request_close_confirm() {
  request_close_at(confirm_root_key_);
}

void zip::remove_confirm_objects() {
  remove_objects_at(confirm_root_key_);
  confirm_root_key_.clear();
}

// ── View Contents dialog ──────────────────────────────────────────────────────

void zip::show_contents_dialog(const std::string& zip_name, const std::vector<archive_entry>& entries) {
  auto tree = import_json(kContentsLayout);
  (*tree[""])["title"_key] = "Contents: " + zip_name;

  std::uint64_t total_uncompressed = 0, total_compressed = 0;
  for (auto& e : entries) {
    if (!e.is_dir) {
      total_uncompressed += e.uncompressed_size;
      total_compressed += e.compressed_size;
    }
  }

  std::ostringstream summary;
  summary << entries.size() << (entries.size() == 1 ? " entry, " : " entries, ") << format_bytes(total_uncompressed)
          << " uncompressed / " << format_bytes(total_compressed) << " compressed";
  tree.with("vbox.summary", [&](const auto& e) { e["text"_key] = summary.str(); });

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  contents_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  ui_element_ptr contents_table_ptr;
  tree.with("vbox.contents_table", [&](const auto& e) { contents_table_ptr = e; });
  tree.with("vbox.btn_row.btn_close", [&](const auto& e) { contents_close_id_ = wish_id_of(e); });

  // Unlike show_prompt()/show_overwrite_confirm() (only ever called from
  // on_event(), outside dispatch), this is only ever reached from
  // do_show_contents() -- an RMI method, which runs *inside* dispatch with
  // the session wlock already held. Acquiring context_wlock here too would
  // self-deadlock (std::shared_mutex is non-recursive) -- use sess()
  // instead, same as on_init()'s own sess().ui_objects.merge() call.
  context& s = sess();
  for (int i = 0;; ++i) {
    std::string candidate = "__zip_contents_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      contents_root_key_ = candidate;
      break;
    }
  }

  s.ui_objects.merge(std::move(tree), contents_root_key_);
  auto it = s.ui_objects.find(contents_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{contents_root_key_}] = it->second;
    (*it->second)["__path__"_key] = contents_root_key_;
    s.top_level_handlers[key_t{contents_root_key_}] = this;
  }

  fill_contents_table(contents_table_ptr, entries);
}

void zip::request_close_contents() {
  request_close_at(contents_root_key_);
}

void zip::remove_contents_objects() {
  remove_objects_at(contents_root_key_);
  contents_root_key_.clear();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void zip::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == file_table_id_) {
    if (event == "row_selected"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx >= 0 && static_cast<size_t>(idx) < entries_.size()) {
        selected_name_ = entries_[static_cast<size_t>(idx)].name;
        selected_type_ = entries_[static_cast<size_t>(idx)].type;
        if (selected_label_ptr_)
          selected_label_ptr_["text"_key] = "Selected: " + selected_name_;
        fill_table(entries_, idx);
      }
      return;
    }
    if (event == "row_activated"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx < 0 || static_cast<size_t>(idx) >= entries_.size())
        return;
      const auto& entry = entries_[static_cast<size_t>(idx)];
      if (entry.type == "dir") {
        dynamic nav;
        nav["name"_key] = entry.name;
        nav["type"_key] = std::string{"dir"};
        emit("on_navigate"_key, std::move(nav));
      } else if (is_zip_name(entry.name)) {
        dynamic req;
        req["path"_key] = path_;
        req["name"_key] = entry.name;
        emit("on_view_contents_requested"_key, std::move(req));
      } else {
        set_status("Not an archive: " + entry.name);
      }
      return;
    }
    if (event == "sorted"_key) {
      selected_name_.clear();
      selected_type_.clear();
      if (selected_label_ptr_)
        selected_label_ptr_["text"_key] = "Selected: (none)";
      on_table_sorted(payload);
      return;
    }
  }

  if (id == path_input_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<std::string>("value"_key)) {
      dynamic nav;
      nav["name"_key] = *v;
      nav["type"_key] = std::string{"path"};
      emit("on_navigate"_key, std::move(nav));
    }
    return;
  }

  if (id == btn_refresh_id_ && event == "clicked"_key) {
    dynamic nav;
    nav["name"_key] = path_;
    nav["type"_key] = std::string{"path"};
    emit("on_navigate"_key, std::move(nav));
    return;
  }

  if (id == btn_compress_id_ && event == "clicked"_key) {
    if (selected_name_.empty() || selected_name_ == "..") {
      set_status("Select a file or folder to compress.");
      return;
    }
    show_prompt(pending_action::compress, selected_name_, selected_name_ + ".zip");
    return;
  }

  if (id == btn_extract_id_ && event == "clicked"_key) {
    if (selected_name_.empty() || selected_type_ != "file" || !is_zip_name(selected_name_)) {
      set_status("Select a .zip file to extract.");
      return;
    }
    show_prompt(pending_action::extract, selected_name_, strip_zip_suffix(selected_name_));
    return;
  }

  if (id == btn_view_id_ && event == "clicked"_key) {
    if (selected_name_.empty() || selected_type_ != "file" || !is_zip_name(selected_name_)) {
      set_status("Select a .zip file to view its contents.");
      return;
    }
    dynamic req;
    req["path"_key] = path_;
    req["name"_key] = selected_name_;
    emit("on_view_contents_requested"_key, std::move(req));
    return;
  }

  if (!prompt_root_key_.empty()) {
    if (id == prompt_window_id_ && event == "closed"_key) {
      remove_prompt_objects();
      prompt_action_ = pending_action::none;
      return;
    }
    if (id == prompt_input_id_ && event == "changed"_key) {
      if (auto* v = payload.findField<std::string>("value"_key))
        prompt_value_ = *v;
      return;
    }
    if (id == prompt_ok_id_ && event == "clicked"_key) {
      on_prompt_confirmed();
      return;
    }
    if (id == prompt_cancel_id_ && event == "clicked"_key) {
      set_status(prompt_action_ == pending_action::compress ? "Compress cancelled." : "Extract cancelled.");
      request_close_prompt();
      return;
    }
  }

  if (!confirm_root_key_.empty()) {
    if (id == confirm_window_id_ && event == "closed"_key) {
      remove_confirm_objects();
      confirm_action_ = pending_action::none;
      return;
    }
    if (id == confirm_yes_id_ && event == "clicked"_key) {
      emit_action_request(confirm_action_, confirm_source_name_, confirm_target_name_);
      set_status(confirm_action_ == pending_action::compress ? "Compressing..." : "Extracting...");
      request_close_confirm();
      return;
    }
    if (id == confirm_no_id_ && event == "clicked"_key) {
      set_status(confirm_action_ == pending_action::compress ? "Compress cancelled." : "Extract cancelled.");
      request_close_confirm();
      return;
    }
  }

  if (!contents_root_key_.empty()) {
    if (id == contents_window_id_ && event == "closed"_key) {
      remove_contents_objects();
      return;
    }
    if (id == contents_close_id_ && event == "clicked"_key) {
      request_close_contents();
      return;
    }
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_zip() {
  auto proto = dynamic_ptr{"Zip"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Zip"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addField(
      "path"_key,
      field{
          std::string{""},
          attr<DisplayName>("Path"),
          attr<Description>("Client-owned local directory currently shown in the browser. "
                            "Updated via update_listing(); read-only from the client's "
                            "perspective otherwise."),
          attr<Category>("Data")});

  proto->addField(
      "status"_key,
      field{
          std::string{"Ready."},
          attr<DisplayName>("Status"),
          attr<Description>("Text shown in the status bar at the bottom of the window."),
          attr<Category>("Data")});

  proto->addMethod(
      "update_listing"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<zip&>(self).do_update_listing(args);
      }});
  proto->addMethod(
      "show_contents"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<zip&>(self).do_show_contents(args);
      }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<zip&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Zip"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Client-machine file browser with compress/extract/view-contents actions for zip archives. "
      "The server owns the UI only; listen for on_navigate/on_compress_requested/"
      "on_extract_requested/on_view_contents_requested to drive the client half of the handshake, "
      "and 'closed' to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<zip>("wish"_key, "Zip"_key));
}

} // namespace bdg::wish
