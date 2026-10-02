// MIT License © 2026 Binary Dice Games
/// @file helm.cpp
/// @brief Implementation of the HelmFrontend form.
///
/// A close port of modules/bdg/dev/kubectl/server/kubectl.cpp: inline JSON
/// window layouts + import_json(), C++-built table rows, a per-row `...`
/// MenuButton, show_confirm() via a privately-instantiated MessageBox, and
/// an id -> handler dispatch map rebuilt on every update_*. The four list
/// windows (Releases / Repositories / Charts / History) share one
/// build_list_window() / clear_list_rows() / add_list_row() path.
#include "helm.hpp"

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

// "#RRGGBBAA" light/dark pairs -- GitHub Primer tokens, the kubectl.cpp /
// docker.cpp pattern. A single text_color tuned for one theme reads poorly
// on the other.
constexpr const char* kOkLight = "#1A7F37FF";
constexpr const char* kOkDark = "#3FB950FF";
constexpr const char* kIdleLight = "#656D76FF";
constexpr const char* kIdleDark = "#8B949EFF";
constexpr const char* kWarnLight = "#9A6700FF";
constexpr const char* kWarnDark = "#D29922FF";
constexpr const char* kBadLight = "#CF222EFF";
constexpr const char* kBadDark = "#F85149FF";

bool starts_with(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

// Release status ("deployed", "failed", "pending-install", "uninstalling",
// "superseded", "uninstalled", "unknown") -> (light, dark) colour.
std::pair<const char*, const char*> release_status_colour(const std::string& status) {
  if (status == "deployed")
    return {kOkLight, kOkDark};
  if (status == "failed")
    return {kBadLight, kBadDark};
  if (starts_with(status, "pending") || status == "uninstalling")
    return {kWarnLight, kWarnDark};
  return {kIdleLight, kIdleDark};
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
      "btn_refresh": { "type": "Button", "label": "Refresh" },
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
      "btn_refresh":    { "type": "Button", "label": "Refresh" },
      "btn_update_all": { "type": "Button", "label": "Update all" },
      "filter":         { "type": "InputText", "hint": "Filter by name", "width": 200 }
    } },
    "add_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "repo_name": { "type": "InputText", "hint": "new repository name", "width": 200 },
      "repo_url":  { "type": "InputText", "hint": "https://charts.example.com", "width": 320 },
      "btn_add":   { "type": "Button", "label": "Add" }
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
      "btn_search": { "type": "Button", "label": "Search" },
      "spring":      { "type": "Spring" },
      "btn_install": { "type": "Button", "label": "Install chart..." }
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
      "btn_refresh": { "type": "Button", "label": "Refresh" }
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
      "btn_defaults": { "type": "Button", "label": "Load chart defaults" }
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

// Details is a toolbar + a read-only TextEditor. A TextEditor displays a
// file, so set_details_text() writes each update into the session sandbox;
// open_details() picks the highlighting language per kind.

static constexpr const char* kDetailsLayout = R"json({
  "type": "Window", "title": "Details", "width": 820, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "(nothing selected)" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "Refresh" }
    } },
    "sep": { "type": "Separator" },
    "editor": {
      "type": "TextEditor", "file_path": "", "language": "yaml", "read_only": true,
      "width": -1, "height": -1
    }
  } } }
})json";

// The Console window: a FIFO-capped `Table` tracing every `helm` command the
// client ran. "auto_scroll": true so it follows the newest row.

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 960, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##helm_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed", "init_width": 44,  "column_id": 0 },
        "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed", "init_width": 380, "column_id": 1 },
        "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed", "init_width": 50,  "column_id": 2 },
        "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
      }
    }
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

std::string plural(size_t n, const char* noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

} // namespace

// ── helm_frontend ──────────────────────────────────────────────────────────

helm_frontend::helm_frontend(dynamic&& base) : form(std::move(base)) {}

void helm_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void helm_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

ui_element_ptr helm_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void helm_frontend::on_init() {
  internal_root_key_ = next_available_key("__helm_");

  auto refresh_button = [this](ui_tree& tree) {
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit("refresh_requested"_key); };
    });
  };

  // Releases is the main root -- form::init() registers internal_root_key_
  // as this form's top-level object automatically. The other windows are
  // registered by hand inside build_list_window() / build_text_window().
  build_list_window(releases_, kReleasesLayout, internal_root_key_, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) { releases_.name_filter_id = wish_id_of(e); });
    tree.with("vbox.toolbar.ns", [&](const auto& e) { releases_.ns_filter_id = wish_id_of(e); });
    tree.with("vbox.toolbar.state", [&](const auto& e) { releases_.status_combo_id = wish_id_of(e); });
    refresh_button(tree);
  });

  build_list_window(repos_, kReposLayout, internal_root_key_ + "_repos", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.filter", [&](const auto& e) { repos_.name_filter_id = wish_id_of(e); });
    refresh_button(tree);
    tree.with("vbox.toolbar.btn_update_all", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        dynamic p;
        p["name"_key] = std::string{};
        p["action"_key] = std::string{"update"};
        emit("repo_action_requested"_key, std::move(p));
      };
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
      click_handlers_[wish_id_of(e)] = [this] {
        if (repo_name_text_.empty() || repo_url_text_.empty()) {
          set_status(repos_, "Enter a repository name and URL to add", false);
          return;
        }
        dynamic p;
        p["name"_key] = repo_name_text_;
        p["url"_key] = repo_url_text_;
        emit("repo_add_requested"_key, std::move(p));
        repo_name_text_.clear();
        repo_url_text_.clear();
        if (repo_name_input_)
          repo_name_input_["value"_key] = std::string{};
        if (repo_url_input_)
          repo_url_input_["value"_key] = std::string{};
      };
    });
  });

  build_list_window(charts_, kChartsLayout, internal_root_key_ + "_charts", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.query", [&](const auto& e) { chart_query_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.btn_install", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { open_install_dialog(false, {}, {}, {}, {}); };
    });
    tree.with("vbox.toolbar.btn_search", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] {
        dynamic p;
        p["query"_key] = chart_query_text_;
        emit("search_requested"_key, std::move(p));
      };
    });
  });

  build_list_window(history_, kHistoryLayout, internal_root_key_ + "_history", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { history_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_history_request(); };
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

  install_root_key_ = internal_root_key_ + "_install";

  console_root_key_ = internal_root_key_ + "_console";
  build_text_window(console_root_key_, kConsoleLayout, console_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.table", [&](const auto& e) { console_table_ = e; });
  });

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
                    dir::down, 0.24f,
                    area({console_root_key_}),
                    area({internal_root_key_, repos_.root_key, charts_.root_key}, internal_root_key_)),
                split(dir::down, 0.4f, area({history_.root_key}), area({details_root_key_}))),
            /*version=*/1, /*target=*/"helm_dock")));
  }

  // Initial population is triggered client-side (run_helm() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event, which would race ahead of that wiring.
}

void helm_frontend::build_text_window(
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

void helm_frontend::build_list_window(
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

void helm_frontend::set_details_text(const std::string& text) {
  if (!details_editor_)
    return;
  // A fresh name every call: the TextEditor renderer only reloads when
  // file_path changes, so rewriting one fixed file would leave the pane
  // showing stale content. Under "private/" because release values and
  // manifests can carry secrets -- see context.hpp's resource_dir doc
  // comment.
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

void helm_frontend::remove_details_file() {
  if (details_file_.empty())
    return;
  std::error_code ec;
  std::filesystem::remove(resource_dir_ / details_file_, ec);
  details_file_.clear();
}

void helm_frontend::open_details(
    const std::string& kind, const std::string& name, const std::string& ns, const std::string& version) {
  open_details_kind_ = kind;
  open_details_name_ = name;
  open_details_ns_ = ns;
  open_details_version_ = version;
  if (details_target_label_)
    details_target_label_["text"_key] = kind + ": " + (ns.empty() ? name : ns + "/" + name);
  if (details_editor_)
    details_editor_["language"_key] = std::string{details_language(kind)};
  emit_details_request();
}

void helm_frontend::emit_details_request() {
  if (open_details_name_.empty())
    return;
  dynamic p;
  p["kind"_key] = open_details_kind_;
  p["name"_key] = open_details_name_;
  p["namespace"_key] = open_details_ns_;
  p["version"_key] = open_details_version_;
  emit("details_requested"_key, std::move(p));
}

void helm_frontend::emit_history_request() {
  if (open_history_name_.empty())
    return;
  dynamic p;
  p["name"_key] = open_history_name_;
  p["namespace"_key] = open_history_ns_;
  emit("history_requested"_key, std::move(p));
}

// ── Confirmation modal (kubectl_frontend::show_confirm() port) ─────────────

void helm_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
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

void helm_frontend::clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key) {
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

void helm_frontend::add_list_row(
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
    menu_action_targets_[wish_id_of(mi)] = row_action{meta.scope, meta.name, meta.ns, meta.extra, it.action};
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

void helm_frontend::set_status(list_window& lw, const std::string& text, bool ok) {
  if (!lw.status_label)
    return;
  lw.status_label["text"_key] = text;
  lw.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  lw.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

void helm_frontend::apply_list_filter(list_window& lw, std::vector<list_row>& rows) {
  auto lc = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
  };
  const std::string name_needle = lc(lw.name_filter);
  const std::string ns_needle = lc(lw.ns_filter);

  for (auto& r : rows) {
    if (!r.row)
      continue;
    bool show = true;
    // "Pending" covers pending-install / pending-upgrade / pending-rollback.
    if (lw.status_filter == 1)
      show = r.state == "deployed";
    else if (lw.status_filter == 2)
      show = r.state == "failed";
    else if (lw.status_filter == 3)
      show = starts_with(r.state, "pending");
    if (show && !ns_needle.empty())
      show = lc(r.ns).find(ns_needle) != std::string::npos;
    if (show && !name_needle.empty())
      show = lc(r.name).find(name_needle) != std::string::npos;
    r.row["visible"_key] = show;
  }
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void helm_frontend::rebuild_releases(const dynamic& args) {
  clear_list_rows(releases_, release_rows_, next_release_key_);

  size_t deployed = 0, total = 0;
  for_each_entry(args, "releases"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "release";
    meta.name = str_of(e, "name"_key);
    meta.ns = str_of(e, "namespace"_key);
    meta.state = str_of(e, "status"_key);
    auto [cl, cd] = release_status_colour(meta.state);

    std::vector<ui_element_ptr> cells = {
        make_label(meta.ns, kIdleLight, kIdleDark),
        make_label(meta.name),
        make_label(str_of(e, "revision"_key)),
        make_label(meta.state, cl, cd),
        make_label(str_of(e, "chart"_key)),
        make_label(str_of(e, "app_version"_key), kIdleLight, kIdleDark),
        make_label(str_of(e, "updated"_key), kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {
        {"Status", "status", false},   {"Values", "values", false},   {"Manifest", "manifest", false},
        {"Notes", "notes", false},     {"History", "history", false}, {},
        {"Upgrade", "upgrade", true},  {"Rollback", "rollback", true}, {"Uninstall", "uninstall", true}};
    if (meta.state == "deployed")
      ++deployed;
    ++total;
    add_list_row(releases_, release_rows_, next_release_key_, std::move(meta), cells, items);
  });

  if (releases_.table)
    releases_.table->refresh_children_order();
  apply_list_filter(releases_, release_rows_);
  set_status(
      releases_,
      total == 0 ? std::string{"No releases. Install a chart from the Charts window."}
                 : plural(total, "release") + " (" + std::to_string(deployed) + " deployed)",
      true);
}

void helm_frontend::rebuild_repos(const dynamic& args) {
  clear_list_rows(repos_, repo_rows_, next_repo_key_);

  size_t total = 0;
  for_each_entry(args, "repos"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "repo";
    meta.name = str_of(e, "name"_key);

    std::vector<ui_element_ptr> cells = {
        make_label(meta.name),
        make_label(str_of(e, "url"_key), kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {{"Update", "update", false}, {}, {"Remove", "remove", true}};
    add_list_row(repos_, repo_rows_, next_repo_key_, std::move(meta), cells, items);
    ++total;
  });

  if (repos_.table)
    repos_.table->refresh_children_order();
  apply_list_filter(repos_, repo_rows_);
  set_status(
      repos_,
      total == 0   ? std::string{"No repositories. Add one with the name and URL boxes above."}
      : total == 1 ? std::string{"1 repository"}
                   : std::to_string(total) + " repositories",
      true);
}

void helm_frontend::rebuild_charts(const dynamic& args) {
  clear_list_rows(charts_, chart_rows_, next_chart_key_);

  size_t total = 0;
  for_each_entry(args, "charts"_key, [&](const dynamic& e) {
    if (total++ >= kMaxChartRows)
      return;
    list_row meta;
    meta.scope = "chart";
    meta.name = str_of(e, "name"_key);
    meta.extra = str_of(e, "version"_key);

    std::vector<ui_element_ptr> cells = {
        make_label(meta.name),
        make_label(meta.extra),
        make_label(str_of(e, "app_version"_key), kIdleLight, kIdleDark),
        make_label(str_of(e, "description"_key), kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {
        {"Values", "chart_values", false}, {"Readme", "chart_readme", false}, {}, {"Install", "install", true}};
    add_list_row(charts_, chart_rows_, next_chart_key_, std::move(meta), cells, items);
  });

  if (charts_.table)
    charts_.table->refresh_children_order();
  const std::string query = str_of(args, "query"_key);
  std::string status = plural(total, "chart") + (query.empty() ? std::string{} : " matching '" + query + "'");
  if (total == 0 && query.empty())
    status = "No charts. Add a repository in the Repositories window, then Search.";
  else if (total > kMaxChartRows)
    status += " (showing the first " + std::to_string(kMaxChartRows) + "; refine the search)";
  set_status(charts_, status, true);
}

void helm_frontend::rebuild_history(const dynamic& args) {
  clear_list_rows(history_, history_rows_, next_history_key_);

  size_t total = 0;
  for_each_entry(args, "revisions"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "revision";
    meta.name = open_history_name_;
    meta.ns = open_history_ns_;
    meta.extra = str_of(e, "revision"_key);
    const std::string status = str_of(e, "status"_key);
    auto [cl, cd] = release_status_colour(status);

    std::vector<ui_element_ptr> cells = {
        make_label(meta.extra),
        make_label(str_of(e, "updated"_key), kIdleLight, kIdleDark),
        make_label(status, cl, cd),
        make_label(str_of(e, "chart"_key)),
        make_label(str_of(e, "description"_key)),
    };
    std::vector<menu_spec> items = {{"Rollback to this revision", "rollback", true}};
    add_list_row(history_, history_rows_, next_history_key_, std::move(meta), cells, items);
    ++total;
  });

  if (history_.table)
    history_.table->refresh_children_order();
  set_status(history_, plural(total, "revision"), true);
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
  if (details_target_label_)
    details_target_label_["text"_key] = str_of(args, "title"_key);
  set_details_text(str_of(args, "text"_key));
  return dynamic{};
}

dynamic helm_frontend::do_command_result(const dynamic& args) {
  const std::string command = str_of(args, "command"_key);
  const bool ok = args.as<bool>("ok"_key);
  const std::string output = str_of(args, "output"_key);
  const std::string scope = str_of(args, "scope"_key);
  if (scope == "install") {
    install_.busy = false;
    if (install_.open && !ok) {
      // Keep the dialog (and the values typed into it) for a retry.
      set_install_status(command + " failed: " + (output.empty() ? "unknown error" : output), false);
      return dynamic{};
    }
    if (ok)
      close_install_dialog();
  }
  list_window* lw = scope == "repos" ? &repos_
      : scope == "charts"            ? &charts_
      : scope == "history"           ? &history_
                                     : &releases_;
  if (ok)
    set_status(*lw, command + ": OK", true);
  else
    set_status(*lw, command + " failed: " + (output.empty() ? "unknown error" : output), false);
  return dynamic{};
}

// ── Console window (client `helm` subprocess trace) ────────────────────────

void helm_frontend::append_console_row(
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

void helm_frontend::erase_console_row_objects(const console_row_entry& entry) {
  for (auto id : entry.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

void helm_frontend::clear_console_rows() {
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

dynamic helm_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      str_of(args, "command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      str_of(args, "output"_key));
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
    build_text_window(install_root_key_, kInstallLayout, install_.window_id, [&](ui_tree& tree) {
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
        click_handlers_[wish_id_of(e)] = [this] { request_install_values("chart"); };
      });
      tree.with("vbox.buttons.btn_submit", [&](const auto& e) {
        e["label"_key] = std::string{upgrade ? "Upgrade" : "Install"};
        click_handlers_[wish_id_of(e)] = [this] { submit_install(); };
      });
      tree.with("vbox.buttons.btn_cancel", [&](const auto& e) {
        click_handlers_[wish_id_of(e)] = [this] { close_install_dialog(); };
      });
      if (upgrade)
        (*tree[""])["title"_key] = "Upgrade release " + ns + "/" + release;
    });
  };

  // Menu items / buttons reach here from on_event(), outside RMI dispatch,
  // while build_text_window() needs sess(): install the session as the
  // dispatch context for the duration, the idiom instantiate_child_form()
  // uses for the same reason.
  if (detail::current_context) {
    build();
  } else {
    auto sess = context_wlock{*sync_ctx_};
    detail::current_context = &*sess;
    build();
    detail::current_context = nullptr;
  }
  install_.open = true;
}

void helm_frontend::close_install_dialog() {
  if (!install_.open)
    return;
  for (auto id : install_.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
  remove_objects_at(install_root_key_);
  install_ = install_dialog{};
}

void helm_frontend::set_install_status(const std::string& text, bool ok) {
  if (!install_.status_label)
    return;
  install_.status_label["text"_key] = text;
  install_.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  install_.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
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

void helm_frontend::run_row_action(const row_action& target) {
  const std::string qualified = target.ns.empty() ? target.name : target.ns + "/" + target.name;

  if (target.scope == "release") {
    if (target.action == "history") {
      open_history_name_ = target.name;
      open_history_ns_ = target.ns;
      if (history_target_label_)
        history_target_label_["text"_key] = "history: " + qualified;
      emit_history_request();
      return;
    }
    if (target.action == "upgrade") {
      // The Chart column is "<name>-<version>", not an installable
      // reference, so the chart box starts empty; the Values box is filled
      // with what the release currently uses.
      open_install_dialog(true, {}, {}, target.name, target.ns);
      request_install_values("release");
      return;
    }
    if (target.action != "rollback" && target.action != "uninstall") {
      open_details(target.action, target.name, target.ns, {});
      return;
    }
  }

  if (target.scope == "release" || target.scope == "revision") {
    // "revision" rows only offer a rollback to their own revision.
    const bool uninstall = target.action == "uninstall";
    const std::string revision = target.scope == "revision" ? target.extra : std::string{};
    auto fire = [this, target, revision] {
      dynamic p;
      p["name"_key] = target.name;
      p["namespace"_key] = target.ns;
      p["action"_key] = target.action;
      p["revision"_key] = revision;
      emit("release_action_requested"_key, std::move(p));
    };
    const std::string where = "'" + target.name + "' in namespace '" + target.ns + "'";
    show_confirm(
        uninstall ? "Uninstall release " + where + "? Its resources will be deleted."
                  : "Roll back release " + where + " to " +
                        (revision.empty() ? std::string{"the previous revision"} : "revision " + revision) + "?",
        fire);
    return;
  }

  if (target.scope == "repo") {
    auto fire = [this, target] {
      dynamic p;
      p["name"_key] = target.name;
      p["action"_key] = target.action;
      emit("repo_action_requested"_key, std::move(p));
    };
    if (target.action == "remove")
      show_confirm("Remove repository '" + target.name + "'?", fire);
    else
      fire();
    return;
  }

  // Charts.
  if (target.action == "install")
    open_install_dialog(false, target.name, target.extra, {}, {});
  else
    open_details(target.action, target.name, {}, target.extra);
}

void helm_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  if (install_.open && event == "closed"_key && id == install_.window_id) {
    close_install_dialog();
    return;
  }

  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == releases_.window_id || id == repos_.window_id || id == charts_.window_id ||
       id == history_.window_id || id == details_window_id_ || id == console_window_id_)) {
    remove_details_file();
    close_install_dialog();
    emit("closed"_key);
    remove_objects_at(repos_.root_key);
    remove_objects_at(charts_.root_key);
    remove_objects_at(history_.root_key);
    remove_objects_at(details_root_key_);
    remove_objects_at(console_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    for (auto [lw, rows] : {std::pair{&releases_, &release_rows_}, std::pair{&repos_, &repo_rows_}}) {
      if (id == lw->name_filter_id)
        lw->name_filter = payload.as<std::string>("value"_key);
      else if (lw->ns_filter_id.id && id == lw->ns_filter_id)
        lw->ns_filter = payload.as<std::string>("value"_key);
      else if (lw->status_combo_id.id && id == lw->status_combo_id)
        lw->status_filter = payload.as<int32_t>("value"_key);
      else
        continue;
      apply_list_filter(*lw, *rows);
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

  if (event != "clicked"_key)
    return;

  if (auto ch = click_handlers_.find(id); ch != click_handlers_.end()) {
    const auto handler = ch->second; // copy: a dialog's handler may erase itself.
    handler();
    return;
  }

  if (auto mi = menu_action_targets_.find(id); mi != menu_action_targets_.end()) {
    const row_action target = mi->second; // copy: the action may rebuild the table.
    run_row_action(target);
  }
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
