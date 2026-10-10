// MIT License © 2026 Binary Dice Games
/// @file helm.cpp
/// @brief Implementation of the HelmFrontend form.
///
/// Inline JSON window layouts + C++-built table rows on the panels shared
/// by the bdg tool forms (modules/bdg/common/server). The four list windows
/// (Releases / Repositories / Charts / History) share one
/// build_list_window() / run_row_action() path.
#include "helm.hpp"

#include "src/bison/bison_object.hpp"

#include <ui/dock_layout_spec.hpp>

#include <algorithm>

namespace bdg::wish {

using namespace bison;
using common::for_each_entry;
using common::kBad;
using common::kIdle;
using common::kOk;
using common::kWarn;
using common::make_payload;
using common::plural;
using common::str_of;
using common::theme_color;
using common::wish_id_of;

namespace {

bool starts_with(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

// Release status ("deployed", "failed", "pending-install", "uninstalling",
// "superseded", "uninstalled", "unknown") -> colour.
theme_color release_status_colour(const std::string& status) {
  if (status == "deployed")
    return kOk;
  if (status == "failed")
    return kBad;
  if (starts_with(status, "pending") || status == "uninstalling")
    return kWarn;
  return kIdle;
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
// in kubectl.cpp / docker.cpp / git.cpp. The fixed column widths are sized so
// the trailing `...` action column stays on screen in the default dock
// arrangement on a 1280px-wide viewport: a table whose fixed columns (plus
// cell padding) exceed the window overflows horizontally and scrolls the
// actions out of view, while the one WidthStretch column just gets clipped.
//
// Every window sharing the Releases tab group, other than Releases itself,
// carries "NoFocusOnAppearing": a window that takes focus on its first frame
// also takes its dock tab group's selection, overriding the `focused` window
// named in the DockArea below.

static constexpr const char* kReleasesLayout = R"json({
  "type": "Window", "title": "Releases", "width": 980, "height": 500,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "filter":      { "type": "InputText", "hint": "Filter by name", "width": 200 },
      "ns":          { "type": "InputText", "hint": "Namespace", "width": 150 },
      "state":       { "type": "Combo", "items": "All\nDeployed\nFailed\nPending", "value": 0, "width": 120 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##helm_releases_table", "columns": 8,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_ns":      { "type": "TableColumn", "label": "Namespace",   "flags": "WidthFixed", "init_width": 90,  "column_id": 0 },
        "col_name":    { "type": "TableColumn", "label": "Name",        "flags": "WidthStretch",                    "column_id": 1 },
        "col_rev":     { "type": "TableColumn", "label": "Revision",    "flags": "WidthFixed", "init_width": 56,  "column_id": 2 },
        "col_status":  { "type": "TableColumn", "label": "Status",      "flags": "WidthFixed", "init_width": 86,  "column_id": 3 },
        "col_chart":   { "type": "TableColumn", "label": "Chart",       "flags": "WidthFixed", "init_width": 130, "column_id": 4 },
        "col_app":     { "type": "TableColumn", "label": "App Version", "flags": "WidthFixed", "init_width": 64,  "column_id": 5 },
        "col_updated": { "type": "TableColumn", "label": "Updated",     "flags": "WidthFixed", "init_width": 140, "column_id": 6 },
        "col_actions": { "type": "TableColumn", "label": "",            "flags": "WidthFixed", "init_width": 40,  "column_id": 7 }
      }
    }
  } } }
})json";

static constexpr const char* kReposLayout = R"json({
  "type": "Window", "title": "Repositories", "width": 820, "height": 360,
  "closable": true, "flags": "NoFocusOnAppearing",
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh":    { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" },
      "btn_update_all": { "type": "Button", "label": "Update all", "icon": "res/icons/download.png" },
      "filter":         { "type": "InputText", "hint": "Filter by name", "width": 200 }
    } },
    "add_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "repo_name": { "type": "InputText", "hint": "new repository name", "width": 200 },
      "repo_url":  { "type": "InputText", "hint": "https://charts.example.com", "width": 320 },
      "btn_add":   { "type": "Button", "label": "Add", "icon": "res/icons/add.png" }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##helm_repos_table", "columns": 3,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":    { "type": "TableColumn", "label": "Name", "flags": "WidthFixed", "init_width": 200, "column_id": 0 },
        "col_url":     { "type": "TableColumn", "label": "URL",  "flags": "WidthStretch",                   "column_id": 1 },
        "col_actions": { "type": "TableColumn", "label": "",     "flags": "WidthFixed", "init_width": 40,  "column_id": 2 }
      }
    }
  } } }
})json";

static constexpr const char* kChartsLayout = R"json({
  "type": "Window", "title": "Charts", "width": 900, "height": 420,
  "closable": true, "flags": "NoFocusOnAppearing",
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "query":      { "type": "InputText", "hint": "Search repositories (empty = all charts)", "width": 320 },
      "btn_search": { "type": "Button", "label": "Search", "icon": "res/icons/search.png" },
      "spring":      { "type": "Spring" },
      "btn_install": { "type": "Button", "label": "Install chart...", "icon": "res/icons/download.png" }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##helm_charts_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":    { "type": "TableColumn", "label": "Name",          "flags": "WidthFixed", "init_width": 230, "column_id": 0 },
        "col_ver":     { "type": "TableColumn", "label": "Chart Version", "flags": "WidthFixed", "init_width": 100, "column_id": 1 },
        "col_app":     { "type": "TableColumn", "label": "App Version",   "flags": "WidthFixed", "init_width": 90,  "column_id": 2 },
        "col_desc":    { "type": "TableColumn", "label": "Description",   "flags": "WidthStretch",                    "column_id": 3 },
        "col_actions": { "type": "TableColumn", "label": "",              "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

static constexpr const char* kHistoryLayout = R"json({
  "type": "Window", "title": "History", "width": 820, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "(no release selected)" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##helm_history_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_rev":     { "type": "TableColumn", "label": "Revision",    "flags": "WidthFixed", "init_width": 50,  "column_id": 0 },
        "col_updated": { "type": "TableColumn", "label": "Updated",     "flags": "WidthFixed", "init_width": 150, "column_id": 1 },
        "col_status":  { "type": "TableColumn", "label": "Status",      "flags": "WidthFixed", "init_width": 86,  "column_id": 2 },
        "col_chart":   { "type": "TableColumn", "label": "Chart",       "flags": "WidthFixed", "init_width": 100, "column_id": 3 },
        "col_desc":    { "type": "TableColumn", "label": "Description", "flags": "WidthStretch",                    "column_id": 4 },
        "col_actions": { "type": "TableColumn", "label": "",            "flags": "WidthFixed", "init_width": 40,  "column_id": 5 }
      }
    }
  } } }
})json";

// The Install / Upgrade dialog. Built on demand (open_install_dialog()), not
// in on_init(): "pos_x"/"pos_y" + NoDocking make it a floating window that
// opens in front of the docked ones. The chart box takes anything `helm
// install` does -- "repo/chart", an "oci://" reference, a URL or a path on
// the client machine -- so a chart needn't come from the Charts table.

static constexpr const char* kInstallLayout = R"json({
  "type": "Window", "title": "Install chart", "width": 660, "height": 540,
  "pos_x": 200, "pos_y": 80, "closable": true, "flags": "NoDocking|NoCollapse",
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 6, "children": {
    "chart_row": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "chart":   { "type": "InputText", "hint": "chart: repo/name, oci://..., URL or path", "width": 420 },
      "version": { "type": "InputText", "hint": "version (latest)", "width": 200 }
    } },
    "release_row": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "release": { "type": "InputText", "hint": "release name", "width": 420 },
      "ns":      { "type": "InputText", "hint": "namespace (default)", "width": 200 }
    } },
    "opts": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "create_ns": { "type": "Checkbox", "label": "Create namespace", "value": true },
      "wait":      { "type": "Checkbox", "label": "Wait until ready", "value": false }
    } },
    "values_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":      { "type": "Label", "text": "Values (YAML, optional)" },
      "spring":       { "type": "Spring" },
      "btn_defaults": { "type": "Button", "label": "Load chart defaults", "icon": "res/icons/document.png" }
    } },
    "values": {
      "type": "InputText", "multiline": true, "flags": "AllowTabInput",
      "max_length": 262144, "width": -1, "height": 300
    },
    "status": { "type": "Label", "text": "" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "spring":     { "type": "Spring" },
      "btn_submit": { "type": "Button", "label": "Install" },
      "btn_cancel": { "type": "Button", "label": "Cancel" }
    } }
  } } }
})json";

// Details `kind` -> TextEditor highlighting language. Values and manifests
// are YAML and `helm status` is `KEY: value` lines; notes are free text.
const char* details_language(const std::string& kind) {
  if (kind == "chart_readme")
    return "markdown";
  if (kind == "notes")
    return "none";
  return "yaml";
}

} // namespace

// ── helm_frontend ──────────────────────────────────────────────────────────

helm_frontend::helm_frontend(dynamic&& base) : tool_form(std::move(base)) {}

void helm_frontend::on_init() {
  internal_root_key_ = next_available_key("__helm_");

  auto refresh_button = [this](ui_tree& tree) {
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { emit("refresh_requested"_key); });
    });
  };

  // Releases is the main root -- form::init() registers internal_root_key_
  // as this form's top-level object automatically; the other windows
  // register themselves (tool_form::build_window()).
  build_list_window(releases_, kReleasesLayout, internal_root_key_, refresh_button);

  build_list_window(repos_, kReposLayout, internal_root_key_ + "_repos", [&](ui_tree& tree) {
    refresh_button(tree);
    tree.with("vbox.toolbar.btn_update_all", [&](const auto& e) {
      on_click(wish_id_of(e), [this] {
        emit("repo_action_requested"_key, make_payload("name"_key, std::string{}, "action"_key, std::string{"update"}));
      });
    });
    tree.with("vbox.add_bar.repo_name", [&](const auto& e) {
      repo_name_input_ = e;
      repo_name_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.add_bar.repo_url", [&](const auto& e) {
      repo_url_input_ = e;
      repo_url_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.add_bar.btn_add", [&](const auto& e) {
      on_click(wish_id_of(e), [this] {
        if (repo_name_text_.empty() || repo_url_text_.empty()) {
          repos_.panel.set_status("Enter a repository name and URL to add", false);
          return;
        }
        emit("repo_add_requested"_key, make_payload("name"_key, repo_name_text_, "url"_key, repo_url_text_));
        repo_name_text_.clear();
        repo_url_text_.clear();
        if (repo_name_input_)
          repo_name_input_["value"_key] = std::string{};
        if (repo_url_input_)
          repo_url_input_["value"_key] = std::string{};
      });
    });
  });

  build_list_window(charts_, kChartsLayout, internal_root_key_ + "_charts", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.query", [&](const auto& e) { chart_query_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_install", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { open_install_dialog(false, {}, {}, {}, {}); });
    });
    tree.with("vbox.toolbar.btn_search", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { emit("search_requested"_key, make_payload("query"_key, chart_query_text_)); });
    });
  });

  build_list_window(history_, kHistoryLayout, internal_root_key_ + "_history", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { history_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { emit_history_request(); });
    });
  });

  // open_details() picks the highlighting language per kind.
  details_.build(*this, internal_root_key_ + "_details", {.title = "Details", .language = "yaml", .on_refresh = [this] {
                                                            emit_details_request();
                                                          }});

  install_root_key_ = internal_root_key_ + "_install";

  console_.build(
      *this, internal_root_key_ + "_console", {.table_id = "##helm_console_table", .width = 960, .command_width = 380});

  // Seed the first-run arrangement (mirrors kubectl / docker): a wide left
  // column of tabbed list windows over a Console strip, and a narrower right
  // column with Details over History. History gets its own area rather than
  // a tab behind Details so the row menu's "History" visibly does something.
  // Owned by imgui.ini after the first run; bump the version arg to layout()
  // if this arrangement changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "helm_dock", "Helm",
        layout(
            split(
                dir::left, 0.6f,
                split(
                    dir::down,
                    0.24f,
                    area({console_.root_key()}),
                    area({internal_root_key_, repos_.panel.root_key(), charts_.panel.root_key()}, internal_root_key_)),
                split(dir::down, 0.4f, area({history_.panel.root_key()}), area({details_.root_key()}))),
            /*version=*/1,
            /*target=*/"helm_dock")));
  }

  // Initial population is triggered client-side (run_helm() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event, which would race ahead of that wiring.
}

void helm_frontend::build_list_window(
    list_window& lw, const char* layout_json, const std::string& root_key,
    const std::function<void(ui_tree&)>& wire) {
  lw.panel.build(*this, root_key, layout_json, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) { lw.name_filter_id = wish_id_of(e); });
    tree.with("vbox.toolbar.ns", [&](const auto& e) { lw.ns_filter_id = wish_id_of(e); });
    tree.with("vbox.toolbar.state", [&](const auto& e) { lw.status_combo_id = wish_id_of(e); });
    wire(tree);
  });
}

void helm_frontend::open_details(
    const std::string& kind, const std::string& name, const std::string& ns, const std::string& version) {
  open_details_kind_ = kind;
  open_details_name_ = name;
  open_details_ns_ = ns;
  open_details_version_ = version;
  details_.set_title(kind + ": " + (ns.empty() ? name : ns + "/" + name));
  if (details_.editor())
    details_.editor()["language"_key] = std::string{details_language(kind)};
  emit_details_request();
}

void helm_frontend::emit_details_request() {
  if (open_details_name_.empty())
    return;
  emit(
      "details_requested"_key,
      make_payload(
          "kind"_key,
          open_details_kind_,
          "name"_key,
          open_details_name_,
          "namespace"_key,
          open_details_ns_,
          "version"_key,
          open_details_version_));
}

void helm_frontend::emit_history_request() {
  if (open_history_name_.empty())
    return;
  emit("history_requested"_key, make_payload("name"_key, open_history_name_, "namespace"_key, open_history_ns_));
}

common::menu_item
helm_frontend::action_item(const std::string& label, const entry& e, const std::string& action, bool confirm) {
  return {label, [this, e, action] { run_row_action(e, action); }, confirm};
}

void helm_frontend::apply_list_filter(list_window& lw) {
  const std::string name_needle = common::lower(lw.name_filter);
  const std::string ns_needle = common::lower(lw.ns_filter);

  lw.panel.apply_filter([&](const entry& e) {
    // "Pending" covers pending-install / pending-upgrade / pending-rollback.
    if (lw.status_filter == 1 && e.state != "deployed")
      return false;
    if (lw.status_filter == 2 && e.state != "failed")
      return false;
    if (lw.status_filter == 3 && !starts_with(e.state, "pending"))
      return false;
    if (!ns_needle.empty() && common::lower(e.ns).find(ns_needle) == std::string::npos)
      return false;
    return name_needle.empty() || common::lower(e.name).find(name_needle) != std::string::npos;
  });
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void helm_frontend::rebuild_releases(const dynamic& args) {
  releases_.panel.clear();

  size_t deployed = 0, total = 0;
  for_each_entry(args, "releases"_key, [&](const dynamic& e) {
    entry r{"release", str_of(e, "name"_key), str_of(e, "namespace"_key), str_of(e, "status"_key), {}};

    std::vector<ui_element_ptr> cells = {
        make_label(r.ns, kIdle),
        make_label(r.name),
        make_label(str_of(e, "revision"_key)),
        make_label(r.state, release_status_colour(r.state)),
        make_label(str_of(e, "chart"_key)),
        make_label(str_of(e, "app_version"_key), kIdle),
        make_label(str_of(e, "updated"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Status", r, "status", false),
        action_item("Values", r, "values", false),
        action_item("Manifest", r, "manifest", false),
        action_item("Notes", r, "notes", false),
        action_item("History", r, "history", false),
        {},
        action_item("Upgrade", r, "upgrade", true),
        action_item("Rollback", r, "rollback", true),
        action_item("Uninstall", r, "uninstall", true)};
    if (r.state == "deployed")
      ++deployed;
    ++total;
    releases_.panel.add(std::move(r), cells, items);
  });

  releases_.panel.refresh();
  apply_list_filter(releases_);
  releases_.panel.set_status(
      total == 0 ? std::string{"No releases. Install a chart from the Charts window."}
                 : plural(total, "release") + " (" + std::to_string(deployed) + " deployed)",
      true);
}

void helm_frontend::rebuild_repos(const dynamic& args) {
  repos_.panel.clear();

  size_t total = 0;
  for_each_entry(args, "repos"_key, [&](const dynamic& e) {
    entry r{"repo", str_of(e, "name"_key), {}, {}, {}};
    std::vector<ui_element_ptr> cells = {
        make_label(r.name),
        make_label(str_of(e, "url"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Update", r, "update", false), {}, action_item("Remove", r, "remove", true)};
    repos_.panel.add(std::move(r), cells, items);
    ++total;
  });

  repos_.panel.refresh();
  apply_list_filter(repos_);
  repos_.panel.set_status(
      total == 0       ? std::string{"No repositories. Add one with the name and URL boxes above."}
          : total == 1 ? std::string{"1 repository"}
                       : std::to_string(total) + " repositories",
      true);
}

void helm_frontend::rebuild_charts(const dynamic& args) {
  charts_.panel.clear();

  size_t total = 0;
  for_each_entry(args, "charts"_key, [&](const dynamic& e) {
    if (total++ >= kMaxChartRows)
      return;
    entry c{"chart", str_of(e, "name"_key), {}, {}, str_of(e, "version"_key)};
    std::vector<ui_element_ptr> cells = {
        make_label(c.name),
        make_label(c.extra),
        make_label(str_of(e, "app_version"_key), kIdle),
        make_label(str_of(e, "description"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Values", c, "chart_values", false),
        action_item("Readme", c, "chart_readme", false),
        {},
        action_item("Install", c, "install", true)};
    charts_.panel.add(std::move(c), cells, items);
  });

  charts_.panel.refresh();
  const std::string query = str_of(args, "query"_key);
  std::string status = plural(total, "chart") + (query.empty() ? std::string{} : " matching '" + query + "'");
  if (total == 0 && query.empty())
    status = "No charts. Add a repository in the Repositories window, then Search.";
  else if (total > kMaxChartRows)
    status += " (showing the first " + std::to_string(kMaxChartRows) + "; refine the search)";
  charts_.panel.set_status(status, true);
}

void helm_frontend::rebuild_history(const dynamic& args) {
  history_.panel.clear();

  size_t total = 0;
  for_each_entry(args, "revisions"_key, [&](const dynamic& e) {
    entry rev{"revision", open_history_name_, open_history_ns_, {}, str_of(e, "revision"_key)};
    const std::string status = str_of(e, "status"_key);
    std::vector<ui_element_ptr> cells = {
        make_label(rev.extra),
        make_label(str_of(e, "updated"_key), kIdle),
        make_label(status, release_status_colour(status)),
        make_label(str_of(e, "chart"_key)),
        make_label(str_of(e, "description"_key)),
    };
    std::vector<common::menu_item> items = {action_item("Rollback to this revision", rev, "rollback", true)};
    history_.panel.add(std::move(rev), cells, items);
    ++total;
  });

  history_.panel.refresh();
  history_.panel.set_status(plural(total, "revision"), true);
}

// ── RMI methods ────────────────────────────────────────────────────────────

dynamic helm_frontend::do_update_releases(const dynamic& args) {
  rebuild_releases(args);
  return dynamic{};
}

dynamic helm_frontend::do_update_repos(const dynamic& args) {
  rebuild_repos(args);
  return dynamic{};
}

dynamic helm_frontend::do_update_charts(const dynamic& args) {
  rebuild_charts(args);
  return dynamic{};
}

dynamic helm_frontend::do_update_history(const dynamic& args) {
  if (str_of(args, "name"_key) != open_history_name_ || str_of(args, "namespace"_key) != open_history_ns_)
    return dynamic{}; // stale response for a release the user navigated away from.
  rebuild_history(args);
  return dynamic{};
}

dynamic helm_frontend::do_update_details(const dynamic& args) {
  if (str_of(args, "kind"_key) != open_details_kind_ || str_of(args, "name"_key) != open_details_name_ ||
      str_of(args, "namespace"_key) != open_details_ns_)
    return dynamic{};
  details_.set_title(str_of(args, "title"_key));
  details_.set_text(str_of(args, "text"_key));
  return dynamic{};
}

dynamic helm_frontend::do_command_result(const dynamic& args) {
  bool ok = false;
  const std::string text = common::command_result_text(args, ok);
  const std::string scope = str_of(args, "scope"_key);
  if (scope == "install") {
    install_.busy = false;
    if (install_.open && !ok) {
      // Keep the dialog (and the values typed into it) for a retry.
      set_install_status(text, false);
      return dynamic{};
    }
    if (ok)
      close_install_dialog();
  }
  list_window& lw = scope == "repos" ? repos_ : scope == "charts" ? charts_ : scope == "history" ? history_ : releases_;
  lw.panel.set_status(text, ok);
  return dynamic{};
}

dynamic helm_frontend::do_append_command_log(const dynamic& args) {
  console_.append_from(args);
  return dynamic{};
}

// ── Install / Upgrade dialog ───────────────────────────────────────────────

void helm_frontend::open_install_dialog(
    bool upgrade, const std::string& chart, const std::string& version, const std::string& release,
    const std::string& ns) {
  close_install_dialog();

  install_ = install_dialog{};
  install_.upgrade = upgrade;
  install_.chart = chart;
  install_.version = version;
  install_.release = release;
  install_.ns = ns;
  ++install_token_;

  constexpr int32_t kReadOnly = 1 << 9; // InputText "flags": ReadOnly
  auto build = [&] {
    build_window(install_root_key_, kInstallLayout, install_.window_id, [&](ui_tree& tree) {
      for (auto& [key, elem] : tree)
        install_.object_ids.push_back(wish_id_of(elem));

      auto input = [&](const char* path, key_t& id_out, const std::string& value, bool read_only) {
        tree.with(path, [&](const auto& e) {
          id_out = wish_id_of(e);
          e["value"_key] = value;
          if (read_only)
            e["flags"_key] = kReadOnly;
        });
      };
      input("vbox.chart_row.chart", install_.chart_id, chart, false);
      input("vbox.chart_row.version", install_.version_id, version, false);
      // An upgrade targets an existing release: its name / namespace are fixed.
      input("vbox.release_row.release", install_.release_id, release, upgrade);
      input("vbox.release_row.ns", install_.ns_id, ns, upgrade);

      tree.with("vbox.opts.create_ns", [&](const auto& e) {
        install_.create_ns_id = wish_id_of(e);
        e["visible"_key] = !upgrade;
      });
      tree.with("vbox.opts.wait", [&](const auto& e) { install_.wait_id = wish_id_of(e); });
      tree.with("vbox.values", [&](const auto& e) {
        install_.values_input = e;
        install_.values_id = wish_id_of(e);
      });
      tree.with("vbox.status", [&](const auto& e) { install_.status_label = e; });
      tree.with("vbox.values_bar.btn_defaults", [&](const auto& e) {
        on_click(wish_id_of(e), [this] { request_install_values("chart"); });
      });
      tree.with("vbox.buttons.btn_submit", [&](const auto& e) {
        e["label"_key] = std::string{upgrade ? "Upgrade" : "Install"};
        on_click(wish_id_of(e), [this] { submit_install(); });
      });
      tree.with("vbox.buttons.btn_cancel", [&](const auto& e) {
        on_click(wish_id_of(e), [this] { close_install_dialog(); });
      });
      if (upgrade)
        (*tree[""])["title"_key] = "Upgrade release " + ns + "/" + release;
    });
  };

  // Menu items / buttons reach here from on_event(), outside RMI dispatch,
  // while build_window() needs sess().
  run_in_dispatch(build);
  install_.open = true;
}

void helm_frontend::close_install_dialog() {
  if (!install_.open)
    return;
  erase_objects(install_.object_ids);
  remove_objects_at(install_root_key_);
  install_ = install_dialog{};
}

void helm_frontend::set_install_status(const std::string& text, bool ok) {
  common::set_status_text(install_.status_label, text, ok);
}

void helm_frontend::submit_install() {
  if (install_.busy)
    return;
  if (install_.chart.empty() || install_.release.empty()) {
    set_install_status(
        install_.chart.empty() ? "Enter the chart to install (for example repo/name)" : "Enter a release name", false);
    return;
  }
  // helm accepts dots in a release name, but charts put it into Service /
  // Pod names, which Kubernetes then rejects -- after helm has recorded a
  // failed release. Hold new releases to a DNS label up front (mirrored by
  // the client's is_valid_release_name()).
  auto label_char = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'; };
  const std::string& name = install_.release;
  if (!install_.upgrade &&
      (name.size() > 53 || name.front() == '-' || name.back() == '-' ||
       !std::all_of(name.begin(), name.end(), label_char))) {
    set_install_status(
        "Release names may only contain lowercase letters, digits and '-' (for example my-app)", false);
    return;
  }
  install_.busy = true;
  const char* verb = install_.upgrade ? "upgrade" : "install";
  set_install_status(std::string{"Running helm "} + verb + " ...", true);

  dynamic p;
  p["mode"_key] = std::string{verb};
  p["chart"_key] = install_.chart;
  p["version"_key] = install_.version;
  p["release"_key] = install_.release;
  p["namespace"_key] = install_.ns.empty() ? std::string{"default"} : install_.ns;
  p["values"_key] = install_.values;
  p["create_namespace"_key] = !install_.upgrade && install_.create_ns;
  p["wait"_key] = install_.wait;
  emit("install_requested"_key, std::move(p));
}

void helm_frontend::request_install_values(const std::string& source) {
  if (source == "chart" && install_.chart.empty()) {
    set_install_status("Enter the chart first", false);
    return;
  }
  dynamic p;
  p["token"_key] = install_token_;
  p["source"_key] = source;
  p["chart"_key] = install_.chart;
  p["version"_key] = install_.version;
  p["name"_key] = install_.release;
  p["namespace"_key] = install_.ns;
  emit("install_values_requested"_key, std::move(p));
}

dynamic helm_frontend::do_set_install_values(const dynamic& args) {
  if (!install_.open || args.as<int32_t>("token"_key) != install_token_)
    return dynamic{}; // the dialog this was for is gone.
  const std::string error = str_of(args, "error"_key);
  if (!error.empty()) {
    set_install_status(error, false);
    return dynamic{};
  }
  install_.values = str_of(args, "text"_key);
  if (install_.values_input)
    install_.values_input["value"_key] = install_.values;
  set_install_status("", true);
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void helm_frontend::run_row_action(const entry& e, const std::string& action) {
  const std::string qualified = e.ns.empty() ? e.name : e.ns + "/" + e.name;

  if (e.scope == "release") {
    if (action == "history") {
      open_history_name_ = e.name;
      open_history_ns_ = e.ns;
      if (history_target_label_)
        history_target_label_["text"_key] = "history: " + qualified;
      emit_history_request();
      return;
    }
    if (action == "upgrade") {
      // The Chart column is "<name>-<version>", not an installable
      // reference, so the chart box starts empty; the Values box is filled
      // with what the release currently uses.
      open_install_dialog(true, {}, {}, e.name, e.ns);
      request_install_values("release");
      return;
    }
    if (action != "rollback" && action != "uninstall") {
      open_details(action, e.name, e.ns, {});
      return;
    }
  }

  if (e.scope == "release" || e.scope == "revision") {
    // "revision" rows only offer a rollback to their own revision.
    const bool uninstall = action == "uninstall";
    const std::string revision = e.scope == "revision" ? e.extra : std::string{};
    auto fire = [this, e, action, revision] {
      dynamic p;
      p["name"_key] = e.name;
      p["namespace"_key] = e.ns;
      p["action"_key] = action;
      p["revision"_key] = revision;
      emit("release_action_requested"_key, std::move(p));
    };
    const std::string where = "'" + e.name + "' in namespace '" + e.ns + "'";
    show_confirm(
        uninstall ? "Uninstall release " + where + "? Its resources will be deleted."
                  : "Roll back release " + where + " to " +
                        (revision.empty() ? std::string{"the previous revision"} : "revision " + revision) + "?",
        fire);
    return;
  }

  if (e.scope == "repo") {
    auto fire = [this, e, action] {
      dynamic p;
      p["name"_key] = e.name;
      p["action"_key] = action;
      emit("repo_action_requested"_key, std::move(p));
    };
    if (action == "remove")
      show_confirm("Remove repository '" + e.name + "'?", fire);
    else
      fire();
    return;
  }

  // Charts.
  if (action == "install")
    open_install_dialog(false, e.name, e.extra, {}, {});
  else
    open_details(action, e.name, {}, e.extra);
}

void helm_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  if (install_.open && event == "closed"_key && id == install_.window_id) {
    close_install_dialog();
    return;
  }

  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == releases_.panel.window_id() || id == repos_.panel.window_id() || id == charts_.panel.window_id() ||
       id == history_.panel.window_id() || id == details_.window_id() || id == console_.window_id())) {
    details_.remove_file();
    close_install_dialog();
    emit("closed"_key);
    remove_objects_at(repos_.panel.root_key());
    remove_objects_at(charts_.panel.root_key());
    remove_objects_at(history_.panel.root_key());
    remove_objects_at(details_.root_key());
    remove_objects_at(console_.root_key());
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    for (auto* lw : {&releases_, &repos_}) {
      if (lw->name_filter_id.id && id == lw->name_filter_id)
        lw->name_filter = payload.as<std::string>("value"_key);
      else if (lw->ns_filter_id.id && id == lw->ns_filter_id)
        lw->ns_filter = payload.as<std::string>("value"_key);
      else if (lw->status_combo_id.id && id == lw->status_combo_id)
        lw->status_filter = payload.as<int32_t>("value"_key);
      else
        continue;
      apply_list_filter(*lw);
      return;
    }
    // Inline toolbar fields: remember the text for the button / menu item
    // that consumes it.
    for (auto [field_id, text] :
         {std::pair{repo_name_input_id_, &repo_name_text_}, std::pair{repo_url_input_id_, &repo_url_text_},
          std::pair{chart_query_id_, &chart_query_text_}}) {
      if (id == field_id) {
        *text = payload.as<std::string>("value"_key);
        return;
      }
    }
    if (!install_.open)
      return;
    for (auto [field_id, text] :
         {std::pair{install_.chart_id, &install_.chart}, std::pair{install_.version_id, &install_.version},
          std::pair{install_.release_id, &install_.release}, std::pair{install_.ns_id, &install_.ns},
          std::pair{install_.values_id, &install_.values}}) {
      if (id == field_id) {
        *text = payload.as<std::string>("value"_key);
        return;
      }
    }
    if (id == install_.create_ns_id)
      install_.create_ns = payload.as<bool>("value"_key);
    else if (id == install_.wait_id)
      install_.wait = payload.as<bool>("value"_key);
    return;
  }

  if (event == "clicked"_key)
    dispatch_click(id);
}

// ── Registration ───────────────────────────────────────────────────────────

void register_helm() {
  auto proto = dynamic_ptr{"HelmFrontend"_key, {}};

  auto add_method = [&](key_t name, dynamic (helm_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<helm_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_releases"_key, &helm_frontend::do_update_releases);
  add_method("update_repos"_key, &helm_frontend::do_update_repos);
  add_method("update_charts"_key, &helm_frontend::do_update_charts);
  add_method("update_history"_key, &helm_frontend::do_update_history);
  add_method("update_details"_key, &helm_frontend::do_update_details);
  add_method("command_result"_key, &helm_frontend::do_command_result);
  add_method("append_command_log"_key, &helm_frontend::do_append_command_log);
  add_method("set_install_values"_key, &helm_frontend::do_set_install_values);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("HelmFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "GUI frontend for the local `helm` CLI (releases, chart repositories, chart search). All `helm` "
      "invocation happens client-side; this form only renders whatever snapshot it was last given. Listen "
      "for the 'closed' event to detect when the user is done, and the '*_requested' events to react to user "
      "actions -- see helm.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<helm_frontend>("wish"_key, "HelmFrontend"_key));
}

} // namespace bdg::wish
