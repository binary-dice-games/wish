// MIT License © 2026 Binary Dice Games
/// @file du.cpp
/// @brief Implementation of the Du form.
///
/// No filesystem access here: the folder being analyzed lives on the
/// client's machine, so the scan itself happens in
/// modules/bdg/desktop/du/client/. See du.hpp's class doc comment for the
/// client/server handshake.
#include "du.hpp"

#include "src/bison/bison_object.hpp"
#include "ui/forms/file_browser_utils.hpp"

#include <ui/dock_layout_spec.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace bdg::wish {

using namespace bison;

namespace {

// Duplicated from the client (du_scan.cpp) rather than shared, like zip's
// and mc's own server/client copies: a module's server and client halves
// are compiled into different targets.
std::string format_bytes(std::uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
  return buf;
}

std::string format_count(std::uint64_t n) {
  std::string digits = std::to_string(n);
  for (ptrdiff_t i = static_cast<ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast<size_t>(i), 1, ',');
  return digits;
}

// Sizes cross the wire as decimal strings: bison's field variant has no
// 64-bit integer, and a float loses bytes above 16 MB.
std::uint64_t parse_bytes(const std::string& text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    return 0;
  return std::strtoull(text.c_str(), nullptr, 10);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  if (text.empty())
    return out;
  size_t begin = 0;
  while (true) {
    const size_t eol = text.find('\n', begin);
    out.push_back(text.substr(begin, eol == std::string::npos ? eol : eol - begin));
    if (eol == std::string::npos)
      return out;
    begin = eol + 1;
  }
}

std::string plural(std::uint64_t n, const char* one, const char* many) {
  return format_count(n) + " " + (n == 1 ? one : many);
}

} // namespace

/// Initial width in pixels of the Files panel; the Treemap gets the rest.
static constexpr float kFilesPanelWidth = 520.0f;

// ── UI layouts ────────────────────────────────────────────────────────────────
//
// Two dockable panels side by side, seeded into a first-run arrangement by on_init()'s
// set_default_dock_layout() call (the multi-window pattern zip, top and pix
// use); imgui.ini owns the arrangement afterwards. Each Window keeps a
// width/height: the size a panel restores to when dragged out of the dock.
//
// Files (the form's main root): path_input is EnterReturnsTrue, so it only
// fires "changed" on Enter -- that is what starts a scan. It comes last in
// its row so its "width": -1 fills what the buttons leave. table carries
// "height": -1 (zip's file_table technique) so it takes whatever the rows
// above and the status line below it leave. col_size is the default sort
// column, largest first.
//
// Treemap: the Treemap element fills its window (its width/height default
// to -1). "padding": 1 keeps deeply nested folders from eating the area.
static constexpr const char* kFilesLayout = R"json({
  "type": "Window",
  "title": "Files",
  "width": 520, "height": 600,
  "closable": true,
  "children": {
    "main": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 6,
          "children": {
            "btn_scan": { "type": "Button", "label": "Rescan", "width": 80 },
            "btn_up": { "type": "Button", "label": "Up", "width": 50 },
            "path_input": { "type": "InputText", "hint": "Folder to analyze (Enter to scan)...", "value": "", "flags": "EnterReturnsTrue", "width": -1 }
          }
        },
        "summary": { "type": "Label", "text": "No folder scanned yet." },
        "table": {
          "type": "Table", "columns": 4, "headers": true,
          "flags": "Resizable|Sortable|RowBg|Borders|ScrollY", "outer_width": 0, "height": -1,
          "auto_scroll": false,
          "children": {
            "col_name":    { "type": "TableColumn", "label": "Name", "column_id": 0 },
            "col_percent": { "type": "TableColumn", "label": "% of Folder", "flags": "WidthFixed", "init_width": 120, "column_id": 1 },
            "col_size":    { "type": "TableColumn", "label": "Size", "flags": "WidthFixed|DefaultSort|PreferSortDescending", "init_width": 90, "column_id": 2 },
            "col_items":   { "type": "TableColumn", "label": "Items", "flags": "WidthFixed", "init_width": 80, "column_id": 3 }
          }
        },
        "status": { "type": "Label", "text": "Ready." }
      }
    }
  }
})json";

static constexpr const char* kTreemapLayout = R"json({
  "type": "Window",
  "title": "Treemap",
  "width": 900, "height": 420,
  "closable": true,
  "children": {
    "map": { "type": "Treemap", "padding": 1 }
  }
})json";

// ── du ───────────────────────────────────────────────────────────────────────

du::du(dynamic&& base) : tool_form(std::move(base)) {}

du::~du() {
  remove_panel_objects();
}

void du::on_init() {
  internal_root_key_ = next_available_key("__du_");
  treemap_root_key_ = internal_root_key_ + "_treemap";

  auto* title_f = findField<std::string>("title"_key);
  const std::string title = title_f ? *title_f : std::string{"Disk Usage"};

  build_window(internal_root_key_, kFilesLayout, window_id_, [&](ui_tree& tree) {
    tree.with("main.toolbar.btn_scan", [&](const auto& e) {
      btn_scan_ptr_ = e;
      btn_scan_id_ = wish_id_of(e);
    });
    tree.with("main.toolbar.btn_up", [&](const auto& e) { btn_up_id_ = wish_id_of(e); });
    tree.with("main.toolbar.path_input", [&](const auto& e) {
      path_input_ptr_ = e;
      path_input_id_ = wish_id_of(e);
    });
    tree.with("main.summary", [&](const auto& e) { summary_label_ptr_ = e; });
    tree.with("main.table", [&](const auto& e) {
      table_ptr_ = e;
      table_id_ = wish_id_of(e);
    });
    tree.with("main.status", [&](const auto& e) { status_label_ptr_ = e; });
  });

  build_window(treemap_root_key_, kTreemapLayout, treemap_window_id_, [&](ui_tree& tree) {
    tree.with("map", [&](const auto& e) {
      treemap_ptr_ = e;
      treemap_id_ = wish_id_of(e);
    });
  });

  // First-run arrangement inside the tool's own nested dockspace (titled
  // with the form's "title" field): Files as a fixed-width panel on the
  // left, the Treemap taking the rest of the screen. Owned by imgui.ini
  // after the first run (see docs/dock-layout.md); bump the version arg to
  // layout() if it changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "du_dock", title,
        layout(
            split_px(dir::left, kFilesPanelWidth, area({internal_root_key_}), area({treemap_root_key_})),
            /*version=*/2, /*target=*/"du_dock")));
  }
}

void du::remove_panel_objects() {
  // Forgotten once removed: next_available_key() may hand this form's freed
  // internal_root_key_ to a new Du instance, whose panel would reuse the key.
  remove_objects_at(treemap_root_key_);
  treemap_root_key_.clear();
}

// ── Table ────────────────────────────────────────────────────────────────────

void du::fill_table() {
  if (!table_ptr_)
    return;
  auto* children_p = table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  // Drops the previous rows (indexed); the named TableColumn children remain.
  children->clear();

  auto make_label = [](const std::string& text, int32_t order) {
    ui_element_ptr lbl = ui_element_ptr::create("wish"_key, "Label"_key);
    lbl["text"_key] = text;
    lbl["order"_key] = order;
    return lbl;
  };

  int32_t idx = 0;
  auto add_row = [&](ui_element_ptr name, ui_element_ptr percent, const std::string& size,
                     const std::string& items, bool selected) {
    ui_element_ptr row_el = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row_el["order"_key] = idx;
    row_el["selected"_key] = selected;
    auto cells = dynamic_ptr{key_t{0U}, {}};
    (*cells)[size_t{0}] = dynamic_ptr{name};
    (*cells)[size_t{1}] = dynamic_ptr{percent};
    (*cells)[size_t{2}] = dynamic_ptr{make_label(size, 2)};
    (*cells)[size_t{3}] = dynamic_ptr{make_label(items, 3)};
    row_el["children"_key] = cells;
    (*children)[static_cast<size_t>(idx)] = dynamic_ptr{row_el};
    ++idx;
  };

  if (has_parent())
    add_row(make_name_cell("..", "dir", ".."), make_label({}, 1), {}, {}, false);

  for (auto& r : rows_) {
    const float share = dir_size_ == 0 ? 0.0f : static_cast<float>(static_cast<double>(r.size) / dir_size_);
    char percent_text[16];
    std::snprintf(percent_text, sizeof(percent_text), "%.1f %%", share * 100.0f);

    ui_element_ptr bar = ui_element_ptr::create("wish"_key, "ProgressBar"_key);
    bar["value"_key] = share;
    bar["label"_key] = std::string{percent_text};
    bar["width"_key] = -1.0f;
    bar["order"_key] = int32_t{1};

    // A "rest" row is shown as a plain file: it has no icon of its own.
    add_row(
        make_name_cell(r.name, r.type == "dir" ? "dir" : "file", r.name), bar, format_bytes(r.size),
        r.type == "file" ? std::string{} : format_count(static_cast<std::uint64_t>(r.items)),
        r.name == selected_name_);
  }
  table_ptr_->refresh_children_order();
}

void du::sort_rows() {
  auto key_less = [&](const row& a, const row& b) {
    switch (sort_column_id_) {
      case 0:
        return ascii_ci_less(a.name, b.name);
      case 3:
        return a.items < b.items;
      default: // % of Folder (1) and Size (2) order alike.
        return a.size < b.size;
    }
  };
  std::stable_sort(rows_.begin(), rows_.end(), [&](const row& a, const row& b) {
    // The "rest" row stands for many entries; it stays last in any order.
    if ((a.type == "rest") != (b.type == "rest"))
      return b.type == "rest";
    return sort_ascending_ ? key_less(a, b) : key_less(b, a);
  });
}

void du::set_status(const std::string& message) {
  (*this)["status"_key] = message;
  if (status_label_ptr_)
    status_label_ptr_["text"_key] = message;
}

std::int32_t du::top_level_node(std::int32_t index) const {
  if (index <= 0 || static_cast<size_t>(index) >= node_parents_.size())
    return -1;
  // A parent always precedes its children, so this walk ends at node 0.
  while (node_parents_[static_cast<size_t>(index)] > 0)
    index = node_parents_[static_cast<size_t>(index)];
  return node_parents_[static_cast<size_t>(index)] == 0 ? index : -1;
}

void du::select(const std::string& name) {
  selected_name_ = name;
  fill_table();

  int32_t node = -1;
  if (!name.empty())
    for (size_t i = 1; i < node_parents_.size() && i < node_labels_.size(); ++i)
      if (node_parents_[i] == 0 && node_labels_[i] == name) {
        node = static_cast<int32_t>(i);
        break;
      }
  if (treemap_ptr_)
    treemap_ptr_["selected"_key] = node;
}

std::string du::display_path() const {
  if (rel_path_.empty())
    return root_;
  return !root_.empty() && (root_.back() == '/' || root_.back() == '\\') ? root_ + rel_path_ : root_ + "/" + rel_path_;
}

void du::navigate(const std::string& rel_path) {
  dynamic nav;
  nav["path"_key] = rel_path;
  emit("on_navigate"_key, std::move(nav));
}

void du::open_entry(const std::string& name) {
  if (name == "..") {
    const size_t slash = rel_path_.find_last_of('/');
    navigate(slash == std::string::npos ? std::string{} : rel_path_.substr(0, slash));
    return;
  }
  navigate(rel_path_.empty() ? name : rel_path_ + "/" + name);
}

// ── RMI methods ───────────────────────────────────────────────────────────────

dynamic du::do_show_directory(const dynamic& args) {
  root_ = args.as<std::string>("root"_key);
  rel_path_ = args.as<std::string>("path"_key);
  dir_size_ = parse_bytes(args.as<std::string>("size"_key));
  int32_t files = 0, dirs = 0;
  if (auto* v = args.findField<int32_t>("files"_key))
    files = *v;
  if (auto* v = args.findField<int32_t>("dirs"_key))
    dirs = *v;

  rows_.clear();
  if (auto* entries_f = args.findField<dynamic_ptr>("entries"_key); entries_f && *entries_f) {
    (*entries_f)->forEach([&](key_t, const field& f) {
      auto* ep = f.get<dynamic_ptr>();
      if (!ep || !*ep)
        return;
      const auto& e = **ep;
      row r;
      r.name = e.as<std::string>("name"_key);
      r.type = e.as<std::string>("type"_key);
      r.size = parse_bytes(e.as<std::string>("size"_key));
      if (auto* v = e.findField<int32_t>("items"_key))
        r.items = *v;
      rows_.push_back(std::move(r));
    });
  }
  sort_rows();

  // The treemap's data is forwarded untouched; a copy of its structure is
  // kept to resolve the node index of a later click.
  node_parents_.clear();
  node_kinds_.clear();
  std::vector<float> sizes;
  std::vector<int32_t> colors;
  std::string labels, details;
  if (auto* v = args.findField<std::vector<int32_t>>("parents"_key))
    node_parents_ = *v;
  if (auto* v = args.findField<std::vector<int32_t>>("kinds"_key))
    node_kinds_ = *v;
  if (auto* v = args.findField<std::vector<float>>("sizes"_key))
    sizes = *v;
  if (auto* v = args.findField<std::vector<int32_t>>("colors"_key))
    colors = *v;
  if (auto* v = args.findField<std::string>("labels"_key))
    labels = *v;
  if (auto* v = args.findField<std::string>("details"_key))
    details = *v;
  node_labels_ = split_lines(labels);
  node_details_ = split_lines(details);
  if (treemap_ptr_) {
    treemap_ptr_["parents"_key] = node_parents_;
    treemap_ptr_["sizes"_key] = std::move(sizes);
    treemap_ptr_["colors"_key] = std::move(colors);
    treemap_ptr_["labels"_key] = std::move(labels);
    treemap_ptr_["details"_key] = std::move(details);
  }

  // A new folder: whatever was selected does not exist here.
  select({});

  const std::string shown = display_path();
  (*this)["path"_key] = shown;
  if (path_input_ptr_)
    path_input_ptr_["value"_key] = shown;
  if (summary_label_ptr_)
    // The path itself is in the path bar just above; repeating it here
    // would not fit the panel's width.
    summary_label_ptr_["text"_key] = format_bytes(dir_size_) + " in " +
        plural(static_cast<std::uint64_t>(files), "file", "files") + ", " +
        plural(static_cast<std::uint64_t>(dirs), "folder", "folders");
  return dynamic{};
}

// ── on_set ────────────────────────────────────────────────────────────────────

dynamic du::on_set(const dynamic& patch) {
  if (auto* v = patch.findField<std::string>("status"_key); v && status_label_ptr_)
    status_label_ptr_["text"_key] = *v;
  if (auto* v = patch.findField<bool>("scanning"_key)) {
    scanning_ = *v;
    if (btn_scan_ptr_)
      btn_scan_ptr_["label"_key] = std::string{scanning_ ? "Stop" : "Rescan"};
  }
  return patch;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void du::on_event(key_t id, key_t event, const dynamic& payload) {
  // Either panel's X button -> tear the whole tool down (zip/top/pix's rule).
  if (event == "closed"_key && (id == window_id_ || id == treemap_window_id_)) {
    emit("closed"_key);
    remove_panel_objects();
    remove_internal_objects();
    return;
  }

  auto request_scan = [&](const std::string& path) {
    if (path.empty()) {
      set_status("Enter a folder to analyze.");
      return;
    }
    dynamic req;
    req["path"_key] = path;
    emit("on_scan_requested"_key, std::move(req));
  };

  if (id == path_input_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<std::string>("value"_key))
      request_scan(*v);
    return;
  }

  if (id == btn_scan_id_ && event == "clicked"_key) {
    if (scanning_)
      emit("on_cancel_requested"_key);
    else
      request_scan(root_);
    return;
  }

  if (id == btn_up_id_ && event == "clicked"_key) {
    if (has_parent())
      open_entry("..");
    else
      set_status("Already at the scanned folder. Enter a path to analyze a different one.");
    return;
  }

  if (id == table_id_) {
    if (event == "sorted"_key) {
      auto* col_f = payload.findField<int32_t>("column_id"_key);
      auto* asc_f = payload.findField<bool>("ascending"_key);
      if (!col_f || !asc_f)
        return;
      sort_column_id_ = *col_f;
      sort_ascending_ = *asc_f;
      sort_rows();
      fill_table();
      return;
    }
    if (event == "row_selected"_key || event == "row_activated"_key) {
      // Row 0 is ".." below the scanned root; the entries follow it.
      int32_t idx = payload.as<int32_t>("index"_key) - (has_parent() ? 1 : 0);
      const bool activated = event == "row_activated"_key;
      if (idx == -1) {
        if (activated)
          open_entry("..");
        return;
      }
      if (idx < 0 || static_cast<size_t>(idx) >= rows_.size())
        return;
      const row r = rows_[static_cast<size_t>(idx)];
      if (activated && r.type == "dir") {
        open_entry(r.name);
        return;
      }
      select(r.name);
      set_status(display_path() + "/" + r.name + "  -  " + format_bytes(r.size));
    }
    return;
  }

  if (id == treemap_id_ && (event == "clicked"_key || event == "activated"_key)) {
    const int32_t index = payload.as<int32_t>("index"_key);
    const int32_t top = top_level_node(index);
    if (top < 0 || static_cast<size_t>(top) >= node_labels_.size())
      return;
    const std::string& top_name = node_labels_[static_cast<size_t>(top)];
    const bool top_is_dir = static_cast<size_t>(top) < node_kinds_.size() && node_kinds_[static_cast<size_t>(top)] == 1;
    if (event == "activated"_key && top_is_dir) {
      open_entry(top_name);
      return;
    }

    // The clicked node's path below the displayed folder, then its size.
    std::string chain;
    for (int32_t i = index; i > 0; i = node_parents_[static_cast<size_t>(i)]) {
      const std::string& label = static_cast<size_t>(i) < node_labels_.size() ? node_labels_[static_cast<size_t>(i)]
                                                                               : std::string{};
      chain = chain.empty() ? label : label + "/" + chain;
    }
    std::string message = display_path() + "/" + chain;
    if (static_cast<size_t>(index) < node_details_.size() && !node_details_[static_cast<size_t>(index)].empty())
      message += "  -  " + node_details_[static_cast<size_t>(index)];

    const bool listed =
        std::any_of(rows_.begin(), rows_.end(), [&](const row& r) { return r.name == top_name; });
    select(listed ? top_name : std::string{});
    if (treemap_ptr_)
      treemap_ptr_["selected"_key] = index;
    set_status(message);
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_du() {
  auto proto = dynamic_ptr{"Du"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Disk Usage"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addField(
      "path"_key,
      field{
          std::string{""},
          attr<DisplayName>("Path"),
          attr<Description>("Client-owned local folder currently displayed. Updated via "
                            "show_directory(); read-only from the client's perspective otherwise."),
          attr<Category>("Data")});

  proto->addField(
      "status"_key,
      field{
          std::string{"Ready."},
          attr<DisplayName>("Status"),
          attr<Description>("Text shown in the status line, e.g. the progress of a scan."),
          attr<Category>("Data")});

  proto->addField(
      "scanning"_key,
      field{
          bool{false},
          attr<DisplayName>("Scanning"),
          attr<Description>("True while the client is scanning: the Rescan button reads Stop and emits "
                            "on_cancel_requested instead of on_scan_requested."),
          attr<Category>("State")});

  proto->addMethod(
      "show_directory"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<du&>(self).do_show_directory(args);
      }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<du&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Du"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Disk usage analyzer for a folder on the client's machine: a size-sorted table of the current "
      "folder plus a treemap of its whole subtree. The server owns the UI only; listen for "
      "on_scan_requested/on_cancel_requested/on_navigate to drive the client half of the handshake "
      "(answering with show_directory()), and 'closed' to detect when the user is done."));

  dynamic::addClass("wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<du>("wish"_key, "Du"_key));
}

} // namespace bdg::wish
