// MIT License © 2025 Binary Dice Games
/// @file mc.cpp
/// @brief Implementation of the mc form.
#include "mc.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"
#include "ui/forms/file_browser_utils.hpp"
#include "ui/forms/properties_dialog.hpp"

#include <context/file_service.hpp>
#include <ui/dock_layout_spec.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace bdg::wish {

using namespace bison;
using common::set_children_list;
namespace fs = std::filesystem;

// open_in_host_explorer() lives in file_browser_utils.hpp/.cpp -- shared
// with PixViewer's own "Open in Explorer" button. format_bytes()/
// format_modified() are deliberately still duplicated per module (see
// zip.cpp's own format_bytes() doc comment).
namespace {

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

// Pre-C++20-portable file_time_type -> calendar string conversion (no
// std::chrono::clock_cast, whose libstdc++ availability lags MSVC's).
std::string format_modified(const fs::file_time_type& ftime) {
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M");
  return oss.str();
}

bool is_separator(char c) {
  return c == '/' || c == '\\';
}

std::string_view strip_trailing_separators(std::string_view path) {
  while (!path.empty() && is_separator(path.back()))
    path.remove_suffix(1);
  return path;
}

enum class path_relation { unrelated, same, ancestor };

// How tree node path @p node relates to @p target, comparing as text with
// '/' and '\\' interchangeable and trailing separators ignored. The local
// tree holds paths of the *client's* machine, which may not share this
// machine's path syntax, so std::filesystem is deliberately not used.
// @p node_is_sandbox_root marks the sandbox's "" root, an ancestor of every
// relative path.
path_relation relate_paths(std::string_view node, std::string_view target, bool node_is_sandbox_root) {
  node = strip_trailing_separators(node);
  target = strip_trailing_separators(target);
  if (target.size() < node.size())
    return path_relation::unrelated;
  for (size_t i = 0; i < node.size(); ++i)
    if (node[i] != target[i] && !(is_separator(node[i]) && is_separator(target[i])))
      return path_relation::unrelated;
  if (target.size() == node.size())
    return path_relation::same;
  return node_is_sandbox_root || is_separator(target[node.size()]) ? path_relation::ancestor
                                                                    : path_relation::unrelated;
}

// Path of directory @p name inside local directory @p parent, using the
// separator @p parent itself uses.
std::string join_local_path(const std::string& parent, const std::string& name) {
  if (is_separator(parent.back()))
    return parent + name;
  bool backslash = parent.find('\\') != std::string::npos && parent.find('/') == std::string::npos;
  return parent + (backslash ? '\\' : '/') + name;
}

} // namespace

// ── UI layouts ────────────────────────────────────────────────────────────────
//
// The browser is split into four dockable panels -- Local and Sandbox, each
// with its folder tree (kLocalTreeLayout/kSandboxTreeLayout) above it --
// seeded into a first-run arrangement by on_init()'s
// set_default_dock_layout() call, the same multi-window pattern top, pix and
// the dev modules (git, curl, docker) use. The user can re-dock, tab or
// float any of them; imgui.ini owns the arrangement after the first run.
// Each Window keeps a width/height: the size a panel restores to when
// dragged out of the dock.
//
// Local (the form's main root, internal_root_key_) and Sandbox share one
// shape: a path bar, an action row (the button that sends the selected files
// to the *other* panel -- Upload in Local, Download in Sandbox -- next to the
// "Selected: ..." label), the file table, a two-line summary strip and a
// status line. Rows are built at runtime by fill_table().
// left_table/right_table's "flags" names the same full-border set
// git.cpp/tail.cpp use for their own grid-style tables, plus Sortable.
// Unlike FileDialog's borderless picker list (Resizable|RowBg|BordersH|
// Sortable|ScrollY -- no BordersV), the outer vertical border frames each
// table against its panel edge, so RowBg's alternating shading doesn't read
// as cropped. col_name/col_size/col_modified's "column_id" (0/1/2) is echoed
// back in each Table's "sorted" event payload -- see on_table_sorted()'s doc
// comment. col_modified is wide enough for the whole "YYYY-MM-DD HH:MM" stamp
// at the default font; narrower, it is cut off right where the table's
// scrollbar starts, which reads as the scrollbar covering the column. (Column
// widths are saved in imgui.ini per table "id", so changing a default width
// only reaches existing users if the id changes too.)
// "auto_scroll" is off: a listing is read from the top, not followed like a
// log. InputText EnterReturnsTrue so path bars only fire "changed" on
// Enter, not per keystroke.
//
// left_table/right_table carry "height": -1: "vbox" hands every other child
// its natural height first, then gives the table whatever's left, so only
// the listing scrolls (ScrollY engages per-panel only when a listing doesn't
// fit). "left_stats"/"left_disk" (and their "right_" twins) are auto-height
// Labels added *after* the table, so they read as a small summary strip
// pinned to the bottom of each panel. "_stats" holds "<N> files, <size>" for
// the currently-listed directory (non-recursive); "_disk" holds the
// used/free/total space of the filesystem that directory lives on. Both
// start blank and are filled in by do_update_local_listing()
// (client-reported, since only the client can see the local machine's disk)
// and navigate_sandbox() (computed directly via std::filesystem, since the
// sandbox lives on this machine). "_status" shows the latest message about
// that panel (see set_status()).
//
// Transfers show their progress in the shared ProgressBox dialog the
// client's command_worker drives (modules/bdg/common/command_worker.hpp), like
// every long-running action in the dev modules -- no progress bar here.
//
// Tagged delimiter (R"json(...)json") rather than the untagged R"(...)"
// convention used elsewhere: "Sandbox (Server)" ends in a ")" immediately
// followed by the JSON string's closing quote, which is exactly the byte
// sequence R"(...)" treats as its own terminator -- an untagged literal
// would truncate here.

static constexpr const char* kLocalLayout = R"json({
  "type": "Window",
  "title": "Local Machine",
  "width": 460, "height": 480,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "spacing": 4,
      "children": {
        "left_path": {
          "type": "InputText", "hint": "Local path...", "value": "",
          "flags": "EnterReturnsTrue", "width": -1
        },
        "left_actions": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "children": {
            "upload": {
              "type": "Button", "label": "Upload", "icon": "res/icons/upload.png",
              "tooltip": "Copy the selected files to the sandbox folder shown in the Sandbox panel"
            },
            "left_selected": { "type": "Label", "text": "Selected: (none)" }
          }
        },
        "left_table": {
          "type": "Table", "id": "##local_files", "columns": 3, "headers": true,
          "flags": "Resizable|RowBg|Borders|Sortable|ScrollY",
          "outer_width": 0, "height": -1, "auto_scroll": false,
          "children": {
            "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
            "col_size":     { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 90, "column_id": 1 },
            "col_modified": { "type": "TableColumn", "label": "Modified", "flags": "WidthFixed", "init_width": 160, "column_id": 2 }
          }
        },
        "left_stats": { "type": "Label", "text": "" },
        "left_disk":  { "type": "Label", "text": "" },
        "left_status": { "type": "Label", "text": "" }
      }
    }
  }
})json";

static constexpr const char* kSandboxLayout = R"json({
  "type": "Window",
  "title": "Sandbox (Server)",
  "width": 460, "height": 480,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "spacing": 4,
      "children": {
        "right_toolbar": {
          "type": "HorizontalLayout",
          "spacing": 6,
          "children": {
            "right_path": {
              "type": "InputText", "hint": "Sandbox path...", "value": "/",
              "flags": "EnterReturnsTrue", "width": -1
            },
            "open_explorer": { "type": "Button", "label": "Open in Explorer", "icon": "res/icons/open_in_new.png" }
          }
        },
        "right_actions": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "children": {
            "download": {
              "type": "Button", "label": "Download", "icon": "res/icons/download.png",
              "tooltip": "Copy the selected files to the local folder shown in the Local Machine panel"
            },
            "right_selected": { "type": "Label", "text": "Selected: (none)" }
          }
        },
        "right_table": {
          "type": "Table", "id": "##sandbox_files", "columns": 3, "headers": true,
          "flags": "Resizable|RowBg|Borders|Sortable|ScrollY",
          "outer_width": 0, "height": -1, "auto_scroll": false,
          "children": {
            "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
            "col_size":     { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 90, "column_id": 1 },
            "col_modified": { "type": "TableColumn", "label": "Modified", "flags": "WidthFixed", "init_width": 160, "column_id": 2 }
          }
        },
        "right_stats": { "type": "Label", "text": "" },
        "right_disk":  { "type": "Label", "text": "" },
        "right_status": { "type": "Label", "text": "Ready." }
      }
    }
  }
})json";

// Folder trees -- one Window each, so the user can resize, re-dock or float
// them like the file panels. "tree" starts empty: its TreeNodes are created at
// runtime, one level at a time (see set_tree_children()). It is the scroll
// region itself ("scroll"), not the Window: a TreeNode scrolls the region it
// is drawn in when it becomes selected, which is how a revealed folder comes
// into view.
static constexpr const char* kLocalTreeLayout = R"json({
  "type": "Window",
  "title": "Local Folders",
  "width": 240, "height": 480,
  "closable": true,
  "children": {
    "tree": { "type": "VerticalLayout", "spacing": 0, "scroll": true, "children": {} }
  }
})json";

static constexpr const char* kSandboxTreeLayout = R"json({
  "type": "Window",
  "title": "Sandbox Folders",
  "width": 240, "height": 480,
  "closable": true,
  "children": {
    "tree": { "type": "VerticalLayout", "spacing": 0, "scroll": true, "children": {} }
  }
})json";

// Rename dialog -- shared by both panels; show_rename_dialog() fills in
// "message" and prefills "new_name" with the current name. EnterReturnsTrue
// lets the user just type-and-press-Enter instead of reaching for "Rename".
// Mirrors top.cpp's confirm-kill dialog: a small internal Window merged as
// its own top-level object, closed via the __request_close__/closed
// handshake (see form.hpp's request_close_at()).
//
// Same resizable-window / stretch-content-pinning-the-footer style as
// kPropertiesLayout below (see that constant's own doc comment for why the
// stretch hint needs a VerticalLayout parent to read it from): "content"
// wraps message/new_name with "height": -1 so it -- not sep/buttons --
// absorbs any extra height the user resizes the window to.
static constexpr const char* kRenameLayout = R"json({
  "type": "Window", "title": "Rename", "modal": true,
  "flags": "NoCollapse",
  "width": 400, "height": 160,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "content": {
          "type": "VerticalLayout",
          "height": -1,
          "children": {
            "message": { "type": "Label", "text": "" },
            "new_name": { "type": "InputText", "value": "", "flags": "EnterReturnsTrue", "width": 300 }
          }
        },
        "sep": { "type": "Separator" },
        "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
          "btn_ok": { "type": "Button", "label": "Rename", "height": 32 },
          "btn_cancel": { "type": "Button", "label": "Cancel", "height": 32 }
        } }
      }
    }
  }
})json";

// Properties is a privately-instantiated PropertiesDialog (see
// form::instantiate_child_form()) reflecting over a FileProperties instance
// (see register_file_properties_class()) rather than a raw element tree --
// every field is already known server-side by show-time (see mc.hpp's class
// doc comment), unlike top.cpp's Properties dialog which has to wait on a
// client round trip, so show_properties_dialog() below never needs to
// retarget an already-open instance.

// ── mc ─────────────────────────────────────────────────────────────

mc::mc(dynamic&& base) : tool_form(std::move(base)) {}

mc::~mc() {
  remove_panel_objects();
}

void mc::on_init() {
  internal_root_key_ = next_available_key("__mc_");
  sandbox_root_key_ = internal_root_key_ + "_sandbox";
  local_tree_root_key_ = internal_root_key_ + "_local_tree";
  sandbox_tree_root_key_ = internal_root_key_ + "_sandbox_tree";

  auto* title_f = findField<std::string>("title"_key);
  const std::string title = title_f ? *title_f : std::string{"File Explorer"};

  build_window(internal_root_key_, kLocalLayout, window_id_, [&](ui_tree& tree) {
    tree.with("vbox.left_path", [&](const auto& e) {
      left_path_ptr_ = e;
      left_path_id_ = wish_id_of(e);
    });
    tree.with("vbox.left_table", [&](const auto& e) {
      left_table_ptr_ = e;
      left_table_id_ = wish_id_of(e);
    });
    tree.with("vbox.left_actions.upload", [&](const auto& e) { upload_id_ = wish_id_of(e); });
    tree.with("vbox.left_actions.left_selected", [&](const auto& e) { left_selected_ptr_ = e; });
    tree.with("vbox.left_stats", [&](const auto& e) { left_stats_ptr_ = e; });
    tree.with("vbox.left_disk", [&](const auto& e) { left_disk_ptr_ = e; });
    tree.with("vbox.left_status", [&](const auto& e) { left_status_ptr_ = e; });
  });

  build_window(sandbox_root_key_, kSandboxLayout, sandbox_window_id_, [&](ui_tree& tree) {
    tree.with("vbox.right_toolbar.right_path", [&](const auto& e) {
      right_path_ptr_ = e;
      right_path_id_ = wish_id_of(e);
    });
    tree.with("vbox.right_toolbar.open_explorer", [&](const auto& e) { open_explorer_id_ = wish_id_of(e); });
    tree.with("vbox.right_table", [&](const auto& e) {
      right_table_ptr_ = e;
      right_table_id_ = wish_id_of(e);
    });
    tree.with("vbox.right_actions.download", [&](const auto& e) { download_id_ = wish_id_of(e); });
    tree.with("vbox.right_actions.right_selected", [&](const auto& e) { right_selected_ptr_ = e; });
    tree.with("vbox.right_stats", [&](const auto& e) { right_stats_ptr_ = e; });
    tree.with("vbox.right_disk", [&](const auto& e) { right_disk_ptr_ = e; });
    tree.with("vbox.right_status", [&](const auto& e) { right_status_ptr_ = e; });
  });

  sandbox_tree_.is_sandbox = true;
  build_window(local_tree_root_key_, kLocalTreeLayout, local_tree_window_id_, [&](ui_tree& tree) {
    tree.with("tree", [&](const auto& e) { local_tree_.box = e; });
  });
  build_window(sandbox_tree_root_key_, kSandboxTreeLayout, sandbox_tree_window_id_, [&](ui_tree& tree) {
    tree.with("tree", [&](const auto& e) { sandbox_tree_.box = e; });
  });

  // Seed the first-run arrangement inside the browser's own nested
  // dockspace (titled with the form's "title" field): a 2x2 grid with the
  // folder trees on top and the file panels below, Local on the left and
  // Sandbox on the right. Owned by imgui.ini after the first run (see
  // docs/dock-layout.md); bump the version arg to layout() if it changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "mc_dock", title,
        layout(
            split(
                dir::up, 0.35f,
                split(dir::left, 0.50f, area({local_tree_root_key_}), area({sandbox_tree_root_key_})),
                split(dir::left, 0.50f, area({internal_root_key_}), area({sandbox_root_key_}))),
            /*version=*/3, /*target=*/"mc_dock")));
  }

  // The sandbox tree starts as its root with the first level under it; the
  // local tree's top level comes from the client (update_local_tree()).
  if (sandbox_tree_.box) {
    set_tree_children(sandbox_tree_, sandbox_tree_.root, {"/"});
    tree_node& sandbox_root = *sandbox_tree_.root.children.front();
    load_sandbox_tree_node(sandbox_root, sess().resource_dir, sess().allow_absolute_paths);
    sandbox_root.elem["open"_key] = true;
  }

  // Populate the sandbox panel immediately -- unlike the local panel, this
  // form has direct filesystem access to it, so no client round trip is
  // needed before the sandbox table shows something.
  navigate_sandbox("", sess().resource_dir, sess().allow_absolute_paths);
}

void mc::remove_panel_objects() {
  // The key is forgotten once removed: next_available_key() may hand this
  // form's freed internal_root_key_ to a new Mc instance, whose Sandbox panel
  // would then reuse this exact secondary key (see the same reasoning in
  // form::remove_objects_at()).
  remove_objects_at(sandbox_root_key_);
  sandbox_root_key_.clear();
  remove_objects_at(local_tree_root_key_);
  local_tree_root_key_.clear();
  remove_objects_at(sandbox_tree_root_key_);
  sandbox_tree_root_key_.clear();
}

// ── Folder trees ─────────────────────────────────────────────────────────────

void mc::set_tree_children(folder_tree& tree, tree_node& parent, std::vector<std::string> names) {
  std::sort(names.begin(), names.end(), ascii_ci_less);
  names.erase(std::unique(names.begin(), names.end()), names.end());

  const bool top_level = &parent == &tree.root;
  std::vector<std::unique_ptr<tree_node>> next;
  std::vector<ui_element_ptr> elems;
  for (auto& name : names) {
    auto old = std::find_if(parent.children.begin(), parent.children.end(), [&](const auto& c) {
      return c && c->name == name;
    });
    if (old != parent.children.end()) {
      next.push_back(std::move(*old));
    } else {
      auto node = std::make_unique<tree_node>();
      node->name = name;
      if (tree.is_sandbox)
        node->path = top_level ? std::string{} : (parent.path.empty() ? name : parent.path + "/" + name);
      else
        node->path = top_level ? name : join_local_path(parent.path, name);

      // open_on_arrow: a click on the name navigates, only the arrow expands.
      node->elem = ui_element_ptr::create("wish"_key, "TreeNode"_key);
      node->elem["label"_key] = name;
      node->elem["open_on_arrow"_key] = true;
      // Same folder icon the file tables show (see make_name_cell()).
      node->elem["icon"_key] = std::string{"res/icons/folder.png"};
      key_t id = rmi::shared::generate_id();
      ctx().put_object(id, node->elem);
      node->elem["__wish_id"_key] = id;
      tree.by_id[id] = node.get();
      next.push_back(std::move(node));
    }
    elems.push_back(next.back()->elem);
  }

  for (auto& gone : parent.children)
    if (gone)
      forget_tree_node(tree, *gone);
  parent.children = std::move(next);
  parent.loaded = true;
  parent.requested = false;

  if (const ui_element_ptr& host = top_level ? tree.box : parent.elem)
    set_children_list(host, elems);
  if (parent.elem)
    parent.elem["leaf"_key] = parent.children.empty();
}

void mc::forget_tree_node(folder_tree& tree, tree_node& node) {
  for (auto& child : node.children)
    forget_tree_node(tree, *child);
  if (tree.selected == &node)
    tree.selected = nullptr;
  key_t id = wish_id_of(node.elem);
  tree.by_id.erase(id);
  ctx().objects.erase(id.id);
}

mc::tree_node* mc::find_tree_node(folder_tree& tree, const std::string& path) {
  tree_node* node = &tree.root;
  for (;;) {
    tree_node* next = nullptr;
    for (auto& child : node->children) {
      auto relation = relate_paths(child->path, path, tree.is_sandbox && child->path.empty());
      if (relation == path_relation::same)
        return child.get();
      if (relation == path_relation::ancestor)
        next = child.get();
    }
    if (!next)
      return nullptr;
    node = next;
  }
}

void mc::select_tree_node(folder_tree& tree, tree_node* node) {
  if (tree.selected == node)
    return;
  if (tree.selected)
    tree.selected->elem["selected"_key] = false;
  tree.selected = node;
  if (node)
    node->elem["selected"_key] = true;
}

void mc::load_sandbox_tree_node(tree_node& node, const fs::path& resource_dir, bool allow_absolute_paths) {
  fs::path full = node.path.empty() ? resource_dir
                                    : file_service::resolve_path(node.path, resource_dir, allow_absolute_paths);
  std::vector<std::string> names;
  std::error_code ec;
  if (!full.empty())
    for (auto& dirent : fs::directory_iterator{full, ec})
      if (dirent.is_directory(ec))
        names.push_back(dirent.path().filename().string());
  set_tree_children(sandbox_tree_, node, std::move(names));
}

void mc::request_local_tree_node(tree_node& node) {
  if (node.requested)
    return;
  node.requested = true;
  dynamic req;
  req["path"_key] = node.path;
  emit("on_local_tree_expand"_key, std::move(req));
}

void mc::reveal_in_tree(
    folder_tree& tree, const std::string& path, const fs::path* resource_dir, bool allow_absolute_paths) {
  tree.reveal_target = path;
  tree.revealing = true;
  continue_reveal(tree, resource_dir, allow_absolute_paths);
}

void mc::continue_reveal(folder_tree& tree, const fs::path* resource_dir, bool allow_absolute_paths) {
  if (!tree.revealing)
    return;
  // The local tree's top level has not arrived yet: it resumes this.
  if (!tree.root.loaded)
    return;

  tree_node* node = &tree.root;
  for (;;) {
    tree_node* next = nullptr;
    auto relation = path_relation::unrelated;
    for (auto& child : node->children) {
      relation = relate_paths(child->path, tree.reveal_target, tree.is_sandbox && child->path.empty());
      if (relation != path_relation::unrelated) {
        next = child.get();
        break;
      }
    }
    if (!next || relation == path_relation::same) {
      tree.revealing = false;
      select_tree_node(tree, next);
      return;
    }

    node = next;
    node->elem["open"_key] = true;
    if (!node->loaded) {
      if (!resource_dir) {
        request_local_tree_node(*node);
        return;
      }
      load_sandbox_tree_node(*node, *resource_dir, allow_absolute_paths);
    }
  }
}

void mc::sync_tree_with_listing(
    folder_tree& tree, const std::string& path, std::vector<std::string> dir_names, bool reveal,
    const fs::path* resource_dir, bool allow_absolute_paths) {
  if (!tree.box)
    return;
  if (tree_node* node = find_tree_node(tree, path); node && node->loaded)
    set_tree_children(tree, *node, std::move(dir_names));
  if (reveal) {
    reveal_in_tree(tree, path, resource_dir, allow_absolute_paths);
  } else if (!tree.revealing) {
    select_tree_node(tree, find_tree_node(tree, path));
  }
}

void mc::navigate_local(const std::string& name, const char* type) {
  local_reveal_pending_ = true;
  dynamic nav;
  nav["name"_key] = name;
  nav["type"_key] = std::string{type};
  emit("on_local_navigate"_key, std::move(nav));
}

// ── Table population ─────────────────────────────────────────────────────────

void mc::fill_table(
    const ui_element_ptr& table, const std::vector<file_row>& entries, bool is_sandbox,
    std::unordered_map<key_t, row_menu_target, key_t, key_t>& menu_targets,
    const std::set<std::string>& selected_names) {
  menu_targets.clear();
  if (!table)
    return;
  auto* children_p = table->findField<dynamic_ptr>("children"_key);
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

    // Name column shows a small type icon ahead of the label, mirroring
    // file_dialog.cpp's file table (see make_name_cell()'s doc comment).
    ui_element_ptr name_cell = make_name_cell(entry.name, entry.type, entry.name);

    auto row_children = dynamic_ptr{key_t{0U}, {}};
    (*row_children)[size_t{0}] = dynamic_ptr{name_cell};
    (*row_children)[size_t{1}] = dynamic_ptr{make_label(entry.type == "dir" ? std::string{} : entry.size, 1)};
    (*row_children)[size_t{2}] = dynamic_ptr{make_label(entry.modified, 2)};

    // The ".." pseudo-entry (see navigate_sandbox()) gets no context menu --
    // there's nothing to rename/inspect/copy-path for "go up a level".
    if (entry.name != "..") {
      std::string path_display = is_sandbox
          ? "/" + (sandbox_path_.empty() ? entry.name : sandbox_path_ + "/" + entry.name)
          : (local_path_.empty() ? entry.name : (fs::path(local_path_) / entry.name).string());
      ui_element_ptr menu = build_row_context_menu(entry, is_sandbox, path_display, menu_targets);
      menu["order"_key] = int32_t{3};
      (*row_children)[size_t{3}] = dynamic_ptr{menu};
    }

    row["children"_key] = row_children;

    (*children)[static_cast<size_t>(idx)] = dynamic_ptr{row};
    ++idx;
  }
  table->refresh_children_order();
}

ui_element_ptr mc::build_row_context_menu(
    const file_row& entry, bool is_sandbox, const std::string& path_display,
    std::unordered_map<key_t, row_menu_target, key_t, key_t>& menu_targets) {
  auto assign_id = [&](ui_element_ptr& el) {
    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, el);
    el["__wish_id"_key] = id;
    return id;
  };

  ui_element_ptr menu = ui_element_ptr::create("wish"_key, "ContextMenu"_key);
  assign_id(menu);

  ui_element_ptr properties = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  properties["label"_key] = std::string{"Properties"};
  common::set_icon(properties, "info");
  menu_targets[assign_id(properties)] = row_menu_target{row_menu_action::properties, is_sandbox, entry.name};

  ui_element_ptr rename = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  rename["label"_key] = std::string{"Rename..."};
  common::set_icon(rename, "edit");
  menu_targets[assign_id(rename)] = row_menu_target{row_menu_action::rename, is_sandbox, entry.name};

  ui_element_ptr sep = ui_element_ptr::create("wish"_key, "Separator"_key);
  assign_id(sep);

  ui_element_ptr copy_path = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  copy_path["label"_key] = std::string{"Copy Path"};
  common::set_icon(copy_path, "copy");
  // No round trip needed: the renderer copies this to the OS clipboard
  // directly on click (see MenuItem.copy_text's field comment in
  // src/ui/ui_elements/menu.cpp). Still routed through menu_targets so
  // on_event() can show a status confirmation.
  copy_path["copy_text"_key] = path_display;
  menu_targets[assign_id(copy_path)] = row_menu_target{row_menu_action::copy_path, is_sandbox, entry.name};

  auto menu_children = dynamic_ptr{key_t{0U}, {}};
  size_t mk = 0;
  (*menu_children)[mk++] = dynamic_ptr{properties};
  (*menu_children)[mk++] = dynamic_ptr{rename};
  (*menu_children)[mk++] = dynamic_ptr{sep};
  (*menu_children)[mk++] = dynamic_ptr{copy_path};
  menu["children"_key] = menu_children;
  menu->refresh_children_order();

  return menu;
}

void mc::sort_entries(std::vector<file_row>& entries, int32_t sort_column_id, bool ascending) const {
  // A leading ".." entry (navigate_sandbox()'s "up" row) is never part of
  // the sort -- pin it at entries[0] and sort only the rest.
  size_t begin = !entries.empty() && entries[0].name == ".." ? 1 : 0;

  auto key_less = [&](const file_row& a, const file_row& b) {
    switch (sort_column_id) {
      case 1: // Size -- numeric, not lexicographic (see parse_display_size()).
        return parse_display_size(a.size) < parse_display_size(b.size);
      case 2: // Modified -- format_modified()'s "%Y-%m-%d %H:%M" sorts
              // correctly as a plain string; trust the client's own format
              // for local entries the same way the rest of this form does.
        return ascii_ci_less(a.modified, b.modified);
      default: // Name (0), and any unrecognized column_id.
        return ascii_ci_less(a.name, b.name);
    }
  };
  std::stable_sort(entries.begin() + static_cast<ptrdiff_t>(begin), entries.end(), [&](auto& a, auto& b) {
    return ascending ? key_less(a, b) : key_less(b, a);
  });
}

void mc::on_table_sorted(
    const dynamic& payload, std::vector<file_row>& entries, const ui_element_ptr& table, bool is_sandbox,
    std::unordered_map<key_t, row_menu_target, key_t, key_t>& menu_targets, int32_t& sort_column_id,
    bool& sort_ascending, const std::set<std::string>& selected_names) {
  auto* col_f = payload.findField<int32_t>("column_id"_key);
  auto* asc_f = payload.findField<bool>("ascending"_key);
  if (!col_f || !asc_f)
    return;
  sort_column_id = *col_f;
  sort_ascending = *asc_f;
  sort_entries(entries, sort_column_id, sort_ascending);
  fill_table(table, entries, is_sandbox, menu_targets, selected_names);
}

// ── Multi-selection ───────────────────────────────────────────────────────────

void mc::apply_row_click(
    std::set<std::string>& selected, int32_t& anchor, const std::vector<file_row>& entries, int32_t idx, bool ctrl,
    bool shift) {
  const std::string& name = entries[static_cast<size_t>(idx)].name;
  if (shift && anchor >= 0 && static_cast<size_t>(anchor) < entries.size()) {
    // Range-select between the anchor and idx, replacing the previous
    // selection -- matches Explorer's Shift+click/drag semantics. The
    // anchor itself does not move, so a following Shift+click/drag sweep
    // keeps redefining the same range's far end.
    selected.clear();
    int32_t lo = std::min(anchor, idx);
    int32_t hi = std::max(anchor, idx);
    for (int32_t i = lo; i <= hi; ++i)
      selected.insert(entries[static_cast<size_t>(i)].name);
  } else if (ctrl) {
    // Toggle this row alone; becomes the new anchor so a following
    // Shift+click/drag extends from here.
    if (!selected.insert(name).second)
      selected.erase(name);
    anchor = idx;
  } else {
    // Plain click (also the fallback for Shift with no anchor yet):
    // replace the selection with just this row.
    selected.clear();
    selected.insert(name);
    anchor = idx;
  }
}

std::string mc::describe_selection(const std::set<std::string>& selected) {
  if (selected.empty())
    return "Selected: (none)";
  if (selected.size() == 1)
    return "Selected: " + *selected.begin();
  return "Selected: " + std::to_string(selected.size()) + " items";
}

std::vector<std::string> mc::selected_file_names(
    const std::set<std::string>& selected, const std::vector<file_row>& entries) {
  std::vector<std::string> names;
  for (auto& e : entries)
    if (e.type == "file" && selected.count(e.name))
      names.push_back(e.name);
  return names;
}

dynamic mc::make_names_payload(const std::vector<std::string>& names) {
  dynamic payload;
  dynamic arr;
  size_t i = 0;
  for (auto& n : names)
    arr[i++] = n;
  payload["names"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return payload;
}

void mc::set_status(const std::string& message, bool is_sandbox) {
  (*this)["status"_key] = message;
  const ui_element_ptr& label = is_sandbox ? right_status_ptr_ : left_status_ptr_;
  if (label)
    label["text"_key] = message;
}

bool mc::sandbox_has_file(const std::string& name) const {
  for (auto& e : sandbox_entries_)
    if (e.type == "file" && e.name == name)
      return true;
  return false;
}

bool mc::local_has_file(const std::string& name) const {
  for (auto& e : local_entries_)
    if (e.type == "file" && e.name == name)
      return true;
  return false;
}

// ── Sandbox navigation (server-owned) ────────────────────────────────────────

void mc::navigate_sandbox(
    std::string relative_path, const fs::path& resource_dir, bool allow_absolute_paths, bool reveal) {
  // navigate_sandbox() is called both from on_init()/RMI methods (inside
  // dispatch, where sync_ctx_'s wlock is already held) and from on_event()
  // handlers (outside dispatch). sync_ctx_ is the very lock the dispatch
  // wlock covers, so acquiring context_rlock unconditionally here would
  // self-deadlock on the dispatch call paths (std::shared_mutex is
  // non-recursive). Callers resolve resource_dir/allow_absolute_paths
  // themselves -- via sess() inside dispatch, via context_rlock outside it
  // (mirrors file_dialog.cpp's on_btn_open_clicked()/on_row_activated()) --
  // and pass the result in, so this function never touches sync_ctx_.
  fs::path full;
  if (relative_path.empty()) {
    full = resource_dir;
  } else {
    full = file_service::resolve_path(relative_path, resource_dir, allow_absolute_paths);
    if (full.empty()) {
      set_status("Invalid or out-of-sandbox path.", true);
      return;
    }
  }

  std::error_code ec;
  if (!fs::is_directory(full, ec)) {
    set_status("Not a directory: " + relative_path, true);
    return;
  }

  std::vector<file_row> entries;
  if (!relative_path.empty())
    entries.push_back({"..", "dir", "", ""});

  uintmax_t file_count = 0;
  uintmax_t total_bytes = 0;
  std::vector<std::string> dir_names;
  for (auto& dirent : fs::directory_iterator{full, ec}) {
    file_row row;
    row.name = dirent.path().filename().string();
    bool is_dir = dirent.is_directory(ec);
    row.type = is_dir ? "dir" : "file";
    if (is_dir)
      dir_names.push_back(row.name);
    if (!is_dir) {
      uintmax_t bytes = dirent.file_size(ec);
      if (!ec) {
        ++file_count;
        total_bytes += bytes;
      }
      row.size = format_bytes(bytes);
    }
    auto ftime = dirent.last_write_time(ec);
    row.modified = ec ? std::string{} : format_modified(ftime);
    entries.push_back(std::move(row));
  }

  sandbox_path_ = relative_path;
  sandbox_entries_ = std::move(entries);
  // Navigating to a (possibly different) directory invalidates whatever was
  // selected before -- the old names may not even exist here.
  selected_sandbox_names_.clear();
  sandbox_selection_anchor_ = -1;
  // Re-apply whatever sort column the user last clicked, so navigating
  // away and back doesn't silently drop it (matches Explorer's own
  // persisted-sort behavior).
  sort_entries(sandbox_entries_, sandbox_sort_column_id_, sandbox_sort_ascending_);
  fill_table(right_table_ptr_, sandbox_entries_, true, sandbox_menu_targets_, selected_sandbox_names_);

  if (right_stats_ptr_)
    right_stats_ptr_["text"_key] =
        std::to_string(file_count) + (file_count == 1 ? " file, " : " files, ") + format_bytes(total_bytes);
  if (right_disk_ptr_) {
    auto space_info = fs::space(full, ec);
    right_disk_ptr_["text"_key] = ec ? std::string{}
                                      : "Disk: " + format_bytes(space_info.capacity - space_info.free) + " used, " +
            format_bytes(space_info.free) + " free of " + format_bytes(space_info.capacity);
  }

  std::string display = "/" + relative_path;
  if (right_path_ptr_)
    right_path_ptr_["value"_key] = display;
  (*this)["sandbox_path"_key] = relative_path;
  set_status("Ready.", true);

  if (right_selected_ptr_)
    right_selected_ptr_["text"_key] = describe_selection(selected_sandbox_names_);

  sync_tree_with_listing(
      sandbox_tree_, sandbox_path_, std::move(dir_names), reveal, &resource_dir, allow_absolute_paths);
}

// ── Rename dialog ─────────────────────────────────────────────────────────────

void mc::show_rename_dialog(bool is_sandbox, const std::string& name) {
  if (!rename_root_key_.empty())
    remove_rename_objects();

  rename_is_sandbox_ = is_sandbox;
  rename_old_name_ = name;

  auto tree = import_json(kRenameLayout);
  tree.with("vbox.content.message", [&](const auto& e) { e["text"_key] = "Rename \"" + name + "\" to:"; });

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  rename_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.content.new_name", [&](const auto& e) {
    e["value"_key] = name;
    rename_input_ptr_ = e;
  });
  tree.with("vbox.buttons.btn_ok", [&](const auto& e) { rename_ok_id_ = wish_id_of(e); });
  tree.with("vbox.buttons.btn_cancel", [&](const auto& e) { rename_cancel_id_ = wish_id_of(e); });

  // show_rename_dialog() is only ever called from on_event() (a MenuItem
  // click), i.e. outside dispatch -- sess()/next_available_key() would
  // throw here, so this merges the dialog as a secondary top-level root by
  // hand under context_wlock, exactly like top.cpp's show_confirm_kill().
  auto lock = context_wlock{*sync_ctx_};
  context& s = *lock;
  for (int i = 0;; ++i) {
    std::string candidate = "__mc_rename_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      rename_root_key_ = candidate;
      break;
    }
  }
  s.ui_objects.merge(std::move(tree), rename_root_key_);
  auto it = s.ui_objects.find(rename_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{rename_root_key_}] = it->second;
    (*it->second)["__path__"_key] = rename_root_key_;
    s.top_level_handlers[key_t{rename_root_key_}] = this;
  }
}

void mc::request_close_rename() {
  request_close_at(rename_root_key_);
}

void mc::remove_rename_objects() {
  remove_objects_at(rename_root_key_);
  rename_root_key_.clear();
  rename_input_ptr_.reset();
}

void mc::apply_rename() {
  if (!rename_input_ptr_) {
    remove_rename_objects();
    return;
  }
  std::string new_name = rename_input_ptr_->as<std::string>("value"_key);
  if (new_name.empty() || new_name == "." || new_name == ".." || new_name.find('/') != std::string::npos ||
      new_name.find('\\') != std::string::npos) {
    set_status("Invalid name.", rename_is_sandbox_);
    request_close_rename();
    return;
  }

  if (new_name == rename_old_name_) {
    request_close_rename();
    return;
  }

  if (rename_is_sandbox_) {
    // Mirrors row_activated's own outside-dispatch navigate_sandbox() call
    // above: resolve resource_dir/allow_absolute_paths via context_rlock,
    // since on_event() runs outside dispatch and sess() would throw here.
    auto s = context_rlock{*sync_ctx_};
    fs::path old_rel = sandbox_path_.empty() ? fs::path(rename_old_name_) : fs::path(sandbox_path_) / rename_old_name_;
    fs::path new_rel = sandbox_path_.empty() ? fs::path(new_name) : fs::path(sandbox_path_) / new_name;
    fs::path old_full = file_service::resolve_path(old_rel.string(), s->resource_dir, s->allow_absolute_paths);
    fs::path new_full = file_service::resolve_path(new_rel.string(), s->resource_dir, s->allow_absolute_paths);
    std::error_code ec;
    if (old_full.empty() || new_full.empty()) {
      set_status("Invalid or out-of-sandbox path.", true);
    } else if (fs::exists(new_full, ec)) {
      set_status("\"" + new_name + "\" already exists.", true);
    } else {
      fs::rename(old_full, new_full, ec);
      if (ec) {
        set_status("Rename failed: " + ec.message(), true);
      } else {
        navigate_sandbox(sandbox_path_, s->resource_dir, s->allow_absolute_paths);
        set_status("Renamed.", true);
      }
    }
  } else {
    // Only the client can rename its own local file -- emit and let
    // on_local_rename_requested's handler (client/mc.cpp) report the
    // outcome via set()/update_local_listing(), same as on_local_navigate.
    dynamic req;
    req["old_name"_key] = rename_old_name_;
    req["new_name"_key] = new_name;
    emit("on_local_rename_requested"_key, std::move(req));
    set_status("Renaming...", false);
  }

  request_close_rename();
}

// ── Properties dialog ─────────────────────────────────────────────────────────

void mc::show_properties_dialog(bool is_sandbox, const file_row& entry) {
  // Same path-composition rule as fill_table()'s Copy Path -- see that
  // call site for why the sandbox side stays relative rather than exposing
  // the server's absolute filesystem layout to the client.
  std::string path_display = is_sandbox
      ? "/" + (sandbox_path_.empty() ? entry.name : sandbox_path_ + "/" + entry.name)
      : (local_path_.empty() ? entry.name : (fs::path(local_path_) / entry.name).string());

  dynamic_ptr details{dynamic::instantiate("wish"_key, "FileProperties"_key)};
  details["name"_key] = entry.name;
  details["type"_key] = entry.type == "dir" ? std::string{"Folder"} : std::string{"File"};
  details["size"_key] = entry.type == "dir" ? std::string{"--"} : entry.size;
  details["modified"_key] = entry.modified.empty() ? std::string{"(unknown)"} : entry.modified;
  details["path"_key] = path_display;

  dynamic params;
  params["title"_key] = "Properties - " + entry.name;
  params["target"_key] = details;

  // Overwriting properties_dialog_ (rather than requiring it be empty
  // first) is safe even if a properties dialog is already open for a
  // different entry -- see top.cpp's confirm_dialog_ doc comment for why
  // (properties_dialog_ follows the identical pattern).
  properties_dialog_ = instantiate_child_form<properties_dialog>("PropertiesDialog"_key, std::move(params));
}

// ── RMI methods ───────────────────────────────────────────────────────────────

dynamic mc::do_update_local_listing(const dynamic& args) {
  local_path_ = args.as<std::string>("path"_key);
  local_entries_.clear();

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
      local_entries_.push_back(std::move(row));
    });
  }

  // A freshly-reported listing (possibly a different directory) invalidates
  // whatever was selected before.
  selected_local_names_.clear();
  local_selection_anchor_ = -1;

  sort_entries(local_entries_, local_sort_column_id_, local_sort_ascending_);
  fill_table(left_table_ptr_, local_entries_, false, local_menu_targets_, selected_local_names_);
  if (left_path_ptr_)
    left_path_ptr_["value"_key] = local_path_;
  (*this)["local_path"_key] = local_path_;

  // file_count/total_size/disk_* are computed client-side (report_local_listing()
  // in client/mc.cpp) since only the client can see the local machine's
  // filesystem/disk -- omitted (left blank) by an older or custom client
  // that doesn't send them.
  if (left_stats_ptr_) {
    auto* count_f = args.findField<int32_t>("file_count"_key);
    auto* size_f = args.findField<std::string>("total_size"_key);
    left_stats_ptr_["text"_key] = count_f && size_f
        ? std::to_string(*count_f) + (*count_f == 1 ? " file, " : " files, ") + *size_f
        : std::string{};
  }
  if (left_disk_ptr_) {
    auto* used_f = args.findField<std::string>("disk_used"_key);
    auto* free_f = args.findField<std::string>("disk_free"_key);
    auto* total_f = args.findField<std::string>("disk_total"_key);
    left_disk_ptr_["text"_key] =
        used_f && free_f && total_f ? "Disk: " + *used_f + " used, " + *free_f + " free of " + *total_f : std::string{};
  }

  if (left_selected_ptr_)
    left_selected_ptr_["text"_key] = describe_selection(selected_local_names_);

  std::vector<std::string> dir_names;
  for (auto& e : local_entries_)
    if (e.type == "dir" && e.name != "..")
      dir_names.push_back(e.name);
  bool reveal = std::exchange(local_reveal_pending_, false);
  sync_tree_with_listing(local_tree_, local_path_, std::move(dir_names), reveal, nullptr, false);
  return dynamic{};
}

dynamic mc::do_update_local_tree(const dynamic& args) {
  const auto* path = args.findField<std::string>("path"_key);
  if (!path || !local_tree_.box)
    return dynamic{};
  tree_node* node = path->empty() ? &local_tree_.root : find_tree_node(local_tree_, *path);
  if (!node)
    return dynamic{};

  std::vector<std::string> names;
  if (auto* dirs_f = args.findField<dynamic_ptr>("dirs"_key); dirs_f && *dirs_f) {
    (*dirs_f)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>() && !f.as<std::string>().empty())
        names.push_back(f.as<std::string>());
    });
  }
  set_tree_children(local_tree_, *node, std::move(names));

  // A lone root ("/" on POSIX) would leave the tree showing a single
  // collapsed node: open it, so the first level is the root's contents.
  if (node == &local_tree_.root && node->children.size() == 1 && !node->children.front()->loaded) {
    tree_node& only = *node->children.front();
    only.elem["open"_key] = true;
    request_local_tree_node(only);
  }
  continue_reveal(local_tree_, nullptr, false);
  return dynamic{};
}

dynamic mc::do_refresh_sandbox(const dynamic& /*args*/) {
  navigate_sandbox(sandbox_path_, sess().resource_dir, sess().allow_absolute_paths);
  return dynamic{};
}

dynamic mc::do_discard_upload(const dynamic& args) {
  const auto* name = args.findField<std::string>("name"_key);
  if (!name || name->empty())
    return dynamic{};
  // file_service::resolve_path() rejects anything that escapes the sandbox.
  // Only the staging file goes: a chunked upload never touches the target
  // name until its last chunk, so a file it was about to overwrite is intact.
  fs::path full = file_service::resolve_path(*name, sess().resource_dir, sess().allow_absolute_paths);
  if (full.empty())
    return dynamic{};
  full += file_service::kStagingSuffix;
  std::error_code ec;
  if (fs::is_regular_file(full, ec))
    fs::remove(full, ec);
  return dynamic{};
}

dynamic mc::on_set(const dynamic& patch) {
  // Only the client sets "status", and it only knows about the local side.
  if (auto* v = patch.findField<std::string>("status"_key); v && left_status_ptr_)
    left_status_ptr_["text"_key] = *v;
  return patch;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void mc::on_event(key_t id, key_t event, const dynamic& payload) {
  // Any panel's X button -> tear the whole browser down (top/pix's rule).
  if (event == "closed"_key && (id == window_id_ || id == sandbox_window_id_ || id == local_tree_window_id_ ||
                                id == sandbox_tree_window_id_)) {
    emit("closed"_key);
    remove_panel_objects();
    remove_internal_objects();
    return;
  }

  // Folder tree nodes: the arrow loads a node's children the first time it
  // is expanded; a click on the name navigates that side's file panel.
  if (event == "toggled"_key || event == "clicked"_key) {
    folder_tree* tree = &local_tree_;
    auto node_it = tree->by_id.find(id);
    if (node_it == tree->by_id.end()) {
      tree = &sandbox_tree_;
      node_it = tree->by_id.find(id);
    }
    if (node_it != tree->by_id.end()) {
      tree_node& node = *node_it->second;
      if (event == "clicked"_key) {
        select_tree_node(*tree, &node);
        if (tree->is_sandbox) {
          // Copy: navigating may refresh the tree and free `node`.
          std::string target = node.path;
          auto s = context_rlock{*sync_ctx_};
          navigate_sandbox(target, s->resource_dir, s->allow_absolute_paths, /*reveal=*/true);
        } else {
          navigate_local(node.path, "path");
        }
      } else if (!node.loaded) {
        auto* open_f = payload.findField<bool>("open"_key);
        if (!open_f || !*open_f)
          return;
        if (tree->is_sandbox) {
          auto s = context_rlock{*sync_ctx_};
          load_sandbox_tree_node(node, s->resource_dir, s->allow_absolute_paths);
        } else {
          request_local_tree_node(node);
        }
      }
      return;
    }
  }

  if (id == left_table_id_) {
    if (event == "row_selected"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx >= 0 && static_cast<size_t>(idx) < local_entries_.size()) {
        bool ctrl = false, shift = false;
        if (auto* v = payload.findField<bool>("ctrl"_key))
          ctrl = *v;
        if (auto* v = payload.findField<bool>("shift"_key))
          shift = *v;
        apply_row_click(selected_local_names_, local_selection_anchor_, local_entries_, idx, ctrl, shift);
        if (left_selected_ptr_)
          left_selected_ptr_["text"_key] = describe_selection(selected_local_names_);
        fill_table(left_table_ptr_, local_entries_, false, local_menu_targets_, selected_local_names_);
      }
      return;
    }
    if (event == "row_activated"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx < 0 || static_cast<size_t>(idx) >= local_entries_.size())
        return;
      const auto& entry = local_entries_[static_cast<size_t>(idx)];
      if (entry.type != "dir")
        return;
      navigate_local(entry.name, "dir");
      return;
    }
    if (event == "sorted"_key) {
      // Row positions change under the new sort order, so a Shift-range
      // anchor (an index) would point at the wrong entry -- reset it. The
      // selection itself is name-keyed (see selected_local_names_'s doc
      // comment) and survives the reorder unchanged.
      local_selection_anchor_ = -1;
      on_table_sorted(
          payload, local_entries_, left_table_ptr_, false, local_menu_targets_, local_sort_column_id_,
          local_sort_ascending_, selected_local_names_);
      if (left_selected_ptr_)
        left_selected_ptr_["text"_key] = describe_selection(selected_local_names_);
      return;
    }
  }

  if (id == left_path_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<std::string>("value"_key))
      navigate_local(*v, "path");
    return;
  }

  if (id == right_table_id_) {
    if (event == "row_selected"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx >= 0 && static_cast<size_t>(idx) < sandbox_entries_.size()) {
        bool ctrl = false, shift = false;
        if (auto* v = payload.findField<bool>("ctrl"_key))
          ctrl = *v;
        if (auto* v = payload.findField<bool>("shift"_key))
          shift = *v;
        apply_row_click(selected_sandbox_names_, sandbox_selection_anchor_, sandbox_entries_, idx, ctrl, shift);
        if (right_selected_ptr_)
          right_selected_ptr_["text"_key] = describe_selection(selected_sandbox_names_);
        fill_table(right_table_ptr_, sandbox_entries_, true, sandbox_menu_targets_, selected_sandbox_names_);
      }
      return;
    }
    if (event == "row_activated"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx < 0 || static_cast<size_t>(idx) >= sandbox_entries_.size())
        return;
      const auto& entry = sandbox_entries_[static_cast<size_t>(idx)];
      if (entry.type != "dir")
        return;
      std::string target = entry.name == ".." ? fs::path(sandbox_path_).parent_path().string()
                                                : (sandbox_path_.empty() ? entry.name
                                                                          : (fs::path(sandbox_path_) / entry.name).string());
      auto s = context_rlock{*sync_ctx_};
      navigate_sandbox(target, s->resource_dir, s->allow_absolute_paths, /*reveal=*/true);
      return;
    }
    if (event == "sorted"_key) {
      sandbox_selection_anchor_ = -1;
      on_table_sorted(
          payload, sandbox_entries_, right_table_ptr_, true, sandbox_menu_targets_, sandbox_sort_column_id_,
          sandbox_sort_ascending_, selected_sandbox_names_);
      if (right_selected_ptr_)
        right_selected_ptr_["text"_key] = describe_selection(selected_sandbox_names_);
      return;
    }
  }

  if (id == right_path_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<std::string>("value"_key)) {
      std::string p = *v;
      if (!p.empty() && (p.front() == '/' || p.front() == '\\'))
        p.erase(0, 1);
      auto s = context_rlock{*sync_ctx_};
      navigate_sandbox(p, s->resource_dir, s->allow_absolute_paths, /*reveal=*/true);
    }
    return;
  }

  if (id == open_explorer_id_ && event == "clicked"_key) {
    fs::path full;
    {
      auto s = context_rlock{*sync_ctx_};
      full = sandbox_path_.empty() ? s->resource_dir
                                    : file_service::resolve_path(sandbox_path_, s->resource_dir, s->allow_absolute_paths);
    }
    if (full.empty() || !open_in_host_explorer(full))
      set_status("Could not open host file explorer.", true);
    else
      set_status("Opened in host file explorer.", true);
    return;
  }

  if (id == upload_id_ && event == "clicked"_key) {
    auto files = selected_file_names(selected_local_names_, local_entries_);
    if (files.empty()) {
      set_status("Select a local file to upload.", false);
      return;
    }
    // Split the selection into targets that can upload immediately and
    // ones that would overwrite an existing sandbox file -- the client
    // confirms the latter once for the whole batch (via its own
    // MessageBox, "on_upload_conflict") instead of this form building a
    // second internal modal -- see mc.hpp's class doc comment.
    std::vector<std::string> ready, conflicts;
    for (auto& name : files)
      (sandbox_has_file(name) ? conflicts : ready).push_back(name);
    if (!ready.empty()) {
      dynamic req = make_names_payload(ready);
      req["local_path"_key] = local_path_;
      req["sandbox_path"_key] = sandbox_path_;
      emit("on_upload_requested"_key, std::move(req));
    }
    if (!conflicts.empty()) {
      dynamic req = make_names_payload(conflicts);
      req["local_path"_key] = local_path_;
      req["sandbox_path"_key] = sandbox_path_;
      emit("on_upload_conflict"_key, std::move(req));
    }
    return;
  }

  if (id == download_id_ && event == "clicked"_key) {
    auto files = selected_file_names(selected_sandbox_names_, sandbox_entries_);
    if (files.empty()) {
      set_status("Select a sandbox file to download.", true);
      return;
    }
    std::vector<std::string> ready, conflicts;
    for (auto& name : files)
      (local_has_file(name) ? conflicts : ready).push_back(name);
    if (!ready.empty()) {
      dynamic req = make_names_payload(ready);
      req["sandbox_path"_key] = sandbox_path_;
      emit("on_download_requested"_key, std::move(req));
    }
    if (!conflicts.empty()) {
      dynamic req = make_names_payload(conflicts);
      req["sandbox_path"_key] = sandbox_path_;
      emit("on_download_conflict"_key, std::move(req));
    }
    return;
  }

  // Row context-menu items (Properties/Rename/Copy Path) always emit
  // "clicked" (see MenuItem's own event doc), regardless of which panel
  // built them -- look the id up in whichever panel's target map has it.
  if (event == "clicked"_key) {
    auto* targets = &local_menu_targets_;
    auto target_it = targets->find(id);
    if (target_it == targets->end()) {
      targets = &sandbox_menu_targets_;
      target_it = targets->find(id);
    }
    if (target_it != targets->end()) {
      const row_menu_target target = target_it->second;
      auto& entries = target.is_sandbox ? sandbox_entries_ : local_entries_;
      auto entry_it =
          std::find_if(entries.begin(), entries.end(), [&](const file_row& e) { return e.name == target.name; });
      switch (target.action) {
        case row_menu_action::properties:
          if (entry_it != entries.end())
            show_properties_dialog(target.is_sandbox, *entry_it);
          return;
        case row_menu_action::rename:
          if (entry_it != entries.end())
            show_rename_dialog(target.is_sandbox, target.name);
          return;
        case row_menu_action::copy_path:
          set_status("Copied path for \"" + target.name + "\" to clipboard.", target.is_sandbox);
          return;
      }
    }
  }

  if (!rename_root_key_.empty()) {
    if (id == rename_window_id_ && event == "closed"_key) {
      remove_rename_objects();
      return;
    }
    if (id == rename_ok_id_ && event == "clicked"_key) {
      apply_rename();
      return;
    }
    // EnterReturnsTrue on the InputText (see kRenameLayout) -- Enter confirms
    // the rename the same way clicking "Rename" does, mirroring left_path/
    // right_path's own Enter-to-navigate behavior elsewhere in this form.
    if (id == wish_id_of(rename_input_ptr_) && event == "changed"_key) {
      apply_rename();
      return;
    }
    if (id == rename_cancel_id_ && event == "clicked"_key) {
      request_close_rename();
      return;
    }
  }

}

// ── Registration ──────────────────────────────────────────────────────────────

// Pure-data class reflected over by the PropertiesDialog show_properties_dialog()
// opens (see that method): every field is a plain, already-display-formatted
// string (formatting happens server-side in show_properties_dialog(), not
// via bison attributes) shown read-only, so Order alone (not Range/Enum/
// etc.) is all that's needed to match the original hand-built grid's row
// order.
void register_file_properties_class() {
  auto proto = dynamic_ptr{"FileProperties"_key, {}};

  auto add_field = [&](const char* name, const char* display_name, int32_t order) {
    proto->addField(key_t{name}, field{std::string{}, attr<DisplayName>(display_name), attr<Order>(order)});
  };

  add_field("name", "Name", 0);
  add_field("type", "Type", 1);
  add_field("size", "Size", 2);
  add_field("modified", "Modified", 3);
  add_field("path", "Path", 4);

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("File Properties"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("File/folder info shown by mc's Properties dialog (see mc::show_properties_dialog())."));

  dynamic::addClass("wish"_key, std::move(proto), key_t{0U});
}

void register_mc() {
  register_file_properties_class();

  auto proto = dynamic_ptr{"Mc"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"File Explorer"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addField(
      "local_path"_key,
      field{
          std::string{""},
          attr<DisplayName>("Local Path"),
          attr<Description>("Client-owned local directory currently shown in the Local panel. "
                            "Updated via update_local_listing(); read-only from the client's "
                            "perspective otherwise."),
          attr<Category>("Data")});

  proto->addField(
      "sandbox_path"_key,
      field{
          std::string{""},
          attr<DisplayName>("Sandbox Path"),
          attr<Description>("Server-owned sandbox directory currently shown in the Sandbox panel, "
                            "relative to the session sandbox root (\"\" == root)."),
          attr<Category>("Data")});

  proto->addField(
      "status"_key,
      field{
          std::string{"Ready."},
          attr<DisplayName>("Status"),
          attr<Description>("Latest status message. Set by the client, it is shown in the Local Machine "
                            "panel's status line (the client only reports on the local side)."),
          attr<Category>("Data")});

  proto->addMethod(
      "update_local_listing"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<mc&>(self).do_update_local_listing(args);
      }});
  proto->addMethod(
      "update_local_tree"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<mc&>(self).do_update_local_tree(args);
      }});
  proto->addMethod(
      "refresh_sandbox"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<mc&>(self).do_refresh_sandbox(args);
      }});
  proto->addMethod(
      "discard_upload"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<mc&>(self).do_discard_upload(args);
      }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<mc&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Mc"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Two-panel file browser: local machine (client-driven) vs. session sandbox "
                        "(server-driven) as dockable panels, each with a lazily-filled folder tree and "
                        "a button sending its selected files to the other, and "
                        "an \"Open in Explorer\" shortcut for the sandbox side. Listen for "
                        "on_local_navigate/on_local_tree_expand/on_upload_requested/on_download_requested to drive the "
                        "client half of the handshake, and 'closed' to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<mc>("wish"_key, "Mc"_key));
}

} // namespace bdg::wish
