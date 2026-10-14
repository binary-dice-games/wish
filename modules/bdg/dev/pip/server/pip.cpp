// MIT License © 2026 Binary Dice Games
/// @file pip.cpp
/// @brief Implementation of the PipFrontend form.
///
/// Inline JSON window layouts + C++-built table rows on the panels shared
/// by the bdg tool forms (modules/bdg/common/server). The two list windows
/// (Packages / Versions) share one package_row type and run_row_action()
/// path.
#include "pip.hpp"
#include "modules/bdg/dev/pip/pip_settings.hpp"

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
using common::str_of;
using common::wish_id_of;

namespace {

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
  "type": "Window", "title": "$$PIP_PACKAGES", "width": 900, "height": 520,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh":  { "type": "Button", "label": "$$PIP_REFRESH", "shortcut": "F5", "icon": "res/icons/refresh.png" },
      "btn_outdated": { "type": "Button", "label": "$$PIP_CHECK_FOR_UPDATES", "icon": "res/icons/arrow_up.png" },
      "filter":       { "type": "InputText", "hint": "$$PIP_FILTER_BY_NAME", "width": 180 },
      "state":        { "type": "Combo", "items": "$$PIP_STATE_ITEMS", "value": 0, "width": 110 },
      "spring":       { "type": "Spring" },
      "btn_freeze":   { "type": "Button", "label": "$$PIP_FREEZE", "icon": "res/icons/document.png" },
      "btn_check":    { "type": "Button", "label": "$$PIP_CHECK", "icon": "res/icons/check.png" }
    } },
    "install_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":      { "type": "Label", "text": "$$PIP_INSTALL_NEW_PACKAGE" },
      "spec":         { "type": "InputText", "hint": "$$PIP_PACKAGE_HINT", "width": 330 },
      "btn_versions": { "type": "Button", "label": "$$PIP_LOOK_UP", "icon": "res/icons/search.png" },
      "btn_install":  { "type": "Button", "label": "$$PIP_INSTALL", "icon": "res/icons/download.png" }
    } },
    "req_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption":          { "type": "Label", "text": "$$PIP_OR_A_REQUIREMENTS_FILE" },
      "req_path":         { "type": "InputText", "hint": "$$PIP_REQUIREMENTS_HINT", "width": 250 },
      "btn_requirements": { "type": "Button", "label": "$$PIP_INSTALL_REQUIREMENTS", "icon": "res/icons/download.png" }
    } },
    "opts_bar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "caption": { "type": "Label", "text": "$$PIP_INSTALL_OPTIONS" },
      "upgrade": { "type": "Checkbox", "label": "$$PIP_UPGRADE", "value": false },
      "user":    { "type": "Checkbox", "label": "$$PIP_USER", "value": false },
      "pre":     { "type": "Checkbox", "label": "$$PIP_PRE_RELEASES", "value": false }
    } },
    "env": { "type": "Label", "text": "" },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pip_packages_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":     { "type": "TableColumn", "label": "$$PIP_NAME",              "flags": "WidthFixed", "init_width": 220, "column_id": 0 },
        "col_version":  { "type": "TableColumn", "label": "$$PIP_VERSION",           "flags": "WidthFixed", "init_width": 110, "column_id": 1 },
        "col_latest":   { "type": "TableColumn", "label": "$$PIP_LATEST",            "flags": "WidthFixed", "init_width": 110, "column_id": 2 },
        "col_location": { "type": "TableColumn", "label": "$$PIP_EDITABLE_LOCATION", "flags": "WidthStretch",                   "column_id": 3 },
        "col_actions":  { "type": "TableColumn", "label": "",                  "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

static constexpr const char* kVersionsLayout = R"json({
  "type": "Window", "title": "$$PIP_VERSIONS", "width": 460, "height": 420,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "$$PIP_NO_PACKAGE_SELECTED" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "$$PIP_REFRESH", "shortcut": "F5", "icon": "res/icons/refresh.png" }
    } },
    "status": { "type": "Label", "text": "$$PIP_VERSIONS_PLACEHOLDER" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##pip_versions_table", "columns": 3,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_version": { "type": "TableColumn", "label": "$$PIP_VERSION", "flags": "WidthFixed", "init_width": 150, "column_id": 0 },
        "col_note":    { "type": "TableColumn", "label": "",        "flags": "WidthStretch",                   "column_id": 1 },
        "col_actions": { "type": "TableColumn", "label": "",        "flags": "WidthFixed", "init_width": 40,  "column_id": 2 }
      }
    }
  } } }
})json";

} // namespace

// ── pip_frontend ───────────────────────────────────────────────────────────

pip_frontend::pip_frontend(dynamic&& base) : tool_form(std::move(base)) {
  i18n_prefix_ = "bdg/dev/pip";
}

void pip_frontend::on_init() {
  internal_root_key_ = next_available_key("__pip_");

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
      set_status(packages_, "$$PIP_CHECKING_FOR_UPDATES", true);
      emit("outdated_requested"_key);
    });
    click("vbox.toolbar.btn_freeze", [this] { open_details("freeze", {}); });
    click("vbox.toolbar.btn_check", [this] { open_details("check", {}); });

    tree.with("vbox.install_bar.spec", [&](const auto& e) {
      spec_input_ = e;
      spec_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.opts_bar.upgrade", [&](const auto& e) {
      upgrade_id_ = wish_id_of(e);
      opt_upgrade_ = setting_bool(pip_settings::upgrade());
      e["value"_key] = opt_upgrade_;
    });
    tree.with("vbox.opts_bar.user", [&](const auto& e) {
      user_id_ = wish_id_of(e);
      opt_user_ = setting_bool(pip_settings::user());
      e["value"_key] = opt_user_;
    });
    tree.with("vbox.opts_bar.pre", [&](const auto& e) {
      pre_id_ = wish_id_of(e);
      opt_pre_ = setting_bool(pip_settings::pre());
      e["value"_key] = opt_pre_;
    });
    click("vbox.install_bar.btn_install", [this] {
      if (spec_text_.find_first_not_of(" \t") == std::string::npos) {
        set_status(packages_, "$$PIP_ENTER_PACKAGE_TO_INSTALL", false);
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
        set_status(packages_, "$$PIP_ENTER_PACKAGE_TO_LOOK_UP", false);
      else {
        set_status(packages_, packages_summary_, true); // drop a stale validation message
        open_versions(name);
      }
    });

    tree.with("vbox.req_bar.req_path", [&](const auto& e) { req_input_id_ = wish_id_of(e); });
    click("vbox.req_bar.btn_requirements", [this] {
      if (req_text_.empty())
        set_status(packages_, "$$PIP_ENTER_REQUIREMENTS_PATH", false);
      else
        emit_install("requirements_requested"_key, "path"_key, req_text_);
    });
    tree.with("vbox.env", [&](const auto& e) { env_label_ = e; });
  });

  versions_window_.build(*this, internal_root_key_ + "_versions", kVersionsLayout, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.target", [&](const auto& e) { versions_target_label_ = e; });
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { emit_versions_request(); });
    });
  });

  details_.build(
      *this, internal_root_key_ + "_details", {.title = "$$COMMON_DETAILS", .width = 460, .on_refresh = [this] {
                                                 emit_details_request();
                                               }});

  console_.build(
      *this, internal_root_key_ + "_console", {.table_id = "##pip_console_table", .width = 900, .command_width = 320});

  // Seed the first-run arrangement (mirrors helm / kubectl): a wide left
  // column with Packages over a Console strip, and a narrower right column
  // with Details over Versions. Owned by imgui.ini after the first run; bump
  // the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "pip_dock",
        tr("$$PIP_PIP"),
        layout(
            split(
                dir::left,
                0.6f,
                split(dir::down, 0.24f, area({console_.root_key()}), area({internal_root_key_})),
                split(dir::down, 0.4f, area({versions_window_.root_key()}), area({details_.root_key()}))),
            /*version=*/1,
            /*target=*/"pip_dock")));
  }

  // Initial population is triggered client-side (run_pip() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event, which would race ahead of that wiring.
}

void pip_frontend::open_details(const std::string& kind, const std::string& name) {
  open_details_kind_ = kind;
  open_details_name_ = name;
  details_.set_title(name.empty() ? "pip " + kind : kind + ": " + name);
  emit_details_request();
}

void pip_frontend::emit_details_request() {
  if (open_details_kind_.empty())
    return;
  emit("details_requested"_key, make_payload("kind"_key, open_details_kind_, "name"_key, open_details_name_));
}

void pip_frontend::open_versions(const std::string& name) {
  open_versions_name_ = name;
  versions_.clear();
  versions_latest_.clear();
  versions_window_.clear();
  versions_window_.refresh();
  if (versions_target_label_)
    versions_target_label_["text"_key] = "versions: " + name;
  set_status(versions_window_, "$$PIP_ASKING_INDEX", true);
  emit_versions_request();
}

void pip_frontend::emit_versions_request() {
  if (open_versions_name_.empty())
    return;
  emit("versions_requested"_key, make_payload("name"_key, open_versions_name_, "pre"_key, opt_pre_));
}

void pip_frontend::emit_install(key_t event, key_t field, const std::string& value) {
  set_status(packages_, trf("$$PIP_RUNNING_INSTALL", {value}), true);
  dynamic p;
  p[field] = value;
  p["upgrade"_key] = opt_upgrade_;
  p["user"_key] = opt_user_;
  p["pre"_key] = opt_pre_;
  emit(event, std::move(p));
}

common::menu_item
pip_frontend::action_item(const std::string& label, const package_row& r, const std::string& action, bool confirm) {
  return {label, [this, r, action] { run_row_action(r, action); }, confirm};
}

void pip_frontend::apply_package_filter() {
  const std::string needle = lower(name_filter_);
  packages_.apply_filter([&](const package_row& r) {
    if (state_filter_ == 1 && !r.outdated)
      return false;
    if (state_filter_ == 2 && !r.editable)
      return false;
    return needle.empty() || lower(r.name).find(needle) != std::string::npos;
  });
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void pip_frontend::rebuild_packages(const dynamic& args) {
  packages_.clear();

  size_t outdated = 0, total = 0;
  for_each_entry(args, "packages"_key, [&](const dynamic& e) {
    package_row r;
    r.scope = "package";
    r.name = str_of(e, "name"_key);
    r.version = str_of(e, "version"_key);
    const std::string latest = str_of(e, "latest"_key);
    const std::string location = str_of(e, "location"_key);
    r.outdated = !latest.empty() && latest != r.version;
    r.editable = !location.empty();

    std::vector<ui_element_ptr> cells = {
        make_label(r.name),
        make_label(r.version),
        make_label(r.outdated ? latest : std::string{}, kWarn),
        make_label(location, kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("$$PIP_DETAILS", r, "show", false),
        action_item("$$PIP_FILES", r, "files", false),
        action_item("$$PIP_VERSIONS", r, "versions", false),
        {},
        action_item("$$PIP_UPGRADE", r, "upgrade", false),
        action_item("$$PIP_REINSTALL", r, "reinstall", true),
        action_item("$$PIP_UNINSTALL", r, "uninstall", true)};
    if (r.outdated)
      ++outdated;
    ++total;
    packages_.add(std::move(r), cells, items);
  });

  packages_.refresh();
  apply_package_filter();

  std::string status = total == 0 ? tr("$$PIP_NO_PACKAGES")
      : flag_of(args, "outdated_checked"_key)
      ? trf(total == 1 ? "$$PIP_N_PACKAGE_OUTDATED" : "$$PIP_N_PACKAGES_OUTDATED",
            {std::to_string(total), std::to_string(outdated)})
      : trf(total == 1 ? "$$PIP_N_PACKAGE" : "$$PIP_N_PACKAGES", {std::to_string(total)});
  packages_summary_ = status;
  set_status(packages_, status, true);

  // The "installed" mark in the Versions table follows this snapshot.
  if (!open_versions_name_.empty() && !versions_.empty())
    rebuild_versions();
}

std::string pip_frontend::installed_version(const std::string& name) const {
  const std::string wanted = normalized_name(name);
  for (auto& r : packages_.rows()) {
    if (normalized_name(r.meta.name) == wanted)
      return r.meta.version;
  }
  return {};
}

void pip_frontend::rebuild_versions() {
  versions_window_.clear();

  const std::string installed = installed_version(open_versions_name_);
  size_t total = 0;
  for (auto& version : versions_) {
    if (total++ >= kMaxVersionRows)
      continue;
    package_row r;
    r.scope = "version";
    r.name = open_versions_name_;
    r.version = version;

    const bool is_installed = version == installed;
    std::string note = is_installed ? tr("$$PIP_INSTALLED") : "";
    if (version == versions_latest_)
      note += (note.empty() ? "" : ", ") + tr("$$PIP_LATEST_NOTE");

    std::vector<ui_element_ptr> cells = {
        make_label(version),
        make_label(note, is_installed ? kOk : kIdle),
    };
    std::vector<common::menu_item> items = {action_item("$$PIP_INSTALL_THIS_VERSION", r, "install", false)};
    versions_window_.add(std::move(r), cells, items);
  }

  versions_window_.refresh();
  const std::string count = std::to_string(total);
  std::string status = installed.empty()
      ? trf(total == 1 ? "$$PIP_N_VERSION_NOT_INSTALLED" : "$$PIP_N_VERSIONS_NOT_INSTALLED", {count})
      : trf(total == 1 ? "$$PIP_N_VERSION_INSTALLED" : "$$PIP_N_VERSIONS_INSTALLED", {count, installed});
  if (total > kMaxVersionRows)
    status += " " + trf("$$PIP_SHOWING_NEWEST", {std::to_string(kMaxVersionRows)});
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
  details_.set_title(str_of(args, "title"_key));
  details_.set_text(str_of(args, "text"_key));
  return dynamic{};
}

dynamic pip_frontend::do_command_result(const dynamic& args) {
  list_window& lw = str_of(args, "scope"_key) == "versions" ? versions_window_ : packages_;
  bool ok = false;
  const std::string text = common::command_result_text(*this, args, ok);
  set_status(lw, text, ok);
  return dynamic{};
}

dynamic pip_frontend::do_set_environment(const dynamic& args) {
  if (!env_label_)
    return dynamic{};
  const std::string interpreter = str_of(args, "interpreter"_key);
  common::set_status_text(
      env_label_, (interpreter.empty() ? std::string{} : interpreter + ": ") + str_of(args, "text"_key), true);
  return dynamic{};
}

dynamic pip_frontend::do_append_command_log(const dynamic& args) {
  console_.append_from(args);
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void pip_frontend::run_row_action(const package_row& r, const std::string& action) {
  if (r.scope == "version") {
    // Pin exactly this version; the Upgrade checkbox is irrelevant to it.
    const std::string spec = r.name + "==" + r.version;
    set_status(packages_, trf("$$PIP_RUNNING_INSTALL", {spec}), true);
    emit(
        "install_requested"_key,
        make_payload("spec"_key, spec, "upgrade"_key, false, "user"_key, opt_user_, "pre"_key, false));
    return;
  }

  if (action == "show" || action == "files") {
    open_details(action, r.name);
    return;
  }
  if (action == "versions") {
    open_versions(r.name);
    return;
  }

  auto fire = [this, name = r.name, action] {
    set_status(packages_, trf("$$PIP_RUNNING_ACTION", {action, name}), true);
    emit("package_action_requested"_key, make_payload("name"_key, name, "action"_key, action));
  };
  if (action == "uninstall")
    show_confirm(trf("$$PIP_UNINSTALL_CONFIRM", {r.name, r.version}), fire);
  else if (action == "reinstall")
    show_confirm(trf("$$PIP_REINSTALL_CONFIRM", {r.name, r.version}), fire);
  else
    fire();
}

void pip_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // The frame's close button (or the taskbar's "Close window").
  if (event == "closed"_key && id == dock_frame_id()) {
    details_.remove_file();
    emit("closed"_key);
    remove_objects_at(versions_window_.root_key());
    remove_objects_at(details_.root_key());
    remove_objects_at(console_.root_key());
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
      save_setting(pip_settings::upgrade(), opt_upgrade_);
    } else if (id == user_id_) {
      opt_user_ = payload.as<bool>("value"_key);
      save_setting(pip_settings::user(), opt_user_);
    } else if (id == pre_id_) {
      opt_pre_ = payload.as<bool>("value"_key);
      save_setting(pip_settings::pre(), opt_pre_);
    }
    return;
  }

  if (event == "clicked"_key)
    dispatch_click(id);
}

// ── Registration ───────────────────────────────────────────────────────────

void register_pip() {
  common::register_tool_settings(pip_settings::all());
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
