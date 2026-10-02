// MIT License © 2026 Binary Dice Games
/// @file pkg.cpp
/// @brief Implementation of the PkgFrontend form.
///
/// A close port of modules/bdg/dev/pip/server/pip.cpp: inline JSON window
/// layouts + import_json(), C++-built table rows, a per-row `...` MenuButton,
/// show_confirm() via a privately-instantiated MessageBox, and an id ->
/// handler dispatch map. The two list windows (Packages / Search) share one
/// build_list_window() / clear_list_rows() / add_list_row() path.
///
/// Unlike pip's, the lists here are large (a Debian system has thousands of
/// packages), so the form keeps the last snapshot and builds table rows only
/// for the entries that pass the filter, up to kMaxRows.
#include "pkg.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/dock_layout_spec.hpp>
#include <ui/forms/message_box.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
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

// Optional string field (absent -> "").
std::string str_of(const dynamic& d, key_t key) {
  const auto* f = d.findField<std::string>(key);
  return f ? *f : std::string{};
}

// Optional bool field (absent -> false).
bool flag_of(const dynamic& d, key_t key) {
  const auto* f = d.findField<bool>(key);
  return f && *f;
}

// "#RRGGBBAA" light/dark pairs -- GitHub Primer tokens, the helm.cpp /
// kubectl.cpp pattern. A single text_color tuned for one theme reads poorly
// on the other.
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kIdleLight = "#656D76FF";
constexpr const char* kIdleDark = "#8B949EFF";
constexpr const char* kWarnLight = "#9A6700FF";
constexpr const char* kWarnDark = "#D29922FF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return s;
}

// A package name without dpkg's architecture qualifier ("libc6:amd64" ->
// "libc6"), the form a search result names it in.
std::string base_name(const std::string& name) {
  return name.substr(0, name.find(':'));
}

// ── Window layouts ─────────────────────────────────────────────────────────
//
// The windows carry no "pos_x"/"pos_y": each opens un-positioned and docks
// into the ambient host dockspace. The *arrangement* is seeded once at the
// end of on_init() via form::set_default_dock_layout(), then owned by
// imgui.ini like any user drag. See docs/dock-layout.md.
//
// Each list table carries both "height": -1 (stretch row in `vbox`) and
// "outer_height": -1 (fill that region) -- the load-bearing pair documented
// in helm.cpp / kubectl.cpp / docker.cpp. The fixed column widths are sized
// so the trailing `...` action column stays on screen in the default dock
// arrangement on a 1280px-wide viewport (see helm.cpp).

static constexpr const char* kPackagesLayout = R"json({
  "type": "Window", "title": "Packages", "width": 900, "height": 520,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh":     { "type": "Button", "label": "Refresh" },
      "btn_outdated":    { "type": "Button", "label": "Check updates" },
      "filter":          { "type": "InputText", "hint": "Filter", "width": 110 },
      "state":           { "type": "Combo", "items": "All\nUpgradable", "value": 0, "width": 110 },
      "spring":          { "type": "Spring" },
      "btn_index":       { "type": "Button", "label": "Update index" },
      "btn_upgrade_all": { "type": "Button", "label": "Upgrade all" }
    } },
    "find_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":     { "type": "Label", "text": "Find new packages:" },
      "query":       { "type": "InputText", "hint": "name or keyword, e.g. htop", "width": 300 },
      "btn_search":  { "type": "Button", "label": "Search" },
      "btn_install": { "type": "Button", "label": "Install" }
    } },
    "env": { "type": "Label", "text": "" },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pkg_packages_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":    { "type": "TableColumn", "label": "Name",        "flags": "WidthFixed", "init_width": 200, "column_id": 0 },
        "col_version": { "type": "TableColumn", "label": "Version",     "flags": "WidthFixed", "init_width": 150, "column_id": 1 },
        "col_latest":  { "type": "TableColumn", "label": "Latest",      "flags": "WidthFixed", "init_width": 150, "column_id": 2 },
        "col_desc":    { "type": "TableColumn", "label": "Description", "flags": "WidthStretch",                   "column_id": 3 },
        "col_actions": { "type": "TableColumn", "label": "",            "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

static constexpr const char* kSearchLayout = R"json({
  "type": "Window", "title": "Search", "width": 460, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "(no search yet)" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "Refresh" }
    } },
    "status": { "type": "Label", "text": "Type a name or keyword in Packages and press Search." },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pkg_search_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":      { "type": "TableColumn", "label": "Name",        "flags": "WidthFixed", "init_width": 150, "column_id": 0 },
        "col_installed": { "type": "TableColumn", "label": "Installed",   "flags": "WidthFixed", "init_width": 90,  "column_id": 1 },
        "col_desc":      { "type": "TableColumn", "label": "Description", "flags": "WidthStretch",                   "column_id": 2 },
        "col_actions":   { "type": "TableColumn", "label": "",            "flags": "WidthFixed", "init_width": 40,  "column_id": 3 }
      }
    }
  } } }
})json";

// Details is a toolbar + a read-only TextEditor. A TextEditor displays a
// file, so set_details_text() writes each update into the session sandbox.

static constexpr const char* kDetailsLayout = R"json({
  "type": "Window", "title": "Details", "width": 460, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "(nothing selected)" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "Refresh" }
    } },
    "sep": { "type": "Separator" },
    "editor": {
      "type": "TextEditor", "file_path": "", "language": "none", "read_only": true,
      "width": -1, "height": -1
    }
  } } }
})json";

// The Console window: a FIFO-capped `Table` tracing every command the client
// ran. "auto_scroll": true so it follows the newest row.

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 900, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##pkg_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed", "init_width": 44,  "column_id": 0 },
        "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed", "init_width": 320, "column_id": 1 },
        "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed", "init_width": 50,  "column_id": 2 },
        "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
      }
    }
  } } }
})json";

std::string plural(size_t n, const char* noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

} // namespace

// ── pkg_frontend ───────────────────────────────────────────────────────────

pkg_frontend::pkg_frontend(dynamic&& base) : form(std::move(base)) {}

void pkg_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void pkg_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

ui_element_ptr pkg_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void pkg_frontend::on_init() {
  internal_root_key_ = next_available_key("__pkg_");

  // Packages is the main root -- form::init() registers internal_root_key_
  // as this form's top-level object automatically. The other windows are
  // registered by hand inside build_list_window() / build_text_window().
  build_list_window(packages_, kPackagesLayout, internal_root_key_, [&](ui_tree& tree) {
    auto click = [&](const char* path, std::function<void()> handler) {
      tree.with(path, [&](const auto& e) { click_handlers_[wish_id_of(e)] = std::move(handler); });
    };
    tree.with("vbox.toolbar.filter", [&](const auto& e) { name_filter_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.state", [&](const auto& e) { state_combo_id_ = wish_id_of(e); });
    click("vbox.toolbar.btn_refresh", [this] { emit("refresh_requested"_key); });
    click("vbox.toolbar.btn_outdated", [this] {
      set_status(packages_, "Checking for updates ...", true);
      emit("outdated_requested"_key);
    });
    click("vbox.toolbar.btn_index", [this] {
      set_status(packages_, "Updating the package index ...", true);
      emit("index_requested"_key);
    });
    click("vbox.toolbar.btn_upgrade_all", [this] {
      show_confirm("Upgrade every package that has a newer version?", [this] {
        set_status(packages_, "Upgrading all packages ...", true);
        emit("upgrade_all_requested"_key);
      });
    });

    tree.with("vbox.find_bar.query", [&](const auto& e) { query_input_id_ = wish_id_of(e); });
    click("vbox.find_bar.btn_search", [this] {
      if (query_text_.find_first_not_of(" \t") == std::string::npos) {
        set_status(packages_, "Enter a package name or keyword to search for", false);
        return;
      }
      set_status(packages_, packages_summary_, true); // drop a stale validation message
      open_search(query_text_);
    });
    click("vbox.find_bar.btn_install", [this] {
      if (query_text_.find_first_not_of(" \t") == std::string::npos)
        set_status(packages_, "Enter the exact name of the package to install", false);
      else
        emit_install(query_text_);
    });
    tree.with("vbox.env", [&](const auto& e) { env_label_ = e; });
  });

  build_list_window(search_, kSearchLayout, internal_root_key_ + "_search", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { search_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_search_request(); };
    });
  });

  // Captured once: on_event() (the close path) runs outside dispatch, where
  // sess() is unavailable, and resource_dir is fixed for the session.
  resource_dir_ = sess().resource_dir;

  details_root_key_ = internal_root_key_ + "_details";
  build_text_window(details_root_key_, kDetailsLayout, details_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.editor", [&](const auto& e) { details_editor_ = e; });
    tree.with("vbox.toolbar.target", [&](const auto& e) { details_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_details_request(); };
    });
  });

  console_root_key_ = internal_root_key_ + "_console";
  build_text_window(console_root_key_, kConsoleLayout, console_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.table", [&](const auto& e) { console_table_ = e; });
  });

  // Seed the first-run arrangement (mirrors helm / kubectl): a wide left
  // column with Packages over a Console strip, and a narrower right column
  // with Details over Search. Owned by imgui.ini after the first run; bump
  // the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "pkg_dock", "Packages",
        layout(
            split(
                dir::left, 0.6f,
                split(dir::down, 0.24f, area({console_root_key_}), area({internal_root_key_})),
                split(dir::down, 0.4f, area({search_.root_key}), area({details_root_key_}))),
            /*version=*/1, /*target=*/"pkg_dock")));
  }

  // Initial population is triggered client-side (run_pkg() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event, which would race ahead of that wiring.
}

void pkg_frontend::build_text_window(
    const std::string& root_key, const char* layout_json, key_t& window_id_out,
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

void pkg_frontend::build_list_window(
    list_window& lw, const char* layout_json, const std::string& root_key,
    const std::function<void(ui_tree&)>& wire) {
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

// ── Details text pane ──────────────────────────────────────────────────────

void pkg_frontend::set_details_text(const std::string& text) {
  if (!details_editor_)
    return;
  // A fresh name every call: the TextEditor renderer only reloads when
  // file_path changes, so rewriting one fixed file would leave the pane
  // showing stale content. Under "private/": a package description may name
  // a private repository -- see context.hpp's resource_dir doc comment.
  std::string rel =
      "private/" + internal_root_key_ + "_details_" + std::to_string(next_details_file_seq_++) + ".txt";
  auto path = file_service::resolve_path(rel, resource_dir_, /*allow_absolute=*/false);
  if (path.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  {
    std::ofstream out(path, std::ios::binary);
    if (!out)
      return;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  remove_details_file();
  details_file_ = rel;
  details_editor_["file_path"_key] = rel;
}

void pkg_frontend::remove_details_file() {
  if (details_file_.empty())
    return;
  std::error_code ec;
  std::filesystem::remove(resource_dir_ / details_file_, ec);
  details_file_.clear();
}

void pkg_frontend::open_details(const std::string& kind, const std::string& name) {
  open_details_kind_ = kind;
  open_details_name_ = name;
  if (details_target_label_)
    details_target_label_["text"_key] = kind + ": " + name;
  emit_details_request();
}

void pkg_frontend::emit_details_request() {
  if (open_details_kind_.empty())
    return;
  dynamic p;
  p["kind"_key] = open_details_kind_;
  p["name"_key] = open_details_name_;
  emit("details_requested"_key, std::move(p));
}

void pkg_frontend::open_search(const std::string& query) {
  open_search_query_ = query;
  results_.clear();
  clear_list_rows(search_, search_rows_, next_search_key_);
  if (search_.table)
    search_.table->refresh_children_order();
  if (search_target_label_)
    search_target_label_["text"_key] = "search: " + query;
  set_status(search_, "Searching the package index ...", true);
  emit_search_request();
}

void pkg_frontend::emit_search_request() {
  if (open_search_query_.empty())
    return;
  dynamic p;
  p["query"_key] = open_search_query_;
  emit("search_requested"_key, std::move(p));
}

void pkg_frontend::emit_install(const std::string& names) {
  set_status(packages_, "Installing " + names + " ...", true);
  dynamic p;
  p["names"_key] = names;
  emit("install_requested"_key, std::move(p));
}

// ── Confirmation modal (pip_frontend::show_confirm() port) ────────────────

void pkg_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
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

void pkg_frontend::clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key) {
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

void pkg_frontend::add_list_row(
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
    menu_action_targets_[wish_id_of(mi)] = row_action{meta.scope, meta.name, it.action};
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

void pkg_frontend::set_status(list_window& lw, const std::string& text, bool ok) {
  if (!lw.status_label)
    return;
  // One line only: a multi-line error would grow the label and shift the
  // table below it. The whole message is in the progress dialog and Console.
  lw.status_label["text"_key] = text.substr(0, text.find('\n'));
  lw.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  lw.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void pkg_frontend::rebuild_package_rows() {
  clear_list_rows(packages_, package_rows_, next_package_key_);

  const std::string needle = lower(name_filter_);
  size_t matching = 0, outdated = 0;
  for (auto& p : installed_) {
    const bool is_outdated = !p.latest.empty() && p.latest != p.version;
    if (is_outdated)
      ++outdated;
    if (state_filter_ == 1 && !is_outdated)
      continue;
    if (!needle.empty() && lower(p.name).find(needle) == std::string::npos)
      continue;
    if (matching++ >= kMaxRows)
      continue;

    list_row meta;
    meta.scope = "package";
    meta.name = p.name;
    std::vector<ui_element_ptr> cells = {
        make_label(p.name),
        make_label(p.version),
        make_label(is_outdated ? p.latest : std::string{}, kWarnLight, kWarnDark),
        make_label(p.description, kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {
        {"Details", "show", false},    {"Files", "files", false},        {},
        {"Upgrade", "upgrade", false}, {"Reinstall", "reinstall", true}, {"Remove", "remove", true}};
    add_list_row(packages_, package_rows_, next_package_key_, std::move(meta), cells, items);
  }
  if (packages_.table)
    packages_.table->refresh_children_order();

  const size_t total = installed_.size();
  std::string status = total == 0 ? std::string{"No packages installed."} : plural(total, "package");
  if (total != 0 && outdated_checked_)
    status += " (" + std::to_string(outdated) + " upgradable)";
  if (matching != total && total != 0)
    status += ", " + std::to_string(matching) + " matching";
  if (matching > kMaxRows)
    status += " (showing the first " + std::to_string(kMaxRows) + "; type in the filter to narrow)";
  packages_summary_ = status;
  set_status(packages_, status, true);
}

std::string pkg_frontend::installed_version(const std::string& name) const {
  const std::string wanted = base_name(name);
  for (auto& p : installed_) {
    if (base_name(p.name) == wanted)
      return p.version;
  }
  return {};
}

void pkg_frontend::rebuild_search_rows() {
  clear_list_rows(search_, search_rows_, next_search_key_);

  size_t total = 0;
  for (auto& p : results_) {
    if (total++ >= kMaxRows)
      continue;
    list_row meta;
    meta.scope = "result";
    meta.name = p.name;
    const std::string installed = installed_version(p.name);
    std::vector<ui_element_ptr> cells = {
        make_label(p.name),
        make_label(installed, kOkLight, kOkDark),
        // A manager whose search reports a version (pacman) shows it first.
        make_label(p.version.empty() ? p.description : p.version + "  " + p.description, kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {{"Details", "show", false}, {}, {"Install", "install", false}};
    add_list_row(search_, search_rows_, next_search_key_, std::move(meta), cells, items);
  }
  if (search_.table)
    search_.table->refresh_children_order();

  std::string status =
      total == 0   ? "No package matches '" + open_search_query_ + "'."
      : total == 1 ? std::string{"1 match"}
                   : std::to_string(total) + " matches";
  if (total > kMaxRows)
    status += " (showing the first " + std::to_string(kMaxRows) + "; refine the search)";
  set_status(search_, status, true);
}

// ── RMI methods ────────────────────────────────────────────────────────────

dynamic pkg_frontend::do_update_packages(const dynamic& args) {
  installed_.clear();
  for_each_entry(args, "packages"_key, [&](const dynamic& e) {
    installed_.push_back(
        {str_of(e, "name"_key), str_of(e, "version"_key), str_of(e, "latest"_key), str_of(e, "description"_key)});
  });
  outdated_checked_ = flag_of(args, "outdated_checked"_key);
  rebuild_package_rows();
  // The Installed column of the Search table follows this snapshot.
  if (!results_.empty())
    rebuild_search_rows();
  return dynamic{};
}

dynamic pkg_frontend::do_update_search(const dynamic& args) {
  if (str_of(args, "query"_key) != open_search_query_)
    return dynamic{}; // stale response for a search the user replaced.
  results_.clear();
  for_each_entry(args, "results"_key, [&](const dynamic& e) {
    results_.push_back({str_of(e, "name"_key), str_of(e, "version"_key), {}, str_of(e, "description"_key)});
  });
  rebuild_search_rows();
  return dynamic{};
}

dynamic pkg_frontend::do_update_details(const dynamic& args) {
  if (str_of(args, "kind"_key) != open_details_kind_ || str_of(args, "name"_key) != open_details_name_)
    return dynamic{};
  if (details_target_label_)
    details_target_label_["text"_key] = str_of(args, "title"_key);
  set_details_text(str_of(args, "text"_key));
  return dynamic{};
}

dynamic pkg_frontend::do_command_result(const dynamic& args) {
  const std::string command = str_of(args, "command"_key);
  const bool ok = args.as<bool>("ok"_key);
  const std::string output = str_of(args, "output"_key);
  list_window& lw = str_of(args, "scope"_key) == "search" ? search_ : packages_;
  if (ok) {
    set_status(lw, command + ": OK", true);
  } else {
    set_status(lw, command + " failed: " + (output.empty() ? "unknown error" : output), false);
  }
  return dynamic{};
}

dynamic pkg_frontend::do_set_environment(const dynamic& args) {
  if (!env_label_)
    return dynamic{};
  const std::string elevation = str_of(args, "elevation"_key);
  std::string text = str_of(args, "manager"_key) + ": " + str_of(args, "text"_key);
  if (!elevation.empty() && elevation != "none")
    text += "  (changes run through " + elevation + ")";
  env_label_["text"_key] = text;
  env_label_["text_color_light"_key] = std::string{kIdleLight};
  env_label_["text_color_dark"_key] = std::string{kIdleDark};
  return dynamic{};
}

// ── Console window (client subprocess trace) ────── ─────────────────────────

void pkg_frontend::append_console_row(
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
  // via MenuItem.copy_text) and "Clear Console" (every row).
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
      wish_id_of(row),         wish_id_of(cell_seq),     wish_id_of(cell_command), wish_id_of(cell_exit),
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

void pkg_frontend::erase_console_row_objects(const console_row_entry& entry) {
  for (auto id : entry.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

void pkg_frontend::clear_console_rows() {
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

dynamic pkg_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      str_of(args, "command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      str_of(args, "output"_key));
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void pkg_frontend::run_row_action(const row_action& target) {
  if (target.action == "show" || target.action == "files") {
    open_details(target.action, target.name);
    return;
  }
  if (target.action == "install") {
    emit_install(target.name);
    return;
  }

  auto fire = [this, target] {
    set_status(packages_, "Running " + target.action + " " + target.name + " ...", true);
    dynamic p;
    p["name"_key] = target.name;
    p["action"_key] = target.action;
    emit("package_action_requested"_key, std::move(p));
  };
  if (target.action == "remove")
    show_confirm("Remove '" + target.name + "'? Packages that depend on it may be removed with it.", fire);
  else if (target.action == "reinstall")
    show_confirm("Reinstall '" + target.name + "'? Its files are replaced.", fire);
  else
    fire();
}

void pkg_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any window's X button -> tear everything down.
  if (event == "closed"_key && (id == packages_.window_id || id == search_.window_id ||
                                id == details_window_id_ || id == console_window_id_)) {
    remove_details_file();
    emit("closed"_key);
    remove_objects_at(search_.root_key);
    remove_objects_at(details_root_key_);
    remove_objects_at(console_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    if (id == name_filter_id_) {
      name_filter_ = payload.as<std::string>("value"_key);
      rebuild_package_rows();
    } else if (id == state_combo_id_) {
      state_filter_ = payload.as<int32_t>("value"_key);
      rebuild_package_rows();
    } else if (id == query_input_id_) {
      query_text_ = payload.as<std::string>("value"_key);
    }
    return;
  }

  if (event != "clicked"_key)
    return;

  if (auto ch = click_handlers_.find(id); ch != click_handlers_.end()) {
    const auto handler = ch->second; // copy: a handler may erase itself.
    handler();
    return;
  }

  if (auto mi = menu_action_targets_.find(id); mi != menu_action_targets_.end()) {
    const row_action target = mi->second; // copy: the action may rebuild the table.
    run_row_action(target);
  }
}

// ── Registration ───────────────────────────────────────────────────────────

void register_pkg() {
  auto proto = dynamic_ptr{"PkgFrontend"_key, {}};

  auto add_method = [&](key_t name, dynamic (pkg_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<pkg_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_packages"_key, &pkg_frontend::do_update_packages);
  add_method("update_search"_key, &pkg_frontend::do_update_search);
  add_method("update_details"_key, &pkg_frontend::do_update_details);
  add_method("command_result"_key, &pkg_frontend::do_command_result);
  add_method("append_command_log"_key, &pkg_frontend::do_append_command_log);
  add_method("set_environment"_key, &pkg_frontend::do_set_environment);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PkgFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "GUI frontend for the system package manager (apt, dnf, pacman or brew): installed packages, index "
      "search, install / upgrade / remove. All package-manager invocation happens client-side; this form only "
      "renders whatever snapshot it was last given. Listen for the 'closed' event to detect when the user is "
      "done, and the '*_requested' events to react to user actions -- see pkg.hpp's class doc comment for the "
      "full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<pkg_frontend>("wish"_key, "PkgFrontend"_key));
}

} // namespace bdg::wish
