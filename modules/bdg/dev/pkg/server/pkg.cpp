// MIT License © 2026 Binary Dice Games
/// @file pkg.cpp
/// @brief Implementation of the PkgFrontend form.
///
/// Inline JSON window layouts + C++-built table rows on the panels shared
/// by the bdg tool forms (modules/bdg/common/server). The two list windows
/// (Packages / Search) share one package_row type and run_row_action() path.
///
/// Unlike pip's, the lists here are large (a Debian system has thousands of
/// packages), so the form keeps the last snapshot and builds table rows only
/// for the entries that pass the filter, up to kMaxRows.
#include "pkg.hpp"

#include "src/bison/bison_object.hpp"

#include <ui/dock_layout_spec.hpp>

namespace bdg::wish {

using namespace bison;
using common::flag_of;
using common::for_each_entry;
using common::kIdle;
using common::kOk;
using common::kWarn;
using common::lower;
using common::make_payload;
using common::plural;
using common::str_of;
using common::wish_id_of;

namespace {

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
      "btn_refresh":     { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "btn_outdated":    { "type": "Button", "label": "Check updates", "icon": "res/icons/arrow_up.png" },
      "filter":          { "type": "InputText", "hint": "Filter", "width": 110 },
      "state":           { "type": "Combo", "items": "All\nUpgradable", "value": 0, "width": 110 },
      "spring":          { "type": "Spring" },
      "btn_index":       { "type": "Button", "label": "Update index", "icon": "res/icons/download.png" },
      "btn_upgrade_all": { "type": "Button", "label": "Upgrade all", "icon": "res/icons/arrow_up.png" }
    } },
    "find_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":     { "type": "Label", "text": "Find new packages:" },
      "query":       { "type": "InputText", "hint": "name or keyword, e.g. htop", "width": 300 },
      "btn_search":  { "type": "Button", "label": "Search", "icon": "res/icons/search.png" },
      "btn_install": { "type": "Button", "label": "Install", "icon": "res/icons/download.png" }
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
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" }
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

} // namespace

// ── pkg_frontend ───────────────────────────────────────────────────────────

pkg_frontend::pkg_frontend(dynamic&& base) : tool_form(std::move(base)) {}

void pkg_frontend::on_init() {
  internal_root_key_ = next_available_key("__pkg_");

  // Packages is the main root -- form::init() registers internal_root_key_
  // as this form's top-level object automatically; the other windows
  // register themselves (tool_form::build_window()).
  packages_.build(*this, internal_root_key_, kPackagesLayout, [&](ui_tree& tree) {
    auto click = [&](const char* path, click_handler handler) {
      tree.with(path, [&](const auto& e) { on_click(wish_id_of(e), std::move(handler)); });
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

  search_.build(*this, internal_root_key_ + "_search", kSearchLayout, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { search_target_label_ = e; });
    tree.with(
        "vbox.toolbar.btn_refresh", [&](const auto& e) { on_click(wish_id_of(e), [this] { emit_search_request(); }); });
  });

  details_.build(*this, internal_root_key_ + "_details", {.title = "Details", .width = 460, .on_refresh = [this] {
                                                            emit_details_request();
                                                          }});

  console_.build(
      *this, internal_root_key_ + "_console", {.table_id = "##pkg_console_table", .width = 900, .command_width = 320});

  // Seed the first-run arrangement (mirrors helm / kubectl): a wide left
  // column with Packages over a Console strip, and a narrower right column
  // with Details over Search. Owned by imgui.ini after the first run; bump
  // the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "pkg_dock",
        "Packages",
        layout(
            split(
                dir::left,
                0.6f,
                split(dir::down, 0.24f, area({console_.root_key()}), area({internal_root_key_})),
                split(dir::down, 0.4f, area({search_.root_key()}), area({details_.root_key()}))),
            /*version=*/1,
            /*target=*/"pkg_dock")));
  }

  // Initial population is triggered client-side (run_pkg() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event, which would race ahead of that wiring.
}

void pkg_frontend::open_details(const std::string& kind, const std::string& name) {
  open_details_kind_ = kind;
  open_details_name_ = name;
  details_.set_title(kind + ": " + name);
  emit_details_request();
}

void pkg_frontend::emit_details_request() {
  if (open_details_kind_.empty())
    return;
  emit("details_requested"_key, make_payload("kind"_key, open_details_kind_, "name"_key, open_details_name_));
}

void pkg_frontend::open_search(const std::string& query) {
  open_search_query_ = query;
  results_.clear();
  search_.clear();
  search_.refresh();
  if (search_target_label_)
    search_target_label_["text"_key] = "search: " + query;
  set_status(search_, "Searching the package index ...", true);
  emit_search_request();
}

void pkg_frontend::emit_search_request() {
  if (open_search_query_.empty())
    return;
  emit("search_requested"_key, make_payload("query"_key, open_search_query_));
}

void pkg_frontend::emit_install(const std::string& names) {
  set_status(packages_, "Installing " + names + " ...", true);
  emit("install_requested"_key, make_payload("names"_key, names));
}

common::menu_item
pkg_frontend::action_item(const std::string& label, const package_row& r, const std::string& action, bool confirm) {
  return {label, [this, r, action] { run_row_action(r, action); }, confirm};
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void pkg_frontend::rebuild_package_rows() {
  packages_.clear();

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

    package_row r{"package", p.name};
    std::vector<ui_element_ptr> cells = {
        make_label(p.name),
        make_label(p.version),
        make_label(is_outdated ? p.latest : std::string{}, kWarn),
        make_label(p.description, kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Details", r, "show", false),
        action_item("Files", r, "files", false),
        {},
        action_item("Upgrade", r, "upgrade", false),
        action_item("Reinstall", r, "reinstall", true),
        action_item("Remove", r, "remove", true)};
    packages_.add(std::move(r), cells, items);
  }
  packages_.refresh();

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
  search_.clear();

  size_t total = 0;
  for (auto& p : results_) {
    if (total++ >= kMaxRows)
      continue;
    package_row r{"result", p.name};
    const std::string installed = installed_version(p.name);
    std::vector<ui_element_ptr> cells = {
        make_label(p.name),
        make_label(installed, kOk),
        // A manager whose search reports a version (pacman) shows it first.
        make_label(p.version.empty() ? p.description : p.version + "  " + p.description, kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Details", r, "show", false), {}, action_item("Install", r, "install", false)};
    search_.add(std::move(r), cells, items);
  }
  search_.refresh();

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
  details_.set_title(str_of(args, "title"_key));
  details_.set_text(str_of(args, "text"_key));
  return dynamic{};
}

dynamic pkg_frontend::do_command_result(const dynamic& args) {
  list_window& lw = str_of(args, "scope"_key) == "search" ? search_ : packages_;
  bool ok = false;
  const std::string text = common::command_result_text(args, ok);
  set_status(lw, text, ok);
  return dynamic{};
}

dynamic pkg_frontend::do_set_environment(const dynamic& args) {
  if (!env_label_)
    return dynamic{};
  const std::string elevation = str_of(args, "elevation"_key);
  std::string text = str_of(args, "manager"_key) + ": " + str_of(args, "text"_key);
  if (!elevation.empty() && elevation != "none")
    text += "  (changes run through " + elevation + ")";
  common::set_status_text(env_label_, text, true);
  return dynamic{};
}

dynamic pkg_frontend::do_append_command_log(const dynamic& args) {
  console_.append_from(args);
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void pkg_frontend::run_row_action(const package_row& r, const std::string& action) {
  if (action == "show" || action == "files") {
    open_details(action, r.name);
    return;
  }
  if (action == "install") {
    emit_install(r.name);
    return;
  }

  auto fire = [this, name = r.name, action] {
    set_status(packages_, "Running " + action + " " + name + " ...", true);
    emit("package_action_requested"_key, make_payload("name"_key, name, "action"_key, action));
  };
  if (action == "remove")
    show_confirm("Remove '" + r.name + "'? Packages that depend on it may be removed with it.", fire);
  else if (action == "reinstall")
    show_confirm("Reinstall '" + r.name + "'? Its files are replaced.", fire);
  else
    fire();
}

void pkg_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == packages_.window_id() || id == search_.window_id() || id == details_.window_id() ||
       id == console_.window_id())) {
    details_.remove_file();
    emit("closed"_key);
    remove_objects_at(search_.root_key());
    remove_objects_at(details_.root_key());
    remove_objects_at(console_.root_key());
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

  if (event == "clicked"_key)
    dispatch_click(id);
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
