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

#include <ui/dock_layout_spec.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

// Space-saved percentage for the Contents panel table's Ratio column, e.g.
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

// ── UI layouts ────────────────────────────────────────────────────────────────
//
// The tool is split into three dockable panels -- Files, Contents and
// Actions -- seeded into a first-run arrangement by on_init()'s
// set_default_dock_layout() call, the same multi-window pattern top, pix
// and the dev modules use. The user can re-dock, tab or float any of them;
// imgui.ini owns the arrangement after the first run. Each Window keeps a
// width/height: the size a panel restores to when dragged out of the dock.
//
// Files (the form's main root, internal_root_key_): a single client-machine
// browser -- mirrors tree.cpp's local (left) panel. file_table's "flags":
// "Resizable|Sortable|RowBg|Borders|ScrollY". InputText EnterReturnsTrue so
// the path bar only fires "changed" on Enter. file_table carries
// "height": -1 rather than a fixed "outer_height" (mc.cpp's left_table_/
// right_table_ technique): "main" hands path_input/selected_label their
// natural size first, then gives file_table whatever's left, so the table
// fills the panel.
//
// Contents: the read-only listing of the archive last viewed (via View
// Contents or double-clicking a .zip), filled by do_show_contents(). Its
// flags are the file table's minus Sortable -- this table has no
// click-to-sort handler, same omission tree.cpp's own comment calls out for
// FileDialog's table. contents_table's "height": -1 fills the panel below
// the summary label.
//
// Actions: the compress/extract/view/refresh buttons and the status label,
// as a strip along the top. Compress/extract progress is not shown here: the
// client reports it through the shared ProgressBox dialog
// (modules/bdg/common/command_worker.hpp), like mc and the dev modules.
static constexpr const char* kFilesLayout = R"json({
  "type": "Window",
  "title": "Files",
  "width": 520, "height": 440,
  "closable": true,
  "children": {
    "main": {
      "type": "VerticalLayout",
      "children": {
        "path_input": { "type": "InputText", "hint": "Local path...", "value": "", "flags": "EnterReturnsTrue", "width": -1 },
        "selected_label": { "type": "Label", "text": "Selected: (none)" },
        "file_table": {
          "type": "Table", "columns": 3, "headers": true,
          "flags": "Resizable|Sortable|RowBg|Borders|ScrollY", "outer_width": 0, "height": -1,
          "children": {
            "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
            "col_size":     { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 90, "column_id": 1 },
            "col_modified": { "type": "TableColumn", "label": "Modified", "flags": "WidthFixed", "init_width": 130, "column_id": 2 }
          }
        }
      }
    }
  }
})json";

static constexpr const char* kContentsLayout = R"json({
  "type": "Window",
  "title": "Contents",
  "width": 480, "height": 440,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "summary": { "type": "Label", "text": "No archive open. Double-click a .zip or use View Contents." },
        "contents_table": {
          "type": "Table", "columns": 4, "headers": true,
          "flags": "Resizable|RowBg|BordersH|ScrollY", "outer_width": 0, "height": -1,
          "children": {
            "col_name":       { "type": "TableColumn", "label": "Name" },
            "col_size":       { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 80 },
            "col_compressed": { "type": "TableColumn", "label": "Compressed", "flags": "WidthFixed", "init_width": 90 },
            "col_ratio":      { "type": "TableColumn", "label": "Ratio", "flags": "WidthFixed", "init_width": 70 }
          }
        }
      }
    }
  }
})json";

static constexpr const char* kActionsLayout = R"json({
  "type": "Window",
  "title": "Actions",
  "width": 1000, "height": 90,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "btn_row": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "children": {
            "btn_compress": { "type": "Button", "label": "Compress...", "icon": "res/icons/zip.png", "height": 32 },
            "btn_extract":  { "type": "Button", "label": "Extract", "icon": "res/icons/folder_open.png", "height": 32 },
            "btn_view":     { "type": "Button", "label": "View Contents", "icon": "res/icons/visibility.png", "height": 32 },
            "btn_refresh":  { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png", "height": 32 }
          }
        },
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

// The second step for both actions, when the name typed at the prompt
// already exists (per the cached listing), is a privately-instantiated
// built-in MessageBox form (see form::instantiate_child_form()) rather than
// a raw element tree owned directly by this form -- see
// show_overwrite_confirm() below.

// ── zip ──────────────────────────────────────────────────────────────────

zip::zip(dynamic&& base) : tool_form(std::move(base)) {}

zip::~zip() {
  remove_panel_objects();
}

void zip::on_init() {
  internal_root_key_ = next_available_key("__zip_");
  contents_root_key_ = internal_root_key_ + "_contents";
  actions_root_key_ = internal_root_key_ + "_actions";

  auto* title_f = findField<std::string>("title"_key);
  const std::string title = title_f ? *title_f : std::string{"Zip"};

  build_window(internal_root_key_, kFilesLayout, window_id_, [&](ui_tree& tree) {
    tree.with("main.path_input", [&](const auto& e) {
      path_input_ptr_ = e;
      path_input_id_ = wish_id_of(e);
    });
    tree.with("main.selected_label", [&](const auto& e) { selected_label_ptr_ = e; });
    tree.with("main.file_table", [&](const auto& e) {
      file_table_ptr_ = e;
      file_table_id_ = wish_id_of(e);
    });
  });

  build_window(contents_root_key_, kContentsLayout, contents_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.summary", [&](const auto& e) { contents_summary_ptr_ = e; });
    tree.with("vbox.contents_table", [&](const auto& e) { contents_table_ptr_ = e; });
  });

  build_window(actions_root_key_, kActionsLayout, actions_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.btn_row.btn_compress", [&](const auto& e) { btn_compress_id_ = wish_id_of(e); });
    tree.with("vbox.btn_row.btn_extract", [&](const auto& e) { btn_extract_id_ = wish_id_of(e); });
    tree.with("vbox.btn_row.btn_view", [&](const auto& e) { btn_view_id_ = wish_id_of(e); });
    tree.with("vbox.btn_row.btn_refresh", [&](const auto& e) { btn_refresh_id_ = wish_id_of(e); });
    tree.with("vbox.status", [&](const auto& e) { status_label_ptr_ = e; });
  });

  // Unlike mc's sandbox panel, this form has no filesystem of its
  // own to populate the table from -- it starts empty until the client's
  // initial update_listing() call arrives.

  // Seed the first-run arrangement inside the tool's own nested dockspace
  // (titled with the form's "title" field): an Actions strip along the
  // top ~14%, and below it Files on the left beside Contents on the
  // right. Owned by imgui.ini after the first run (see docs/dock-layout.md);
  // bump the version arg to layout() if it changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "zip_dock", title,
        layout(
            split(
                dir::up, 0.14f, area({actions_root_key_}),
                split(dir::left, 0.55f, area({internal_root_key_}), area({contents_root_key_}))),
            /*version=*/2, /*target=*/"zip_dock")));
  }
}

void zip::remove_panel_objects() {
  // Keys are forgotten once removed: next_available_key() may hand this
  // form's freed internal_root_key_ to a new Zip instance, whose panels
  // would then reuse these exact secondary keys (see the same reasoning in
  // form::remove_objects_at()).
  for (std::string* key : {&contents_root_key_, &actions_root_key_}) {
    remove_objects_at(*key);
    key->clear();
  }
}

// ── Table population and sorting ─────────────────────────────────────────────

void zip::fill_table(const std::vector<file_row>& entries, const std::set<std::string>& selected_names) {
  if (!file_table_ptr_)
    return;
  auto* children_p = file_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  children->clear();

  int32_t idx = 0;
  for (auto& entry : entries) {
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row["order"_key] = idx;
    row["selected"_key] = selected_names.count(entry.name) > 0;

    auto make_label = [&](const std::string& text, int32_t order) {
      ui_element_ptr lbl = ui_element_ptr::create("wish"_key, "Label"_key);
      lbl["text"_key] = text;
      lbl["order"_key] = order;
      return lbl;
    };

    ui_element_ptr name_cell = make_name_cell(entry.name, entry.type, entry.name);

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
  // Drop the previous archive's rows (indexed); the named TableColumn
  // children remain.
  children->clear();

  int32_t idx = 0;
  for (auto& entry : entries) {
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row["order"_key] = idx;

    ui_element_ptr name_cell = make_name_cell(entry.name, entry.is_dir ? "dir" : "file", entry.name);

    auto make_label = [&](const std::string& text, int32_t order) {
      ui_element_ptr lbl = ui_element_ptr::create("wish"_key, "Label"_key);
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
  // Row positions change under the new sort order, so a Shift-range anchor
  // (an index) would point at the wrong entry -- reset it. The selection
  // itself is name-keyed and survives the reorder unchanged (mirrors
  // mc.cpp's own on_table_sorted() doc comment).
  selection_anchor_ = -1;
  sort_entries(entries_, sort_column_id_, sort_ascending_);
  fill_table(entries_, selected_names_);
}

void zip::set_status(const std::string& message) {
  (*this)["status"_key] = message;
  if (status_label_ptr_)
    status_label_ptr_["text"_key] = message;
}

// ── Multi-selection ───────────────────────────────────────────────────────────

void zip::apply_row_click(
    std::set<std::string>& selected, int32_t& anchor, const std::vector<file_row>& entries, int32_t idx, bool ctrl,
    bool shift) {
  const std::string& name = entries[static_cast<size_t>(idx)].name;
  if (shift && anchor >= 0 && static_cast<size_t>(anchor) < entries.size()) {
    selected.clear();
    int32_t lo = std::min(anchor, idx);
    int32_t hi = std::max(anchor, idx);
    for (int32_t i = lo; i <= hi; ++i)
      selected.insert(entries[static_cast<size_t>(i)].name);
  } else if (ctrl) {
    if (!selected.insert(name).second)
      selected.erase(name);
    anchor = idx;
  } else {
    selected.clear();
    selected.insert(name);
    anchor = idx;
  }
}

std::string zip::describe_selection(const std::set<std::string>& selected) {
  if (selected.empty())
    return "Selected: (none)";
  if (selected.size() == 1)
    return "Selected: " + *selected.begin();
  return "Selected: " + std::to_string(selected.size()) + " items";
}

std::vector<std::string> zip::selected_target_names(
    const std::set<std::string>& selected, const std::vector<file_row>& entries) {
  std::vector<std::string> names;
  for (auto& e : entries)
    if (e.name != ".." && selected.count(e.name))
      names.push_back(e.name);
  return names;
}

dynamic zip::make_names_payload(const std::vector<std::string>& names) {
  dynamic payload;
  dynamic arr;
  size_t i = 0;
  for (auto& n : names)
    arr[i++] = n;
  payload["names"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return payload;
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

  // A freshly-reported listing (possibly a different directory) invalidates
  // whatever was selected before -- the old names may not even exist here.
  selected_names_.clear();
  selection_anchor_ = -1;

  sort_entries(entries_, sort_column_id_, sort_ascending_);
  fill_table(entries_, selected_names_);
  if (path_input_ptr_)
    path_input_ptr_["value"_key] = path_;
  (*this)["path"_key] = path_;
  set_status("Ready.");

  if (selected_label_ptr_)
    selected_label_ptr_["text"_key] = describe_selection(selected_names_);
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

  show_contents_panel(name, archive_entries);
  return dynamic{};
}

// ── on_set ────────────────────────────────────────────────────────────────────

dynamic zip::on_set(const dynamic& patch) {
  if (auto* v = patch.findField<std::string>("status"_key); v && status_label_ptr_)
    status_label_ptr_["text"_key] = *v;
  return patch;
}

// ── Name/destination prompt dialog ───────────────────────────────────────────

void zip::show_prompt(
    pending_action action, const std::vector<std::string>& source_names, const std::string& default_value) {
  // Called from on_event(), outside dispatch -- mirrors
  // tree.cpp's show_overwrite_confirm() for the same reason: sess()
  // would throw here, so this acquires context_wlock directly.
  prompt_action_ = action;
  prompt_source_names_ = source_names;
  prompt_value_ = default_value;

  auto tree = import_json(kPromptLayout);
  std::string title = action == pending_action::compress ? "Compress" : "Extract";
  std::string source_desc = source_names.size() == 1 ? ("\"" + source_names[0] + "\"")
                                                       : (std::to_string(source_names.size()) + " items");
  std::string message = action == pending_action::compress ? ("Create archive from " + source_desc + ":")
                                                             : ("Extract " + source_desc + " to folder:");
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
    pending_action action, const std::vector<std::string>& source_names, const std::string& target_name) {
  dynamic req;
  req["path"_key] = path_;
  if (action == pending_action::compress) {
    dynamic arr;
    size_t i = 0;
    for (auto& n : source_names)
      arr[i++] = n;
    req["source_names"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
    req["archive_name"_key] = target_name;
    emit("on_compress_requested"_key, std::move(req));
  } else if (action == pending_action::extract) {
    req["zip_name"_key] = source_names.empty() ? std::string{} : source_names[0];
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

  if (prompt_action_ == pending_action::compress &&
      std::find(prompt_source_names_.begin(), prompt_source_names_.end(), value) != prompt_source_names_.end()) {
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
    pending_action action = prompt_action_;
    std::vector<std::string> source_names = prompt_source_names_;
    request_close_prompt();
    std::string message = action == pending_action::compress
        ? ("\"" + value + "\" already exists. Overwrite it?")
        : ("\"" + value + "\" already exists. Merge and overwrite its contents?");
    show_overwrite_confirm(action, source_names, value, message);
    return;
  }

  pending_action action = prompt_action_;
  std::vector<std::string> source_names = prompt_source_names_;
  request_close_prompt();
  emit_action_request(action, source_names, value);
  set_status(action == pending_action::compress ? "Compressing..." : "Extracting...");
}

// ── Overwrite confirmation dialog ────────────────────────────────────────────

void zip::show_overwrite_confirm(
    pending_action action, const std::vector<std::string>& source_names, const std::string& target_name,
    const std::string& message) {
  // action/source_names/target_name are captured by value rather than
  // stashed in member fields: the callbacks are the only place that needs
  // them.
  const bool compress = action == pending_action::compress;
  show_confirm(
      message,
      [this, action, source_names, target_name, compress] {
        emit_action_request(action, source_names, target_name);
        set_status(compress ? "Compressing..." : "Extracting...");
      },
      {.title = "Confirm Overwrite",
       .on_no = [this, compress] { set_status(compress ? "Compress cancelled." : "Extract cancelled."); }});
}

// ── Contents panel ────────────────────────────────────────────────────────────

void zip::show_contents_panel(const std::string& zip_name, const std::vector<archive_entry>& entries) {
  std::uint64_t total_uncompressed = 0, total_compressed = 0;
  for (auto& e : entries) {
    if (!e.is_dir) {
      total_uncompressed += e.uncompressed_size;
      total_compressed += e.compressed_size;
    }
  }

  std::ostringstream summary;
  summary << zip_name << ": " << entries.size() << (entries.size() == 1 ? " entry, " : " entries, ")
          << format_bytes(total_uncompressed) << " uncompressed / " << format_bytes(total_compressed)
          << " compressed";
  if (contents_summary_ptr_)
    contents_summary_ptr_["text"_key] = summary.str();

  fill_contents_table(contents_table_ptr_, entries);
}

// ── Event routing ─────────────────────────────────────────────────────────────

void zip::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any panel's X button -> tear the whole tool down (top/pix's rule).
  if (event == "closed"_key && (id == window_id_ || id == contents_window_id_ || id == actions_window_id_)) {
    emit("closed"_key);
    remove_panel_objects();
    remove_internal_objects();
    return;
  }

  if (id == file_table_id_) {
    if (event == "row_selected"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx >= 0 && static_cast<size_t>(idx) < entries_.size()) {
        bool ctrl = false, shift = false;
        if (auto* v = payload.findField<bool>("ctrl"_key))
          ctrl = *v;
        if (auto* v = payload.findField<bool>("shift"_key))
          shift = *v;
        apply_row_click(selected_names_, selection_anchor_, entries_, idx, ctrl, shift);
        if (selected_label_ptr_)
          selected_label_ptr_["text"_key] = describe_selection(selected_names_);
        fill_table(entries_, selected_names_);
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
      on_table_sorted(payload);
      if (selected_label_ptr_)
        selected_label_ptr_["text"_key] = describe_selection(selected_names_);
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
    auto names = selected_target_names(selected_names_, entries_);
    if (names.empty()) {
      set_status("Select one or more files/folders to compress.");
      return;
    }
    std::string default_value = names.size() == 1 ? (names[0] + ".zip") : std::string{"Archive.zip"};
    show_prompt(pending_action::compress, names, default_value);
    return;
  }

  if (id == btn_extract_id_ && event == "clicked"_key) {
    if (selected_names_.size() != 1) {
      set_status("Select a single .zip file to extract.");
      return;
    }
    const std::string& name = *selected_names_.begin();
    if (cached_entry_type(name) != "file" || !is_zip_name(name)) {
      set_status("Select a .zip file to extract.");
      return;
    }
    show_prompt(pending_action::extract, {name}, strip_zip_suffix(name));
    return;
  }

  if (id == btn_view_id_ && event == "clicked"_key) {
    if (selected_names_.size() != 1) {
      set_status("Select a single .zip file to view its contents.");
      return;
    }
    const std::string& name = *selected_names_.begin();
    if (cached_entry_type(name) != "file" || !is_zip_name(name)) {
      set_status("Select a .zip file to view its contents.");
      return;
    }
    dynamic req;
    req["path"_key] = path_;
    req["name"_key] = name;
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
          attr<Description>("Text shown in the Actions panel's status line."),
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
