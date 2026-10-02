// MIT License © 2026 Binary Dice Games
/// @file pip.cpp
/// @brief Implementation of the PipFrontend form.
///
/// A close port of modules/bdg/dev/helm/server/helm.cpp: inline JSON window
/// layouts + import_json(), C++-built table rows, a per-row `...` MenuButton,
/// show_confirm() via a privately-instantiated MessageBox, and an id ->
/// handler dispatch map rebuilt on every update_*. The two list windows
/// (Packages / Versions) share one build_list_window() / clear_list_rows() /
/// add_list_row() path.
#include "pip.hpp"

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

// PEP 503 name normalization: lowercase, runs of `-` `_` `.` become one `-`,
// so "Typing_Extensions" and "typing-extensions" compare equal.
std::string normalized_name(const std::string& name) {
  std::string out;
  for (char ch : lower(name)) {
    const bool sep = ch == '-' || ch == '_' || ch == '.';
    if (!sep)
      out += ch;
    else if (out.empty() || out.back() != '-')
      out += '-';
  }
  return out;
}

// The package name at the front of a requirement: "requests[socks]>=2 ..."
// -> "requests".
std::string requirement_name(const std::string& spec) {
  const size_t begin = spec.find_first_not_of(" \t");
  if (begin == std::string::npos)
    return {};
  const size_t end = spec.find_first_of(" \t=<>!~[;@", begin);
  return spec.substr(begin, end == std::string::npos ? end : end - begin);
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
      "btn_refresh":  { "type": "Button", "label": "Refresh" },
      "btn_outdated": { "type": "Button", "label": "Check for updates" },
      "filter":       { "type": "InputText", "hint": "Filter by name", "width": 180 },
      "state":        { "type": "Combo", "items": "All\nOutdated\nEditable", "value": 0, "width": 110 },
      "spring":       { "type": "Spring" },
      "btn_freeze":   { "type": "Button", "label": "Freeze" },
      "btn_check":    { "type": "Button", "label": "Check" }
    } },
    "install_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":      { "type": "Label", "text": "Install new package:" },
      "spec":         { "type": "InputText", "hint": "exact name, e.g. requests or requests==2.31.0", "width": 330 },
      "btn_versions": { "type": "Button", "label": "Look up" },
      "btn_install":  { "type": "Button", "label": "Install" }
    } },
    "req_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":          { "type": "Label", "text": "Or a requirements file:" },
      "req_path":         { "type": "InputText", "hint": "path on the client machine", "width": 250 },
      "btn_requirements": { "type": "Button", "label": "Install requirements" }
    } },
    "opts_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption": { "type": "Label", "text": "Install options:" },
      "upgrade": { "type": "Checkbox", "label": "Upgrade", "value": false },
      "user":    { "type": "Checkbox", "label": "User", "value": false },
      "pre":     { "type": "Checkbox", "label": "Pre-releases", "value": false }
    } },
    "env": { "type": "Label", "text": "" },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pip_packages_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":     { "type": "TableColumn", "label": "Name",              "flags": "WidthFixed", "init_width": 220, "column_id": 0 },
        "col_version":  { "type": "TableColumn", "label": "Version",           "flags": "WidthFixed", "init_width": 110, "column_id": 1 },
        "col_latest":   { "type": "TableColumn", "label": "Latest",            "flags": "WidthFixed", "init_width": 110, "column_id": 2 },
        "col_location": { "type": "TableColumn", "label": "Editable location", "flags": "WidthStretch",                   "column_id": 3 },
        "col_actions":  { "type": "TableColumn", "label": "",                  "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

static constexpr const char* kVersionsLayout = R"json({
  "type": "Window", "title": "Versions", "width": 460, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "(no package selected)" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "Refresh" }
    } },
    "status": { "type": "Label", "text": "Type a name in Packages and press Look up." },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pip_versions_table", "columns": 3,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_version": { "type": "TableColumn", "label": "Version", "flags": "WidthFixed", "init_width": 150, "column_id": 0 },
        "col_note":    { "type": "TableColumn", "label": "",        "flags": "WidthStretch",                   "column_id": 1 },
        "col_actions": { "type": "TableColumn", "label": "",        "flags": "WidthFixed", "init_width": 40,  "column_id": 2 }
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

// The Console window: a FIFO-capped `Table` tracing every `pip` command the
// client ran. "auto_scroll": true so it follows the newest row.

static constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 900, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##pip_console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "headers": true,
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

// The progress dialog: a true modal ("modal": true), built on demand. It has
// a fixed size rather than MessageBox's AlwaysAutoResize so appended output
// never resizes it. No child carries an explicit "width" inside a
// HorizontalLayout -- see message_box.cpp on why that breaks hit-testing in
// a modal.

static constexpr const char* kProgressLayout = R"json({
  "type": "Window", "title": "Running pip", "modal": true, "width": 720, "height": 420,
  "flags": "NoResize|NoCollapse",
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 6, "children": {
    "command": { "type": "Label", "text": "" },
    "bar":     { "type": "ProgressBar", "value": -0.001, "label": "", "width": -1 },
    "result":  { "type": "Label", "text": "", "visible": false },
    "table": {
      "type": "Table", "id": "##pip_progress_table", "columns": 1,
      "flags": "RowBg|Borders|ScrollX|ScrollY", "headers": false,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_line": { "type": "TableColumn", "label": "Output", "flags": "WidthStretch", "column_id": 0 }
      }
    },
    "btn_cancel": { "type": "Button", "label": "Cancel" }
  } } }
})json";

std::string plural(size_t n, const char* noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

} // namespace

// ── pip_frontend ───────────────────────────────────────────────────────────

pip_frontend::pip_frontend(dynamic&& base) : form(std::move(base)) {}

void pip_frontend::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void pip_frontend::set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

ui_element_ptr pip_frontend::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void pip_frontend::on_init() {
  internal_root_key_ = next_available_key("__pip_");

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
      set_status(packages_, "Checking the package index for updates ...", true);
      emit("outdated_requested"_key);
    });
    click("vbox.toolbar.btn_freeze", [this] { open_details("freeze", {}); });
    click("vbox.toolbar.btn_check", [this] { open_details("check", {}); });

    tree.with("vbox.install_bar.spec", [&](const auto& e) {
      spec_input_ = e;
      spec_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.opts_bar.upgrade", [&](const auto& e) { upgrade_id_ = wish_id_of(e); });
    tree.with("vbox.opts_bar.user", [&](const auto& e) { user_id_ = wish_id_of(e); });
    tree.with("vbox.opts_bar.pre", [&](const auto& e) { pre_id_ = wish_id_of(e); });
    click("vbox.install_bar.btn_install", [this] {
      if (spec_text_.find_first_not_of(" \t") == std::string::npos) {
        set_status(packages_, "Enter a package to install (for example requests or requests==2.31.0)", false);
        return;
      }
      emit_install("install_requested"_key, "spec"_key, spec_text_);
      spec_text_.clear();
      if (spec_input_)
        spec_input_["value"_key] = std::string{};
    });
    click("vbox.install_bar.btn_versions", [this] {
      const std::string name = requirement_name(spec_text_);
      if (name.empty())
        set_status(packages_, "Enter a package name to look it up on the package index", false);
      else {
        set_status(packages_, packages_summary_, true); // drop a stale validation message
        open_versions(name);
      }
    });

    tree.with("vbox.req_bar.req_path", [&](const auto& e) { req_input_id_ = wish_id_of(e); });
    click("vbox.req_bar.btn_requirements", [this] {
      if (req_text_.empty())
        set_status(packages_, "Enter the path of a requirements file", false);
      else
        emit_install("requirements_requested"_key, "path"_key, req_text_);
    });
    tree.with("vbox.env", [&](const auto& e) { env_label_ = e; });
  });

  build_list_window(versions_window_, kVersionsLayout, internal_root_key_ + "_versions", [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { versions_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      click_handlers_[wish_id_of(e)] = [this] { emit_versions_request(); };
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

  progress_root_key_ = internal_root_key_ + "_progress";

  console_root_key_ = internal_root_key_ + "_console";
  build_text_window(console_root_key_, kConsoleLayout, console_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.table", [&](const auto& e) { console_table_ = e; });
  });

  // Seed the first-run arrangement (mirrors helm / kubectl): a wide left
  // column with Packages over a Console strip, and a narrower right column
  // with Details over Versions. Owned by imgui.ini after the first run; bump
  // the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "pip_dock", "Pip",
        layout(
            split(
                dir::left, 0.6f,
                split(dir::down, 0.24f, area({console_root_key_}), area({internal_root_key_})),
                split(dir::down, 0.4f, area({versions_window_.root_key}), area({details_root_key_}))),
            /*version=*/1, /*target=*/"pip_dock")));
  }

  // Initial population is triggered client-side (run_pip() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event, which would race ahead of that wiring.
}

void pip_frontend::build_text_window(
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

void pip_frontend::build_list_window(
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

void pip_frontend::set_details_text(const std::string& text) {
  if (!details_editor_)
    return;
  // A fresh name every call: the TextEditor renderer only reloads when
  // file_path changes, so rewriting one fixed file would leave the pane
  // showing stale content. Under "private/" because `pip freeze` can list
  // private index / VCS URLs -- see context.hpp's resource_dir doc comment.
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

void pip_frontend::remove_details_file() {
  if (details_file_.empty())
    return;
  std::error_code ec;
  std::filesystem::remove(resource_dir_ / details_file_, ec);
  details_file_.clear();
}

void pip_frontend::open_details(const std::string& kind, const std::string& name) {
  open_details_kind_ = kind;
  open_details_name_ = name;
  if (details_target_label_)
    details_target_label_["text"_key] = name.empty() ? "pip " + kind : kind + ": " + name;
  emit_details_request();
}

void pip_frontend::emit_details_request() {
  if (open_details_kind_.empty())
    return;
  dynamic p;
  p["kind"_key] = open_details_kind_;
  p["name"_key] = open_details_name_;
  emit("details_requested"_key, std::move(p));
}

void pip_frontend::open_versions(const std::string& name) {
  open_versions_name_ = name;
  versions_.clear();
  versions_latest_.clear();
  clear_list_rows(versions_window_, version_rows_, next_version_key_);
  if (versions_window_.table)
    versions_window_.table->refresh_children_order();
  if (versions_target_label_)
    versions_target_label_["text"_key] = "versions: " + name;
  set_status(versions_window_, "Asking the package index ...", true);
  emit_versions_request();
}

void pip_frontend::emit_versions_request() {
  if (open_versions_name_.empty())
    return;
  dynamic p;
  p["name"_key] = open_versions_name_;
  p["pre"_key] = opt_pre_;
  emit("versions_requested"_key, std::move(p));
}

void pip_frontend::emit_install(key_t event, key_t field, const std::string& value) {
  set_status(packages_, "Running pip install " + value + " ...", true);
  dynamic p;
  p[field] = value;
  p["upgrade"_key] = opt_upgrade_;
  p["user"_key] = opt_user_;
  p["pre"_key] = opt_pre_;
  emit(event, std::move(p));
}

// ── Confirmation modal (helm_frontend::show_confirm() port) ────────────────

void pip_frontend::show_confirm(const std::string& message, std::function<void()> on_confirm) {
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

void pip_frontend::clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key) {
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

void pip_frontend::add_list_row(
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
    menu_action_targets_[wish_id_of(mi)] = row_action{meta.scope, meta.name, meta.version, it.action};
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

void pip_frontend::set_status(list_window& lw, const std::string& text, bool ok) {
  if (!lw.status_label)
    return;
  // One line only: a multi-line pip error would grow the label and shift the
  // table below it. The whole message is in the progress dialog and Console.
  lw.status_label["text"_key] = text.substr(0, text.find('\n'));
  lw.status_label["text_color_light"_key] = std::string{ok ? kIdleLight : kBadLight};
  lw.status_label["text_color_dark"_key] = std::string{ok ? kIdleDark : kBadDark};
}

void pip_frontend::apply_package_filter() {
  const std::string needle = lower(name_filter_);
  for (auto& r : package_rows_) {
    if (!r.row)
      continue;
    bool show = true;
    if (state_filter_ == 1)
      show = r.outdated;
    else if (state_filter_ == 2)
      show = r.editable;
    if (show && !needle.empty())
      show = lower(r.name).find(needle) != std::string::npos;
    r.row["visible"_key] = show;
  }
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void pip_frontend::rebuild_packages(const dynamic& args) {
  clear_list_rows(packages_, package_rows_, next_package_key_);

  size_t outdated = 0, total = 0;
  for_each_entry(args, "packages"_key, [&](const dynamic& e) {
    list_row meta;
    meta.scope = "package";
    meta.name = str_of(e, "name"_key);
    meta.version = str_of(e, "version"_key);
    const std::string latest = str_of(e, "latest"_key);
    const std::string location = str_of(e, "location"_key);
    meta.outdated = !latest.empty() && latest != meta.version;
    meta.editable = !location.empty();

    std::vector<ui_element_ptr> cells = {
        make_label(meta.name),
        make_label(meta.version),
        make_label(meta.outdated ? latest : std::string{}, kWarnLight, kWarnDark),
        make_label(location, kIdleLight, kIdleDark),
    };
    std::vector<menu_spec> items = {
        {"Details", "show", false},      {"Files", "files", false},          {"Versions", "versions", false}, {},
        {"Upgrade", "upgrade", false},   {"Reinstall", "reinstall", true},   {"Uninstall", "uninstall", true}};
    if (meta.outdated)
      ++outdated;
    ++total;
    add_list_row(packages_, package_rows_, next_package_key_, std::move(meta), cells, items);
  });

  if (packages_.table)
    packages_.table->refresh_children_order();
  apply_package_filter();

  std::string status = total == 0 ? std::string{"No packages installed."} : plural(total, "package");
  if (total != 0 && flag_of(args, "outdated_checked"_key))
    status += " (" + std::to_string(outdated) + " outdated)";
  packages_summary_ = status;
  set_status(packages_, status, true);

  // The "installed" mark in the Versions table follows this snapshot.
  if (!open_versions_name_.empty() && !versions_.empty())
    rebuild_versions();
}

std::string pip_frontend::installed_version(const std::string& name) const {
  const std::string wanted = normalized_name(name);
  for (auto& r : package_rows_) {
    if (normalized_name(r.name) == wanted)
      return r.version;
  }
  return {};
}

void pip_frontend::rebuild_versions() {
  clear_list_rows(versions_window_, version_rows_, next_version_key_);

  const std::string installed = installed_version(open_versions_name_);
  size_t total = 0;
  for (auto& version : versions_) {
    if (total++ >= kMaxVersionRows)
      continue;
    list_row meta;
    meta.scope = "version";
    meta.name = open_versions_name_;
    meta.version = version;

    const bool is_installed = version == installed;
    std::string note = is_installed ? "installed" : "";
    if (version == versions_latest_)
      note += note.empty() ? "latest" : ", latest";

    std::vector<ui_element_ptr> cells = {
        make_label(version),
        make_label(note, is_installed ? kOkLight : kIdleLight, is_installed ? kOkDark : kIdleDark),
    };
    add_list_row(
        versions_window_, version_rows_, next_version_key_, std::move(meta), cells,
        {{"Install this version", "install", false}});
  }

  if (versions_window_.table)
    versions_window_.table->refresh_children_order();
  std::string status = plural(total, "version") + (installed.empty() ? ", not installed" : ", installed: " + installed);
  if (total > kMaxVersionRows)
    status += " (showing the newest " + std::to_string(kMaxVersionRows) + ")";
  set_status(versions_window_, status, true);
}

// ── RMI methods ────────────────────────────────────────────────────────────

dynamic pip_frontend::do_update_packages(const dynamic& args) {
  rebuild_packages(args);
  return dynamic{};
}

dynamic pip_frontend::do_update_versions(const dynamic& args) {
  if (str_of(args, "name"_key) != open_versions_name_)
    return dynamic{}; // stale response for a package the user navigated away from.
  versions_.clear();
  versions_latest_ = str_of(args, "latest"_key);
  const auto* arr_f = args.findField<dynamic_ptr>("versions"_key);
  if (arr_f && *arr_f) {
    (*arr_f)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>())
        versions_.push_back(f.as<std::string>());
    });
  }
  rebuild_versions();
  return dynamic{};
}

dynamic pip_frontend::do_update_details(const dynamic& args) {
  if (str_of(args, "kind"_key) != open_details_kind_ || str_of(args, "name"_key) != open_details_name_)
    return dynamic{};
  if (details_target_label_)
    details_target_label_["text"_key] = str_of(args, "title"_key);
  set_details_text(str_of(args, "text"_key));
  return dynamic{};
}

dynamic pip_frontend::do_command_result(const dynamic& args) {
  const std::string command = str_of(args, "command"_key);
  const bool ok = args.as<bool>("ok"_key);
  const std::string output = str_of(args, "output"_key);
  list_window& lw = str_of(args, "scope"_key) == "versions" ? versions_window_ : packages_;
  if (ok) {
    set_status(lw, command + ": OK", true);
  } else {
    const std::string text = command + " failed: " + (output.empty() ? "unknown error" : output);
    set_status(lw, text, false);
    if (progress_.state == progress_dialog::phase::open)
      progress_.failure = text;
  }
  return dynamic{};
}

// ── Progress dialog ────────────────────────────────────────────────────────

void pip_frontend::with_context(const std::function<void()>& fn) {
  // The idiom instantiate_child_form() uses: on_event() runs outside RMI
  // dispatch, while building / erasing top-level objects needs sess().
  if (detail::current_context) {
    fn();
    return;
  }
  auto sess = context_wlock{*sync_ctx_};
  detail::current_context = &*sess;
  fn();
  detail::current_context = nullptr;
}

void pip_frontend::open_progress_dialog() {
  progress_ = progress_dialog{};
  build_text_window(progress_root_key_, kProgressLayout, progress_.window_id, [&](ui_tree& tree) {
    for (auto& [key, elem] : tree)
      progress_.object_ids.push_back(wish_id_of(elem));
    progress_.window = tree[""];
    tree.with("vbox.command", [&](const auto& e) { progress_.command_label = e; });
    tree.with("vbox.bar", [&](const auto& e) { progress_.bar = e; });
    tree.with("vbox.result", [&](const auto& e) { progress_.result_label = e; });
    tree.with("vbox.table", [&](const auto& e) { progress_.table = e; });
    tree.with("vbox.btn_cancel", [&](const auto& e) {
      progress_.button = e;
      click_handlers_[wish_id_of(e)] = [this] {
        if (progress_.finished) {
          request_progress_close();
          return;
        }
        if (progress_.cancelling)
          return;
        progress_.cancelling = true;
        if (progress_.command_label)
          progress_.command_label["text"_key] = std::string{"Cancelling ..."};
        emit("cancel_requested"_key);
      };
    });
  });
  progress_.state = progress_dialog::phase::open;
}

void pip_frontend::request_progress_close() {
  if (progress_.state != progress_dialog::phase::open)
    return;
  progress_.state = progress_dialog::phase::closing;
  if (progress_.window)
    progress_.window["__request_close__"_key] = true;
}

void pip_frontend::destroy_progress_dialog() {
  if (progress_.state == progress_dialog::phase::closed)
    return;
  for (auto id : progress_.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
  for (auto& row : progress_.rows) {
    for (auto id : row.object_ids)
      ctx().objects.erase(id.id);
  }
  remove_objects_at(progress_root_key_);
  progress_ = progress_dialog{};
}

void pip_frontend::append_progress_line(const std::string& text, bool is_command) {
  if (!progress_.table)
    return;
  auto* children_p = progress_.table->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  assign_id(row);
  ui_element_ptr cell = is_command ? make_label("$ " + text, kOkLight, kOkDark) : make_label(text);
  set_children_list(row, {cell});

  console_row_entry entry;
  entry.child_key = progress_.next_child_key++;
  entry.object_ids = {wish_id_of(row), wish_id_of(cell)};
  (*children)[entry.child_key] = dynamic_ptr{row};
  progress_.rows.push_back(std::move(entry));

  if (progress_.rows.size() > kMaxProgressRows) {
    for (auto id : progress_.rows.front().object_ids)
      ctx().objects.erase(id.id);
    children->erase(progress_.rows.front().child_key);
    progress_.rows.pop_front();
  }
}

dynamic pip_frontend::do_set_progress(const dynamic& args) {
  using phase = progress_dialog::phase;

  if (!flag_of(args, "active"_key)) {
    progress_.reopen = false;
    if (progress_.state != phase::open)
      return dynamic{};
    if (progress_.failure.empty() || progress_.cancelling) {
      request_progress_close();
      return dynamic{};
    }
    // Keep a failure on screen, with the output that led to it.
    progress_.finished = true;
    if (progress_.command_label)
      progress_.command_label["text"_key] = std::string{"pip finished with an error"};
    if (progress_.bar)
      progress_.bar["visible"_key] = false;
    if (progress_.result_label) {
      progress_.result_label["text"_key] = progress_.failure;
      progress_.result_label["text_color_light"_key] = std::string{kBadLight};
      progress_.result_label["text_color_dark"_key] = std::string{kBadDark};
      progress_.result_label["visible"_key] = true;
    }
    if (progress_.button)
      progress_.button["label"_key] = std::string{"Close"};
    return dynamic{};
  }

  if (progress_.state == phase::closing) {
    progress_.reopen = true; // rebuilt once the renderer confirms the close.
    return dynamic{};
  }
  if (progress_.state == phase::closed)
    open_progress_dialog();

  if (progress_.finished) { // a queued command started behind a failure.
    progress_.finished = false;
    progress_.failure.clear();
    if (progress_.bar)
      progress_.bar["visible"_key] = true;
    if (progress_.result_label)
      progress_.result_label["visible"_key] = false;
    if (progress_.button)
      progress_.button["label"_key] = std::string{"Cancel"};
  }

  const std::string command = str_of(args, "command"_key);
  if (command != progress_.command) {
    progress_.command = command;
    append_progress_line(command, /*is_command=*/true);
  }
  if (progress_.command_label && !progress_.cancelling)
    progress_.command_label["text"_key] = command;

  const auto* lines = args.findField<dynamic_ptr>("lines"_key);
  if (lines && *lines) {
    (*lines)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>())
        append_progress_line(f.as<std::string>(), /*is_command=*/false);
    });
  }
  if (progress_.table)
    progress_.table->refresh_children_order();

  // A negative ProgressBar value draws ImGui's indeterminate animation,
  // whose position follows the value -- hence the ever-growing phase.
  if (progress_.bar) {
    const auto* seconds = args.findField<float>("phase"_key);
    progress_.bar["value"_key] = -0.001f - (seconds ? *seconds : 0.0f) * 0.5f;
  }
  return dynamic{};
}

dynamic pip_frontend::do_set_environment(const dynamic& args) {
  if (!env_label_)
    return dynamic{};
  const std::string interpreter = str_of(args, "interpreter"_key);
  env_label_["text"_key] = (interpreter.empty() ? std::string{} : interpreter + ": ") + str_of(args, "text"_key);
  env_label_["text_color_light"_key] = std::string{kIdleLight};
  env_label_["text_color_dark"_key] = std::string{kIdleDark};
  return dynamic{};
}

// ── Console window (client `pip` subprocess trace) ─────────────────────────

void pip_frontend::append_console_row(
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

void pip_frontend::erase_console_row_objects(const console_row_entry& entry) {
  for (auto id : entry.object_ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

void pip_frontend::clear_console_rows() {
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

dynamic pip_frontend::do_append_command_log(const dynamic& args) {
  append_console_row(
      str_of(args, "command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      str_of(args, "output"_key));
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void pip_frontend::run_row_action(const row_action& target) {
  if (target.scope == "version") {
    // Pin exactly this version; the Upgrade checkbox is irrelevant to it.
    const std::string spec = target.name + "==" + target.version;
    set_status(packages_, "Running pip install " + spec + " ...", true);
    dynamic p;
    p["spec"_key] = spec;
    p["upgrade"_key] = false;
    p["user"_key] = opt_user_;
    p["pre"_key] = false;
    emit("install_requested"_key, std::move(p));
    return;
  }

  if (target.action == "show" || target.action == "files") {
    open_details(target.action, target.name);
    return;
  }
  if (target.action == "versions") {
    open_versions(target.name);
    return;
  }

  auto fire = [this, target] {
    set_status(packages_, "Running pip " + target.action + " " + target.name + " ...", true);
    dynamic p;
    p["name"_key] = target.name;
    p["action"_key] = target.action;
    emit("package_action_requested"_key, std::move(p));
  };
  const std::string what = "'" + target.name + "' " + target.version;
  if (target.action == "uninstall")
    show_confirm("Uninstall " + what + "? Packages that depend on it will stop working.", fire);
  else if (target.action == "reinstall")
    show_confirm("Reinstall " + what + "? Its files are replaced; its dependencies are left as they are.", fire);
  else
    fire();
}

void pip_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // The renderer confirmed the progress modal closed (request_progress_close()).
  if (progress_.state != progress_dialog::phase::closed && event == "closed"_key && id == progress_.window_id) {
    const bool reopen = progress_.reopen;
    with_context([&] {
      destroy_progress_dialog();
      if (reopen)
        open_progress_dialog();
    });
    return;
  }

  // Any window's X button -> tear everything down.
  if (event == "closed"_key && (id == packages_.window_id || id == versions_window_.window_id ||
                                id == details_window_id_ || id == console_window_id_)) {
    remove_details_file();
    destroy_progress_dialog();
    emit("closed"_key);
    remove_objects_at(versions_window_.root_key);
    remove_objects_at(details_root_key_);
    remove_objects_at(console_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    if (id == name_filter_id_) {
      name_filter_ = payload.as<std::string>("value"_key);
      apply_package_filter();
    } else if (id == state_combo_id_) {
      state_filter_ = payload.as<int32_t>("value"_key);
      apply_package_filter();
    } else if (id == spec_input_id_) {
      spec_text_ = payload.as<std::string>("value"_key);
    } else if (id == req_input_id_) {
      req_text_ = payload.as<std::string>("value"_key);
    } else if (id == upgrade_id_) {
      opt_upgrade_ = payload.as<bool>("value"_key);
    } else if (id == user_id_) {
      opt_user_ = payload.as<bool>("value"_key);
    } else if (id == pre_id_) {
      opt_pre_ = payload.as<bool>("value"_key);
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

void register_pip() {
  auto proto = dynamic_ptr{"PipFrontend"_key, {}};

  auto add_method = [&](key_t name, dynamic (pip_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<pip_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_packages"_key, &pip_frontend::do_update_packages);
  add_method("update_versions"_key, &pip_frontend::do_update_versions);
  add_method("update_details"_key, &pip_frontend::do_update_details);
  add_method("command_result"_key, &pip_frontend::do_command_result);
  add_method("append_command_log"_key, &pip_frontend::do_append_command_log);
  add_method("set_environment"_key, &pip_frontend::do_set_environment);
  add_method("set_progress"_key, &pip_frontend::do_set_progress);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PipFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "GUI frontend for the local `pip` CLI (installed Python packages, index versions, install / upgrade / "
      "uninstall). All `pip` invocation happens client-side; this form only renders whatever snapshot it was "
      "last given. Listen for the 'closed' event to detect when the user is done, and the '*_requested' events "
      "to react to user actions -- see pip.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<pip_frontend>("wish"_key, "PipFrontend"_key));
}

} // namespace bdg::wish
