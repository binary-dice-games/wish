// MIT License © 2026 Binary Dice Games
/// @file docker.cpp
/// @brief Implementation of the DockerFrontend form.
///
/// Inline JSON window layouts + C++-built table rows on the panels shared
/// by the bdg tool forms (modules/bdg/common/server). The four list windows
/// (Containers / Images / Volumes / Networks) share one build_list_window()
/// / run_row_action() path.
#include "docker.hpp"

#include "src/bison/bison_object.hpp"

#include <ui/dock_layout_spec.hpp>

#include <iomanip>
#include <map>
#include <sstream>

namespace bdg::wish {

using namespace bison;
using common::for_each_entry;
using common::kBad;
using common::kIdle;
using common::kOk;
using common::kWarn;
using common::make_payload;
using common::theme_color;
using common::wish_id_of;

namespace {

std::string format_percent(float pct) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << pct << "%";
  return oss.str();
}

// Container `docker ps` .State -> status-text colour.
theme_color state_colour(const std::string& state) {
  if (state == "running")
    return kOk;
  if (state == "paused" || state == "restarting")
    return kWarn;
  if (state == "dead")
    return kBad;
  return kIdle; // exited, created, removing, ...
}

// ── Window layouts ─────────────────────────────────────────────────────────
//
// The windows carry no "pos_x"/"pos_y": each opens un-positioned and docks
// into the ambient host dockspace. The *arrangement* (which pane, split
// sizes, tab groups) is seeded once at the end of on_init() via
// form::set_default_dock_layout(), then owned by imgui.ini like any user
// drag. "width"/"height" remain as the floating size a pane restores to when
// undocked.
//
// Each list table carries both "height": -1 (stretch row in `vbox`) and
// "outer_height": -1 (fill that region) -- the load-bearing pair documented
// in git.cpp / tail.cpp. `vbox` is each Window's sole direct child so it
// already fills the body (the `top` module's kLayout shape), no hint needed.
//
// docker_mock.json / docker_mock.html (this directory) mirror these layouts
// as a single tabbed window -- the mockup validated in the `editor` tool
// before implementation. Keep them roughly in sync when changing columns.

static constexpr const char* kContainersLayout = R"json({
  "type": "Window", "title": "Containers", "width": 940, "height": 500,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh" },
      "btn_prune":   { "type": "Button", "label": "Prune stopped..." },
      "filter":      { "type": "InputText", "hint": "Filter by name / image", "width": 240 },
      "state":       { "type": "Combo", "items": "All\nRunning\nStopped", "value": 0, "width": 110 }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##containers_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":    { "type": "TableColumn", "label": "Name",    "flags": "WidthFixed", "init_width": 150, "column_id": 0 },
        "col_image":   { "type": "TableColumn", "label": "Image",   "flags": "WidthStretch",                    "column_id": 1 },
        "col_status":  { "type": "TableColumn", "label": "Status",  "flags": "WidthFixed", "init_width": 175, "column_id": 2 },
        "col_ports":   { "type": "TableColumn", "label": "Ports",   "flags": "WidthFixed", "init_width": 160, "column_id": 3 },
        "col_created": { "type": "TableColumn", "label": "Created", "flags": "WidthFixed", "init_width": 110, "column_id": 4 },
        "col_actions": { "type": "TableColumn", "label": "",        "flags": "WidthFixed", "init_width": 40,  "column_id": 5 }
      }
    }
  } } }
})json";

static constexpr const char* kImagesLayout = R"json({
  "type": "Window", "title": "Images", "width": 820, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh" },
      "pull_ref":    { "type": "InputText", "hint": "repo:tag to pull", "width": 220 },
      "btn_pull":    { "type": "Button", "label": "Pull" },
      "btn_prune":   { "type": "Button", "label": "Prune dangling..." }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##images_table", "columns": 6,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_repo":    { "type": "TableColumn", "label": "Repository", "flags": "WidthStretch",                    "column_id": 0 },
        "col_tag":     { "type": "TableColumn", "label": "Tag",        "flags": "WidthFixed", "init_width": 110, "column_id": 1 },
        "col_id":      { "type": "TableColumn", "label": "Image ID",   "flags": "WidthFixed", "init_width": 140, "column_id": 2 },
        "col_created": { "type": "TableColumn", "label": "Created",    "flags": "WidthFixed", "init_width": 120, "column_id": 3 },
        "col_size":    { "type": "TableColumn", "label": "Size",       "flags": "WidthFixed", "init_width": 90,  "column_id": 4 },
        "col_actions": { "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 5 }
      }
    }
  } } }
})json";

static constexpr const char* kVolumesLayout = R"json({
  "type": "Window", "title": "Volumes", "width": 720, "height": 300,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh" },
      "vol_name":    { "type": "InputText", "hint": "new volume name", "width": 200 },
      "btn_create":  { "type": "Button", "label": "Create" },
      "btn_prune":   { "type": "Button", "label": "Prune..." }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##volumes_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":   { "type": "TableColumn", "label": "Name",       "flags": "WidthFixed", "init_width": 220, "column_id": 0 },
        "col_driver": { "type": "TableColumn", "label": "Driver",     "flags": "WidthFixed", "init_width": 90,  "column_id": 1 },
        "col_mount":  { "type": "TableColumn", "label": "Mountpoint", "flags": "WidthStretch",                    "column_id": 2 },
        "col_actions":{ "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 3 }
      }
    }
  } } }
})json";

static constexpr const char* kNetworksLayout = R"json({
  "type": "Window", "title": "Networks", "width": 720, "height": 300,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn_refresh": { "type": "Button", "label": "Refresh" },
      "btn_prune":   { "type": "Button", "label": "Prune..." }
    } },
    "status": { "type": "Label", "text": "" },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##networks_table", "columns": 5,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":   { "type": "TableColumn", "label": "Name",       "flags": "WidthFixed", "init_width": 200, "column_id": 0 },
        "col_driver": { "type": "TableColumn", "label": "Driver",     "flags": "WidthFixed", "init_width": 100, "column_id": 1 },
        "col_scope":  { "type": "TableColumn", "label": "Scope",      "flags": "WidthFixed", "init_width": 100, "column_id": 2 },
        "col_id":     { "type": "TableColumn", "label": "Network ID", "flags": "WidthStretch",                    "column_id": 3 },
        "col_actions":{ "type": "TableColumn", "label": "",           "flags": "WidthFixed", "init_width": 40,  "column_id": 4 }
      }
    }
  } } }
})json";

// Logs is common::text_viewer_panel's toolbar + read-only TextEditor with
// Follow / Lines controls added ("auto_scroll": true so it follows the
// newest line as `docker logs` output arrives). Inspect uses the panel's
// default layout.

static constexpr const char* kLogsLayout = R"json({
  "type": "Window", "title": "Logs", "width": 900, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":     { "type": "Label", "text": "(no container selected)" },
      "spring":     { "type": "Spring" },
      "follow":     { "type": "Checkbox", "label": "Follow", "value": false },
      "lines":      { "type": "InputInt", "label": "Lines", "value": 500, "step": 100, "width": 130 },
      "btn_refresh":{ "type": "Button", "label": "Refresh" }
    } },
    "sep": { "type": "Separator" },
    "editor": {
      "type": "TextEditor", "file_path": "", "language": "log", "read_only": true,
      "auto_scroll": true, "width": -1, "height": -1
    }
  } } }
})json";

// The Stats window: two `top`-style rolling Plots (CPU %, Memory %) fed one
// sample per update_stats call by docker_source's background poll thread,
// plus a current-values Table. Each Plot starts with an empty children map;
// build_stats_window() creates the aggregate "Total" line and per-container
// lines are added/removed at runtime by update_stats_plot().

static constexpr const char* kStatsLayout = R"json({
  "type": "Window", "title": "Stats", "width": 940, "height": 800,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "status": { "type": "Label", "text": "" },
    "cpu_plot": {
      "type": "Plot", "title": "CPU %  (per container)", "height": 300,
      "profiler_marker": "Docker CPU Plot", "y_label": "%", "children": {}
    },
    "mem_plot": {
      "type": "Plot", "title": "Memory %  (per container)", "height": 300,
      "profiler_marker": "Docker Mem Plot", "y_label": "%", "children": {}
    },
    "sep": { "type": "Separator" },
    "table": {
      "type": "Table", "id": "##docker_stats_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": false,
      "children": {
        "col_name":  { "type": "TableColumn", "label": "Name",      "flags": "WidthStretch",                  "column_id": 0 },
        "col_cpu":   { "type": "TableColumn", "label": "CPU %",     "flags": "WidthFixed", "init_width": 90,  "column_id": 1 },
        "col_mem":   { "type": "TableColumn", "label": "Mem %",     "flags": "WidthFixed", "init_width": 90,  "column_id": 2 },
        "col_usage": { "type": "TableColumn", "label": "Mem Usage", "flags": "WidthFixed", "init_width": 190, "column_id": 3 }
      }
    }
  } } }
})json";

} // namespace

// ── docker_frontend ────────────────────────────────────────────────────────

docker_frontend::docker_frontend(dynamic&& base) : tool_form(std::move(base)) {}

void docker_frontend::on_init() {
  internal_root_key_ = next_available_key("__docker_");

  auto* title_f = findField<std::string>("title"_key);
  title_ = title_f ? *title_f : std::string{"Docker"};

  // Containers is the main root -- form::init() registers internal_root_key_
  // as this form's top-level object automatically; the other windows
  // register themselves (tool_form::build_window()).
  build_list_window(
      containers_,
      kContainersLayout,
      internal_root_key_,
      "containers",
      "Remove all stopped containers (docker container prune)?",
      [&](ui_tree& tree) {
        tree.with("vbox.toolbar.filter", [&](const auto& e) { filter_input_id_ = wish_id_of(e); });
        tree.with("vbox.toolbar.state", [&](const auto& e) { state_combo_id_ = wish_id_of(e); });
      });

  build_list_window(
      images_,
      kImagesLayout,
      internal_root_key_ + "_images",
      "images",
      "Remove all dangling images (docker image prune)?",
      [&](ui_tree& tree) {
        tree.with("vbox.toolbar.pull_ref", [&](const auto& e) {
          pull_ref_input_ = e;
          pull_ref_input_id_ = wish_id_of(e);
        });
        tree.with("vbox.toolbar.btn_pull", [&](const auto& e) {
          on_click(wish_id_of(e), [this] {
            if (pull_ref_text_.empty())
              return;
            emit("pull_image_requested"_key, make_payload("ref"_key, pull_ref_text_));
            pull_ref_text_.clear();
            if (pull_ref_input_)
              pull_ref_input_["value"_key] = std::string{};
          });
        });
      });

  build_list_window(
      volumes_,
      kVolumesLayout,
      internal_root_key_ + "_volumes",
      "volumes",
      "Remove all unused local volumes (docker volume prune)?",
      [&](ui_tree& tree) {
        tree.with("vbox.toolbar.vol_name", [&](const auto& e) {
          volume_name_input_ = e;
          volume_name_input_id_ = wish_id_of(e);
        });
        tree.with("vbox.toolbar.btn_create", [&](const auto& e) {
          on_click(wish_id_of(e), [this] {
            if (volume_name_text_.empty())
              return;
            emit("create_volume_requested"_key, make_payload("name"_key, volume_name_text_));
            volume_name_text_.clear();
            if (volume_name_input_)
              volume_name_input_["value"_key] = std::string{};
          });
        });
      });

  build_list_window(
      networks_,
      kNetworksLayout,
      internal_root_key_ + "_networks",
      "networks",
      "Remove all unused networks (docker network prune)?");

  common::text_viewer_options logs_options;
  logs_options.layout_json = kLogsLayout;
  logs_options.file_stem = "logs";
  logs_options.on_refresh = [this] { emit_logs_request(); };
  logs_.build(*this, internal_root_key_ + "_logs", std::move(logs_options), [&](ui_tree& tree) {
    tree.with("vbox.toolbar.follow", [&](const auto& e) { logs_follow_id_ = wish_id_of(e); });
    tree.with("vbox.toolbar.lines", [&](const auto& e) { logs_lines_id_ = wish_id_of(e); });
  });

  inspect_.build(
      *this,
      internal_root_key_ + "_inspect",
      {.title = "Inspect", .language = "json", .file_stem = "inspect", .on_refresh = [this] {
         emit_inspect_request();
       }});

  console_.build(*this, internal_root_key_ + "_console", {.table_id = "##docker_console_table"});

  stats_root_key_ = internal_root_key_ + "_stats";
  build_stats_window();

  // Seed the first-run arrangement (see the "Window layouts" comment above):
  // a wide left column of tabbed list/stats windows over a Console strip, and
  // a narrower right column with Logs + Inspect. Owned by imgui.ini after the
  // first run; bump the version arg to layout() if this arrangement changes.
  {
    using namespace dock;
    // split(dir, ratio, near, far): `near` is the pane on the `dir` side and
    // takes `ratio` of the space. So: carve the left 62% off for a column
    // that is itself split -- a Console strip along its bottom 24%, the
    // tabbed list/stats windows filling the rest -- and leave the right 38%
    // for Logs + Inspect.
    set_default_dock_layout(viewport(
        "docker_dock", "Docker",
        layout(
            split(
                dir::left, 0.62f,
                split(
                    dir::down,
                    0.24f,
                    area({console_.root_key()}),
                    area(
                        {internal_root_key_,
                         images_.root_key(),
                         volumes_.root_key(),
                         networks_.root_key(),
                         stats_root_key_},
                        internal_root_key_)),
                area({logs_.root_key(), inspect_.root_key()}, logs_.root_key())),
            /*version=*/1,
            /*target=*/"docker_dock")));
  }

  // Initial population is triggered client-side (run_docker() calls
  // source->refresh_all() after wiring every handler) -- never via an
  // on_init()-emitted event (git's documented initial-load-race fix).
}

void docker_frontend::build_list_window(
    list_window& lw,
    const char* layout_json,
    const std::string& root_key,
    const std::string& prune_scope,
    const std::string& prune_message,
    const std::function<void(ui_tree&)>& wire) {
  lw.build(*this, root_key, layout_json, [&](ui_tree& tree) {
    tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
      on_click(wish_id_of(e), [this] { emit("refresh_requested"_key); });
    });
    tree.with("vbox.toolbar.btn_prune", [&](const auto& e) {
      on_click(wish_id_of(e), [this, prune_scope, prune_message] {
        show_confirm(prune_message, [this, prune_scope] {
          emit("prune_requested"_key, make_payload("scope"_key, prune_scope));
        });
      });
    });
    if (wire)
      wire(tree);
  });
}

void docker_frontend::emit_logs_request() {
  if (open_logs_id_.empty())
    return;
  emit(
      "logs_requested"_key,
      make_payload("id"_key, open_logs_id_, "follow"_key, logs_follow_, "lines"_key, logs_lines_));
}

void docker_frontend::emit_inspect_request() {
  if (open_inspect_id_.empty())
    return;
  emit("inspect_requested"_key, make_payload("kind"_key, open_inspect_kind_, "id"_key, open_inspect_id_));
}

common::menu_item
docker_frontend::action_item(const std::string& label, const entity& e, const std::string& action, bool confirm) {
  return {label, [this, e, action] { run_row_action(e, action); }, confirm};
}

// ── Per-window rebuild ─────────────────────────────────────────────────────

void docker_frontend::rebuild_containers(const dynamic& args) {
  containers_.clear();

  int running = 0, total = 0;
  for_each_entry(args, "containers"_key, [&](const dynamic& e) {
    const std::string state = e.as<std::string>("state"_key);
    entity c{
        "container", e.as<std::string>("id"_key), e.as<std::string>("name"_key), e.as<std::string>("image"_key), state};

    std::vector<ui_element_ptr> cells = {
        make_label(c.name),
        make_label(c.extra),
        make_label(e.as<std::string>("status"_key), state_colour(state)),
        make_label(e.as<std::string>("ports"_key)),
        make_label(e.as<std::string>("created"_key), kIdle),
    };

    std::vector<common::menu_item> items;
    if (state == "running")
      items = {
          action_item("Stop", c, "stop", true),
          action_item("Restart", c, "restart", false),
          action_item("Pause", c, "pause", false),
          action_item("Kill", c, "kill", true)};
    else if (state == "paused")
      items = {
          action_item("Unpause", c, "unpause", false),
          action_item("Stop", c, "stop", true),
          action_item("Kill", c, "kill", true)};
    else
      items = {action_item("Start", c, "start", false)};
    items.push_back({});
    items.push_back(action_item("Logs", c, "logs", false));
    items.push_back(action_item("Inspect", c, "inspect", false));
    items.push_back({});
    items.push_back(action_item("Remove", c, "remove", true));

    containers_.add(std::move(c), cells, items);
    ++total;
    if (state == "running")
      ++running;
  });

  containers_.refresh();
  apply_container_filter();
  containers_.set_status(
      std::to_string(total) + (total == 1 ? " container (" : " containers (") + std::to_string(running) + " running)",
      true);
}

void docker_frontend::rebuild_images(const dynamic& args) {
  images_.clear();

  int total = 0;
  for_each_entry(args, "images"_key, [&](const dynamic& e) {
    const std::string repo = e.as<std::string>("repository"_key);
    const std::string tag = e.as<std::string>("tag"_key);
    const bool dangling = repo == "<none>" || repo.empty();

    entity img;
    img.scope = "image";
    img.key = e.as<std::string>("id"_key);
    img.name = dangling ? img.key.substr(0, 19) : repo + ":" + tag;

    std::vector<ui_element_ptr> cells = {
        dangling ? make_label(repo, kIdle) : make_label(repo),
        dangling ? make_label(tag, kIdle) : make_label(tag),
        make_label(img.key.substr(0, 19), kIdle),
        make_label(e.as<std::string>("created"_key), kIdle),
        make_label(e.as<std::string>("size"_key)),
    };
    std::vector<common::menu_item> items = {
        action_item("Run", img, "run", false),
        action_item("Inspect", img, "inspect", false),
        {},
        action_item("Remove", img, "remove", true)};
    images_.add(std::move(img), cells, items);
    ++total;
  });

  images_.refresh();
  images_.set_status(std::to_string(total) + (total == 1 ? " image" : " images"), true);
}

void docker_frontend::rebuild_volumes(const dynamic& args) {
  volumes_.clear();

  int total = 0;
  for_each_entry(args, "volumes"_key, [&](const dynamic& e) {
    entity vol;
    vol.scope = "volume";
    vol.key = e.as<std::string>("name"_key);
    vol.name = vol.key;

    std::vector<ui_element_ptr> cells = {
        make_label(vol.key),
        make_label(e.as<std::string>("driver"_key)),
        make_label(e.as<std::string>("mountpoint"_key), kIdle),
    };
    std::vector<common::menu_item> items = {
        action_item("Inspect", vol, "inspect", false), {}, action_item("Remove", vol, "remove", true)};
    volumes_.add(std::move(vol), cells, items);
    ++total;
  });

  volumes_.refresh();
  volumes_.set_status(std::to_string(total) + (total == 1 ? " volume" : " volumes"), true);
}

void docker_frontend::rebuild_networks(const dynamic& args) {
  networks_.clear();

  int total = 0;
  for_each_entry(args, "networks"_key, [&](const dynamic& e) {
    entity net;
    net.scope = "network";
    net.key = e.as<std::string>("id"_key);
    net.name = e.as<std::string>("name"_key);
    const bool builtin = net.name == "bridge" || net.name == "host" || net.name == "none";

    std::vector<ui_element_ptr> cells = {
        make_label(net.name),
        make_label(e.as<std::string>("driver"_key)),
        make_label(e.as<std::string>("scope"_key)),
        make_label(net.key.substr(0, 12), kIdle),
    };
    // The three built-in networks can't be removed -- offer Inspect only.
    std::vector<common::menu_item> items = {action_item("Inspect", net, "inspect", false)};
    if (!builtin) {
      items.push_back({});
      items.push_back(action_item("Remove", net, "remove", true));
    }
    networks_.add(std::move(net), cells, items);
    ++total;
  });

  networks_.refresh();
  networks_.set_status(std::to_string(total) + (total == 1 ? " network" : " networks"), true);
}

void docker_frontend::apply_container_filter() {
  const std::string needle = common::lower(filter_text_);
  containers_.apply_filter([&](const entity& c) {
    if (state_filter_ == 1 && c.state != "running")
      return false;
    if (state_filter_ == 2 && c.state == "running")
      return false;
    return needle.empty() || common::lower(c.name + " " + c.extra).find(needle) != std::string::npos;
  });
}

// ── RMI methods ────────────────────────────────────────────────────────────

dynamic docker_frontend::do_update_containers(const dynamic& args) {
  rebuild_containers(args);
  return dynamic{};
}
dynamic docker_frontend::do_update_images(const dynamic& args) {
  rebuild_images(args);
  return dynamic{};
}
dynamic docker_frontend::do_update_volumes(const dynamic& args) {
  rebuild_volumes(args);
  return dynamic{};
}
dynamic docker_frontend::do_update_networks(const dynamic& args) {
  rebuild_networks(args);
  return dynamic{};
}

dynamic docker_frontend::do_update_logs(const dynamic& args) {
  if (args.as<std::string>("container_id"_key) != open_logs_id_)
    return dynamic{}; // stale response for a container the user navigated away from.
  logs_.set_title(args.as<std::string>("title"_key));
  logs_.set_text(args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic docker_frontend::do_update_inspect(const dynamic& args) {
  if (args.as<std::string>("target_id"_key) != open_inspect_id_)
    return dynamic{};
  inspect_.set_title(args.as<std::string>("title"_key));
  inspect_.set_text(args.as<std::string>("text"_key));
  return dynamic{};
}

dynamic docker_frontend::do_command_result(const dynamic& args) {
  const std::string scope = common::str_of(args, "scope"_key);
  list_window& lw = scope == "images" ? images_
      : scope == "volumes"            ? volumes_
      : scope == "networks"           ? networks_
                                      : containers_;
  bool ok = false;
  const std::string text = common::command_result_text(args, ok);
  lw.set_status(text, ok);
  return dynamic{};
}

dynamic docker_frontend::do_append_command_log(const dynamic& args) {
  console_.append_from(args);
  return dynamic{};
}

// ── Stats window (live `docker stats` graphs) ──────────────────────────────

void docker_frontend::build_stats_window() {
  build_window(stats_root_key_, kStatsLayout, stats_window_id_, [&](ui_tree& tree) {
    using y_axis = common::rolling_plot::y_axis;
    tree.with("vbox.status", [&](const auto& e) { stats_status_label_ = e; });
    tree.with("vbox.table", [&](const auto& e) { stats_table_.attach(*this, e); });
    // Y auto-fits on the CPU plot (a busy multi-core container can exceed
    // 100 %); the memory plot is a true 0..100 % gauge.
    tree.with("vbox.cpu_plot", [&](const auto& e) {
      common::rolling_plot::configure_axes(e, y_axis::auto_fit);
      stats_cpu_.init(*this, e, "Total");
    });
    tree.with("vbox.mem_plot", [&](const auto& e) {
      common::rolling_plot::configure_axes(e, y_axis::percent);
      stats_mem_.init(*this, e, "Total");
    });
  });
}

void docker_frontend::rebuild_stats_table(const dynamic& args) {
  stats_table_.clear();
  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    stats_table_.add(
        {},
        {
            make_label(e.as<std::string>("name"_key)),
            make_label(format_percent(e.as<float>("cpu_percent"_key))),
            make_label(format_percent(e.as<float>("mem_percent"_key))),
            make_label(e.as<std::string>("mem_usage"_key)),
        });
  });
  stats_table_.refresh();
}

dynamic docker_frontend::do_update_stats(const dynamic& args) {
  std::map<std::string, float> cpu;
  std::map<std::string, float> mem;
  size_t count = 0;
  for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    const std::string name = e.as<std::string>("name"_key);
    cpu[name] = e.as<float>("cpu_percent"_key);
    mem[name] = e.as<float>("mem_percent"_key);
    ++count;
  });

  stats_cpu_.push(cpu);
  stats_mem_.push(mem);
  rebuild_stats_table(args);

  const std::string error = common::str_of(args, "error"_key);
  if (!error.empty())
    common::set_status_text(stats_status_label_, "docker stats unavailable: " + error, false);
  else
    common::set_status_text(
        stats_status_label_, std::to_string(count) + (count == 1 ? " running container" : " running containers"), true);
  return dynamic{};
}

// ── Event routing ──────────────────────────────────────────────────────────

void docker_frontend::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any window's X button -> tear everything down.
  if (event == "closed"_key &&
      (id == containers_.window_id() || id == images_.window_id() || id == volumes_.window_id() ||
       id == networks_.window_id() || id == logs_.window_id() || id == inspect_.window_id() ||
       id == console_.window_id() || id == stats_window_id_)) {
    logs_.remove_file();
    inspect_.remove_file();
    emit("closed"_key);
    remove_objects_at(images_.root_key());
    remove_objects_at(volumes_.root_key());
    remove_objects_at(networks_.root_key());
    remove_objects_at(logs_.root_key());
    remove_objects_at(inspect_.root_key());
    remove_objects_at(console_.root_key());
    remove_objects_at(stats_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "changed"_key) {
    if (id == filter_input_id_) {
      filter_text_ = payload.as<std::string>("value"_key);
      apply_container_filter();
    } else if (id == state_combo_id_) {
      state_filter_ = payload.as<int32_t>("value"_key);
      apply_container_filter();
    } else if (id == pull_ref_input_id_) {
      pull_ref_text_ = payload.as<std::string>("value"_key);
    } else if (id == volume_name_input_id_) {
      volume_name_text_ = payload.as<std::string>("value"_key);
    } else if (id == logs_follow_id_) {
      logs_follow_ = payload.as<bool>("value"_key);
      emit_logs_request();
    } else if (id == logs_lines_id_) {
      logs_lines_ = payload.as<int32_t>("value"_key);
    }
    return;
  }

  if (event == "clicked"_key)
    dispatch_click(id);
}

void docker_frontend::run_row_action(const entity& e, const std::string& action) {
  const std::string display_name = e.name.empty() ? e.key : e.name;

  if (action == "logs") {
    open_logs_id_ = e.key;
    logs_.set_title(display_name);
    emit_logs_request();
    return;
  }
  if (action == "inspect") {
    open_inspect_id_ = e.key;
    open_inspect_kind_ = e.scope;
    inspect_.set_title(e.scope + ": " + display_name);
    emit_inspect_request();
    return;
  }

  const key_t event_name = e.scope == "container" ? "container_action_requested"_key
      : e.scope == "image"                        ? "image_action_requested"_key
      : e.scope == "volume"                       ? "volume_action_requested"_key
                                                  : "network_action_requested"_key;
  const key_t id_field = e.scope == "volume" ? "name"_key : "id"_key;
  auto fire = [this, event_name, id_field, key = e.key, action] {
    emit(event_name, make_payload(id_field, key, "action"_key, action));
  };

  const bool destructive = action == "stop" || action == "kill" || action == "remove";
  if (!destructive) {
    fire();
    return;
  }

  std::string verb = action == "stop" ? "Stop" : action == "kill" ? "Kill" : "Remove";
  show_confirm(verb + " " + e.scope + " '" + display_name + "'?", fire);
}

// ── Registration ───────────────────────────────────────────────────────────

void register_docker() {
  auto proto = dynamic_ptr{"DockerFrontend"_key, {}};

  proto->addField("title"_key, field{std::string{"Docker"}});

  auto add_method = [&](key_t name, dynamic (docker_frontend::*fn)(const dynamic&)) {
    proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                       return (static_cast<docker_frontend&>(self).*fn)(args);
                     }});
  };
  add_method("update_containers"_key, &docker_frontend::do_update_containers);
  add_method("update_images"_key, &docker_frontend::do_update_images);
  add_method("update_volumes"_key, &docker_frontend::do_update_volumes);
  add_method("update_networks"_key, &docker_frontend::do_update_networks);
  add_method("update_logs"_key, &docker_frontend::do_update_logs);
  add_method("update_inspect"_key, &docker_frontend::do_update_inspect);
  add_method("command_result"_key, &docker_frontend::do_command_result);
  add_method("append_command_log"_key, &docker_frontend::do_append_command_log);
  add_method("update_stats"_key, &docker_frontend::do_update_stats);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockerFrontend"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Docker Desktop-style GUI frontend for the local `docker` CLI. All `docker` invocation happens "
      "client-side; this form only renders whatever snapshot it was last given. Listen for the 'closed' "
      "event to detect when the user is done, and the '*_requested' events to react to user actions -- "
      "see docker.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<docker_frontend>("wish"_key, "DockerFrontend"_key));
}

} // namespace bdg::wish
