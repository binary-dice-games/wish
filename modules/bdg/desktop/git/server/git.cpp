// MIT License © 2025 Binary Dice Games
/// @file git.cpp
/// @brief Implementation of the GitRepo form.
#include "git.hpp"
#include "git_graph_layout.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/forms/message_box.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cstdint>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// Sets `parent`'s children map to exactly `kids`, in order.
void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto row_children = dynamic_ptr{key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*row_children)[k++] = dynamic_ptr{kid};
  (*parent)["children"_key] = row_children;
  parent->refresh_children_order();
}

// Invokes fn(dynamic&) for each nested-dynamic_ptr entry of the array field
// `field_key` on `parent` -- the shape every *_args array (commits, branches,
// staged, ...) uses. Mirrors top::update_process_table()'s
// "processes" array walk.
template <typename Fn>
void for_each_entry(const dynamic& parent, key_t field_key, Fn&& fn) {
  const auto* arr_f = parent.findField<dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return;
  (*arr_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto entry_ptr = f.as<dynamic_ptr>();
    if (!entry_ptr)
      return;
    fn(*entry_ptr);
  });
}

// Reads an array-of-plain-strings field (e.g. a commit's "parents") into a
// std::vector<std::string>. bison::field has no vector<string> alternative
// (see bison_common.hpp's field_base variant), so a string array is a
// dynamic object whose entries are themselves plain string fields, not
// nested dynamic_ptr objects -- unlike for_each_entry()'s shape above.
std::vector<std::string> read_string_array(const dynamic& parent, key_t field_key) {
  std::vector<std::string> out;
  const auto* arr_f = parent.findField<dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return out;
  (*arr_f)->forEach([&](key_t, const field& f) {
    if (f.is<std::string>())
      out.push_back(f.as<std::string>());
  });
  return out;
}

// Small event-payload builders -- bison::dynamic has no initializer-list
// constructor (see bison_object.hpp), so payloads are built field-by-field,
// same as every other form in this codebase (see nano.cpp's
// do_open_file()); these just save repeating that boilerplate at each
// emit() call site below.
template <typename T>
dynamic payload1(key_t k, T v) {
  dynamic d;
  d[k] = std::move(v);
  return d;
}
template <typename T1, typename T2>
dynamic payload2(key_t k1, T1 v1, key_t k2, T2 v2) {
  dynamic d;
  d[k1] = std::move(v1);
  d[k2] = std::move(v2);
  return d;
}
template <typename T1, typename T2, typename T3>
dynamic payload3(key_t k1, T1 v1, key_t k2, T2 v2, key_t k3, T3 v3) {
  dynamic d;
  d[k1] = std::move(v1);
  d[k2] = std::move(v2);
  d[k3] = std::move(v3);
  return d;
}

// Status-code -> Label.text_color hex string ("#RRGGBBAA", the format
// Image.tint/Label.text_color use -- see parse_hex_color() in
// src/imgui/imgui_renderer.cpp), loosely matching SourceTree's own
// file-list coloring: green for additions, red for removals, purple for
// renames, blue for modifications (the default).
std::string status_color_hex(const std::string& status) {
  if (status == "A" || status == "?")
    return "#98C379FF"; // green
  if (status == "D")
    return "#E06C75FF"; // red
  if (status == "R")
    return "#C678DDFF"; // purple (rename)
  return "#61AFEFFF"; // blue (modified, default)
}

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// ImGuiTableFlags: Resizable=1, RowBg=64, Borders=1920, ScrollY=1<<25=33554432.
// graph_table/files_table = Resizable|RowBg|Borders|ScrollY = 33556417.
// diff_table (no row banding/borders -- diff coloring gives its own
// structure) = Resizable|ScrollY = 33554433.
// ImGuiTableColumnFlags: WidthFixed=16, WidthStretch=8.
//
// Every "outer_height": -1 table below also carries its own "height": -1 --
// load-bearing, not cosmetic. "outer_height": -1 asks render_table() to
// fill whatever ambient region the table is rendered into, but each of
// these tables is the child of an auto-sized (no height hint of its own)
// VerticalLayout; without also marking the table itself as a stretch (-1)
// Layout child, arrange_vertical_layout() falls back to the table's own
// *previous frame's real rendered height* (measure_node()'s
// last_rendered_size() fallback, imgui_layout.cpp) as if it were the
// table's natural size -- which already includes its own fill-driven
// height, compounding without bound frame over frame (see
// modules/bdg/desktop/tail/server/tail.cpp's kLayout comment for the full
// mechanism, confirmed there via WISH_LAYOUT_DEBUG_LOG).
//
// The same "height": -1 requirement propagates one level further up here:
// "graph_panel" (the VerticalLayout wrapping current_branch_label +
// graph_table) is itself a HorizontalLayout child ("body"), and must carry
// its own "height": -1 too, not just "width": -1. Without it,
// arrange_horizontal_layout()'s cross-axis sizing (imgui_layout.cpp) falls
// back to graph_panel's *natural/measured* height for the "no hint"
// (auto) case -- and measure_vertical_layout() deliberately counts a
// "height": -1 child (graph_table) as contributing 0 to that natural sum
// (a stretch child "wants to fill whatever's left over, not define it"),
// so graph_panel's measured height collapses to just
// current_branch_label's own height, starving graph_table down to a few
// pixels and rendering the whole graph as blank space -- this was a real,
// live-reproduced bug (see DESIGN.md's Implementation Status / this fix's
// history) fixed by adding "height": -1 here, matching the sibling
// VerticalLayout-next-to-a-fill-Table pattern already used by e.g.
// modules/bdg/desktop/mc/server/mc.cpp's "left"/"right" panels
// ("width": -1, "height": -1" on both).
//
// graph_table's own "auto_scroll": false overrides Table's own default
// (true, "stick to the newest row at the bottom as the row count grows" --
// see docs/ui-elements.md's Table.auto_scroll entry): that default assumes
// a log-like table where the newest entry is the LAST row, e.g. this same
// file's log_table. The commit graph is the opposite -- newest commit (or
// the synthetic "Uncommitted changes" row) is always row 0 -- so the
// default behavior scrolled to the *oldest* commit on every load/refresh,
// the exact opposite of useful. With auto_scroll off, ImGui's own default
// (top of a freshly-created scroll region) already puts the newest commit
// in view with no code needed, and a user's manual scroll position across
// commit history survives a refresh instead of jumping back and forth.

static constexpr const char* kMainLayout = R"({
  "type": "Window", "title": "Git", "width": 900, "height": 720, "pos_x": 0, "pos_y": 0, "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout", "spacing": 6,
          "children": {
            "btn_commit":  { "type": "Button", "label": "Commit",  "width": 90 },
            "btn_push":    { "type": "Button", "label": "Push",    "width": 90 },
            "btn_pull":    { "type": "Button", "label": "Pull",    "width": 90 },
            "btn_fetch":   { "type": "Button", "label": "Fetch",   "width": 90 },
            "btn_branch":  { "type": "Button", "label": "Branch",  "width": 90 },
            "btn_merge":   { "type": "Button", "label": "Merge",   "width": 90 },
            "btn_stash":   { "type": "Button", "label": "Stash",   "width": 90 },
            "btn_refresh": { "type": "Button", "label": "Refresh", "width": 90 }
          }
        },
        "status_label": { "type": "Label", "text": "" },
        "sep": { "type": "Separator" },
        "body": {
          "type": "HorizontalLayout", "spacing": 6, "height": -1,
          "children": {
            "sidebar": {
              "type": "VerticalLayout", "width": 260,
              "children": {
                "new_branch_row": {
                  "type": "HorizontalLayout", "spacing": 4,
                  "children": {
                    "new_branch_input": { "type": "InputText", "hint": "New branch name", "width": 150 },
                    "btn_create_branch": { "type": "Button", "label": "Create", "width": 64 }
                  }
                },
                "sep_sidebar": { "type": "Separator" },
                "branches_header": { "type": "Label", "text": "BRANCHES" },
                "branches": { "type": "TreeNode", "label": "Local", "open": true, "children": {} },
                "remotes_header": { "type": "Label", "text": "REMOTES" },
                "remotes": { "type": "TreeNode", "label": "Remote-tracking", "open": false, "children": {} },
                "tags_header": { "type": "Label", "text": "TAGS" },
                "tags": { "type": "TreeNode", "label": "Tags", "open": false, "children": {} },
                "stashes_header": { "type": "Label", "text": "STASHES" },
                "stashes": { "type": "TreeNode", "label": "Stashes", "open": false, "children": {} }
              }
            },
            "graph_panel": {
              "type": "VerticalLayout", "width": -1, "height": -1,
              "children": {
                "current_branch_label": { "type": "Label", "text": "" },
                "graph_table": {
                  "type": "Table", "id": "##graph_table", "columns": 5, "height": -1,
                  "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true, "outer_height": -1,
                  "auto_scroll": false,
                  "children": {
                    "col_graph":  { "type": "TableColumn", "label": "Graph",       "flags": "WidthFixed", "init_width": 80,  "column_id": 0 },
                    "col_desc":   { "type": "TableColumn", "label": "Description", "flags": "WidthStretch",                     "column_id": 1 },
                    "col_author": { "type": "TableColumn", "label": "Author",      "flags": "WidthFixed", "init_width": 130, "column_id": 2 },
                    "col_date":   { "type": "TableColumn", "label": "Date",        "flags": "WidthFixed", "init_width": 150, "column_id": 3 },
                    "col_hash":   { "type": "TableColumn", "label": "Commit",      "flags": "WidthFixed", "init_width": 80,  "column_id": 4 }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})";

static constexpr const char* kFilesLayout = R"({
  "type": "Window", "title": "Files", "width": 380, "height": 360, "pos_x": 900, "pos_y": 0,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "title_label": { "type": "Label", "text": "Uncommitted changes" },
        "files_table": {
          "type": "Table", "id": "##files_table", "columns": 2, "height": -1,
          "flags": "Resizable|RowBg|Borders|ScrollY", "outer_height": -1,
          "children": {
            "col_check": { "type": "TableColumn", "flags": "WidthFixed", "init_width": 28, "column_id": 0 },
            "col_path":  { "type": "TableColumn", "flags": "WidthStretch",                    "column_id": 1 }
          }
        },
        "commit_row": {
          "type": "HorizontalLayout", "spacing": 6,
          "children": {
            "commit_message": { "type": "InputText", "hint": "Commit message", "width": -1 },
            "commit_button":  { "type": "Button", "label": "Commit", "width": 90 }
          }
        }
      }
    }
  }
})";

static constexpr const char* kDiffLayout = R"({
  "type": "Window", "title": "Diff", "width": 380, "height": 360, "pos_x": 900, "pos_y": 360,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "title_label": { "type": "Label", "text": "" },
        "diff_table": {
          "type": "Table", "id": "##diff_table", "columns": 2, "height": -1,
          "flags": "Resizable|ScrollY", "outer_height": -1,
          "children": {
            "col_gutter": { "type": "TableColumn", "flags": "WidthFixed", "init_width": 16, "column_id": 0 },
            "col_text":   { "type": "TableColumn", "flags": "WidthStretch",                    "column_id": 1 }
          }
        }
      }
    }
  }
})";

static constexpr const char* kLogLayout = R"({
  "type": "Window", "title": "Log", "width": 900, "height": 260, "pos_x": 0, "pos_y": 720,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "log_table": {
          "type": "Table", "id": "##git_log_table", "columns": 4, "height": -1,
          "flags": "Resizable|RowBg|Borders|ScrollY", "headers": true, "outer_height": -1,
          "children": {
            "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed", "init_width": 40,  "column_id": 0 },
            "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed", "init_width": 340, "column_id": 1 },
            "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed", "init_width": 50,  "column_id": 2 },
            "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
          }
        }
      }
    }
  }
})";

// The confirm dialog (show_confirm() below) is now a privately-instantiated
// MessageBox (see form::instantiate_child_form()) -- no inline layout needed.

// ── git_repo ─────────────────────────────────────────────────────────────────

git_repo::git_repo(dynamic&& base) : form(std::move(base)) {}

void git_repo::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

void git_repo::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__git_");
  files_root_key_ = internal_root_key_ + "_files";
  diff_root_key_ = internal_root_key_ + "_diff";
  log_root_key_ = internal_root_key_ + "_log";

  build_main_window();
  build_files_window();
  build_diff_window();
  build_log_window();

  // Initial population is triggered client-side instead of by emitting
  // "refresh_requested" here -- an event fired this early (during on_init()
  // itself, before the client has even finished wiring its onEvent
  // handlers) was reliably lost; see client/git.cpp's run_git() comment on
  // its own explicit initial refresh_all() call.
}

void git_repo::build_main_window() {
  auto tree = import_json(kMainLayout);

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.status_label", [&](const auto& e) { status_label_ = e; });
  tree.with("vbox.body.sidebar.new_branch_row.new_branch_input", [&](const auto& e) {
    new_branch_input_ = e;
    new_branch_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.body.sidebar.branches", [&](const auto& e) { sidebar_branches_section_ = e; });
  tree.with("vbox.body.sidebar.remotes", [&](const auto& e) { sidebar_remotes_section_ = e; });
  tree.with("vbox.body.sidebar.tags", [&](const auto& e) { sidebar_tags_section_ = e; });
  tree.with("vbox.body.sidebar.stashes", [&](const auto& e) { sidebar_stashes_section_ = e; });
  tree.with("vbox.body.graph_panel.current_branch_label", [&](const auto& e) { current_branch_label_ = e; });
  tree.with("vbox.body.graph_panel.graph_table", [&](const auto& e) {
    graph_table_ = e;
    graph_table_id_ = wish_id_of(e);
  });

  // Toolbar buttons.
  auto bind_click = [&](const std::string& path, std::function<void()> handler) {
    tree.with(path, [&](const auto& e) { click_handlers_[wish_id_of(e)] = std::move(handler); });
  };
  bind_click("vbox.toolbar.btn_commit", [this] {
    if (!commit_message_text_.empty())
      emit("commit_requested"_key, payload1("message"_key, commit_message_text_));
  });
  bind_click("vbox.toolbar.btn_push", [this] { emit("push_requested"_key); });
  bind_click("vbox.toolbar.btn_pull", [this] { emit("pull_requested"_key); });
  bind_click("vbox.toolbar.btn_fetch", [this] { emit("fetch_requested"_key); });
  bind_click("vbox.toolbar.btn_branch", [this] { submit_new_branch(); });
  bind_click("vbox.body.sidebar.new_branch_row.btn_create_branch", [this] { submit_new_branch(); });
  bind_click("vbox.toolbar.btn_merge", [this] {
    if (!selected_branch_.empty())
      emit("merge_requested"_key, payload1("ref"_key, selected_branch_));
  });
  bind_click("vbox.toolbar.btn_stash", [this] { emit("stash_push_requested"_key); });
  bind_click("vbox.toolbar.btn_refresh", [this] { emit("refresh_requested"_key); });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

void git_repo::build_files_window() {
  auto tree = import_json(kFilesLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  files_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.title_label", [&](const auto& e) { files_title_label_ = e; });
  tree.with("vbox.files_table", [&](const auto& e) { files_table_ = e; });
  tree.with("vbox.commit_row.commit_message", [&](const auto& e) {
    commit_message_input_ = e;
    commit_message_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.commit_row.commit_button", [&](const auto& e) {
    commit_button_ = e;
    commit_button_id_ = wish_id_of(e);
    click_handlers_[commit_button_id_] = [this] {
      if (!commit_message_text_.empty())
        emit("commit_requested"_key, payload1("message"_key, commit_message_text_));
    };
  });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), files_root_key_);
  sess().top_level_objects[key_t{files_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{files_root_key_}] = this;
  (*root_ptr)["__path__"_key] = files_root_key_;
}

void git_repo::build_diff_window() {
  auto tree = import_json(kDiffLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  diff_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.title_label", [&](const auto& e) { diff_title_label_ = e; });
  tree.with("vbox.diff_table", [&](const auto& e) { diff_table_ = e; });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), diff_root_key_);
  sess().top_level_objects[key_t{diff_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{diff_root_key_}] = this;
  (*root_ptr)["__path__"_key] = diff_root_key_;
}

void git_repo::build_log_window() {
  auto tree = import_json(kLogLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  log_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.log_table", [&](const auto& e) { log_table_ = e; });

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), log_root_key_);
  sess().top_level_objects[key_t{log_root_key_}] = root_ptr;
  sess().top_level_handlers[key_t{log_root_key_}] = this;
  (*root_ptr)["__path__"_key] = log_root_key_;
}

// ── Confirmation modal ───────────────────────────────────────────────────────

void git_repo::show_confirm(
    const std::string& message, const std::string& /*confirm_label*/, std::function<void()> on_confirm) {
  dynamic params;
  params["title"_key] = std::string{"Confirm"};
  params["message"_key] = message;
  params["icon"_key] = std::string{"warning"};
  params["buttons"_key] = std::string{"yes_no"};

  // Overwriting confirm_dialog_ (rather than requiring it be empty first)
  // is safe even if a confirm dialog is already open -- see confirm_dialog_'s
  // doc comment.
  confirm_dialog_ = instantiate_child_form<message_box>(
      "MessageBox"_key, std::move(params),
      [on_confirm = std::move(on_confirm)](key_t /*event_name*/, const dynamic& payload) {
        if (payload.as<std::string>("button"_key) == "yes")
          on_confirm();
      });
}

// ── Sidebar ──────────────────────────────────────────────────────────────────

ui_element_ptr git_repo::make_menu_item(const std::string& label, std::function<void()> on_click) {
  ui_element_ptr item{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  item["label"_key] = label;
  assign_id(item);
  click_handlers_[wish_id_of(item)] = std::move(on_click);
  return item;
}

ui_element_ptr git_repo::make_sidebar_row(
    const std::string& label,
    std::function<void()> on_click,
    const std::vector<std::pair<std::string, std::function<void()>>>& menu_items) {
  ui_element_ptr row{dynamic::instantiate("wish"_key, "HorizontalLayout"_key)};
  row["spacing"_key] = 4.0f;
  assign_id(row);

  ui_element_ptr sel{dynamic::instantiate("wish"_key, "Selectable"_key)};
  sel["label"_key] = label;
  sel["width"_key] = 180.0f;
  assign_id(sel);
  selectable_handlers_[wish_id_of(sel)] = std::move(on_click);

  ui_element_ptr menu{dynamic::instantiate("wish"_key, "MenuButton"_key)};
  menu["label"_key] = "...";
  assign_id(menu);
  std::vector<ui_element_ptr> items;
  items.reserve(menu_items.size());
  for (auto& [item_label, item_click] : menu_items)
    items.push_back(make_menu_item(item_label, item_click));
  set_children_list(menu, items);

  set_children_list(row, {sel, menu});
  return row;
}

void git_repo::rebuild_section(
    const ui_element_ptr& section,
    std::vector<sidebar_row>& rows,
    size_t count,
    const std::function<ui_element_ptr(size_t)>& make_row) {
  if (!section)
    return;
  auto* children_p = section->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto& r : rows)
    children->erase(r.child_key);
  rows.clear();

  // dynamic::size() (bison_object.hpp) reports "highest numeric key + 1",
  // not a true element count -- so each independent children map's keys
  // must be its own contiguous 0-based sequence, not shared with any other
  // container. Safe here because every prior row was just erased above:
  // the loop index directly gives that fresh 0..count-1 sequence.
  for (size_t i = 0; i < count; ++i) {
    ui_element_ptr row = make_row(i);
    sidebar_row entry;
    entry.child_key = i;
    (*children)[entry.child_key] = dynamic_ptr{row};
    entry.row = std::move(row);
    rows.push_back(std::move(entry));
  }
  section->refresh_children_order();
}

// ── New branch creation ──────────────────────────────────────────────────

void git_repo::submit_new_branch() {
  if (new_branch_name_text_.empty())
    return;
  emit("create_branch_requested"_key, payload2("name"_key, new_branch_name_text_, "start_point"_key, std::string{}));
  new_branch_name_text_.clear();
  if (new_branch_input_)
    new_branch_input_["value"_key] = std::string{};
}

// ── update_refs ────────────────────────────────────────────────────────────

dynamic git_repo::do_update_refs(const dynamic& args) {
  std::string current_branch = args.as<std::string>("current_branch"_key);
  if (current_branch_label_)
    current_branch_label_["text"_key] = "On branch: " + current_branch;

  struct branch_info {
    std::string name;
    bool is_remote{false};
    std::string upstream;
    int32_t ahead{0};
    int32_t behind{0};
  };
  std::vector<branch_info> local, remote;
  for_each_entry(args, "branches"_key, [&](const dynamic& e) {
    branch_info b;
    b.name = e.as<std::string>("name"_key);
    b.is_remote = e.as<bool>("is_remote"_key);
    b.upstream = e.as<std::string>("upstream"_key);
    b.ahead = e.as<int32_t>("ahead"_key);
    b.behind = e.as<int32_t>("behind"_key);
    (b.is_remote ? remote : local).push_back(std::move(b));
  });

  rebuild_section(sidebar_branches_section_, branch_rows_, local.size(), [&](size_t i) {
    const auto& b = local[i];
    std::string label = b.name;
    if (b.name == current_branch)
      label = "* " + label;
    if (b.ahead > 0 || b.behind > 0)
      label += " (" + std::to_string(b.ahead) + "\xE2\x86\x91 " + std::to_string(b.behind) + "\xE2\x86\x93)";
    std::string ref = b.name;
    // A plain click only selects the row (sets selected_branch_, the Merge
    // button's target) rather than checking the branch out immediately --
    // switching branches is a deliberate action with real side effects
    // (working-tree files change), so it belongs behind an explicit
    // "Checkout" menu action (below), the same way tags already work, not
    // behind an easy-to-trigger-by-accident single click.
    return make_sidebar_row(
        label, [this, ref] { selected_branch_ = ref; },
        {{"Checkout", [this, ref] { selected_branch_ = ref; emit("checkout_requested"_key, payload1("ref"_key, ref)); }},
         {"Merge into current", [this, ref] { selected_branch_ = ref; emit("merge_requested"_key, payload1("ref"_key, ref)); }},
         {"Delete",
          [this, ref] {
            show_confirm("Delete branch '" + ref + "'?", "Delete", [this, ref] {
              emit("delete_branch_requested"_key, payload2("name"_key, ref, "force"_key, false));
            });
          }}});
  });

  rebuild_section(sidebar_remotes_section_, remote_rows_, remote.size(), [&](size_t i) {
    const auto& b = remote[i];
    std::string ref = b.name;
    return make_sidebar_row(
        ref, [this, ref] { selected_branch_ = ref; },
        {{"Checkout", [this, ref] { selected_branch_ = ref; emit("checkout_requested"_key, payload1("ref"_key, ref)); }},
         {"Merge into current", [this, ref] { selected_branch_ = ref; emit("merge_requested"_key, payload1("ref"_key, ref)); }}});
  });

  std::vector<std::string> tags;
  for_each_entry(args, "tags"_key, [&](const dynamic& e) { tags.push_back(e.as<std::string>("name"_key)); });
  rebuild_section(sidebar_tags_section_, tag_rows_, tags.size(), [&](size_t i) {
    std::string ref = tags[i];
    return make_sidebar_row(
        ref, [this, ref] { selected_branch_ = ref; }, {{"Checkout", [this, ref] { emit("checkout_requested"_key, payload1("ref"_key, ref)); }}});
  });

  struct stash_info {
    int32_t index{0};
    std::string message;
  };
  std::vector<stash_info> stashes;
  for_each_entry(args, "stashes"_key, [&](const dynamic& e) {
    stash_info s;
    s.index = e.as<int32_t>("index"_key);
    s.message = e.as<std::string>("message"_key);
    stashes.push_back(std::move(s));
  });
  rebuild_section(sidebar_stashes_section_, stash_rows_, stashes.size(), [&](size_t i) {
    int32_t idx = stashes[i].index;
    return make_sidebar_row(
        "stash@{" + std::to_string(idx) + "}: " + stashes[i].message,
        [] {},
        {{"Apply", [this, idx] { emit("stash_apply_requested"_key, payload1("index"_key, idx)); }},
         {"Pop", [this, idx] { emit("stash_pop_requested"_key, payload1("index"_key, idx)); }},
         {"Drop",
          [this, idx] {
            show_confirm(
                "Drop stash@{" + std::to_string(idx) + "}? This cannot be undone.", "Drop", [this, idx] {
                  emit("stash_drop_requested"_key, payload1("index"_key, idx));
                });
          }}});
  });

  return dynamic{};
}

// ── update_log / graph ───────────────────────────────────────────────────────

dynamic git_repo::do_update_log(const dynamic& args) {
  rebuild_graph_table(args);
  return dynamic{};
}

void git_repo::rebuild_graph_table(const dynamic& args) {
  if (!graph_table_)
    return;
  auto* children_p = graph_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  // Clear any previously-added rows (TableColumn children, added once by
  // kMainLayout, are left untouched). Rows aren't individually tracked by
  // key here (unlike rebuild_section()'s sidebar_row::child_key), so find
  // them by walking the existing children and matching on class instead.
  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    if (f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  graph_row_hashes_.clear();

  std::vector<git_graph_commit_in> commit_inputs;
  struct commit_meta {
    std::string hash, author, date, subject;
  };
  std::vector<commit_meta> metas;
  for_each_entry(args, "commits"_key, [&](const dynamic& e) {
    git_graph_commit_in c;
    c.hash = e.as<std::string>("hash"_key);
    c.parents = read_string_array(e, "parents"_key);
    commit_inputs.push_back(c);
    metas.push_back({c.hash, e.as<std::string>("author"_key), e.as<std::string>("date"_key), e.as<std::string>("subject"_key)});
  });

  const bool working_dirty = args.as<bool>("working_dirty"_key);
  auto layout = compute_git_graph_layout(commit_inputs);

  // Local, freshly-zeroed counter: this table's children map was just fully
  // cleared above, so a 0-based sequence here matches dynamic::size()'s
  // "highest numeric key + 1" semantics exactly (see rebuild_section()'s
  // comment on the same point) -- must not be a counter shared with any
  // other children map (sidebar sections, files/diff tables, ...).
  size_t row_key = 0;
  auto add_row = [&](const std::string& hash,
                      const std::string& subject,
                      const std::string& author,
                      const std::string& date,
                      int32_t lane,
                      int32_t color,
                      bool is_head,
                      bool is_working,
                      const std::vector<git_graph_segment>& top,
                      const std::vector<git_graph_segment>& bottom) {
    ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
    assign_id(row);

    ui_element_ptr graph_cell{dynamic::instantiate("wish"_key, "GraphNode"_key)};
    graph_cell["lane"_key] = lane;
    graph_cell["color"_key] = color;
    graph_cell["is_head"_key] = is_head;
    graph_cell["is_working"_key] = is_working;
    {
      std::vector<int32_t> tf, tt, tc, bf, bt, bc;
      for (auto& s : top) {
        tf.push_back(s.from_lane);
        tt.push_back(s.to_lane);
        tc.push_back(s.color);
      }
      for (auto& s : bottom) {
        bf.push_back(s.from_lane);
        bt.push_back(s.to_lane);
        bc.push_back(s.color);
      }
      graph_cell["top_from"_key] = tf;
      graph_cell["top_to"_key] = tt;
      graph_cell["top_color"_key] = tc;
      graph_cell["bottom_from"_key] = bf;
      graph_cell["bottom_to"_key] = bt;
      graph_cell["bottom_color"_key] = bc;
    }
    assign_id(graph_cell);

    ui_element_ptr desc{dynamic::instantiate("wish"_key, "Label"_key)};
    desc["text"_key] = subject;
    assign_id(desc);

    ui_element_ptr author_l{dynamic::instantiate("wish"_key, "Label"_key)};
    author_l["text"_key] = author;
    assign_id(author_l);

    ui_element_ptr date_l{dynamic::instantiate("wish"_key, "Label"_key)};
    date_l["text"_key] = date;
    assign_id(date_l);

    ui_element_ptr hash_l{dynamic::instantiate("wish"_key, "Label"_key)};
    hash_l["text"_key] = hash.substr(0, 8);
    assign_id(hash_l);

    set_children_list(row, {graph_cell, desc, author_l, date_l, hash_l});

    (*children)[row_key++] = dynamic_ptr{row};
    graph_row_hashes_.push_back(hash);
  };

  if (working_dirty) {
    const int32_t head_lane = layout.empty() ? 0 : layout.front().lane;
    const int32_t head_color = layout.empty() ? git_graph_lane_color(0) : layout.front().color;
    std::vector<git_graph_segment> bottom;
    if (!layout.empty())
      bottom.push_back({0, head_lane, head_color});
    add_row("", "Uncommitted changes", "", "", 0, head_color, false, true, {}, bottom);
  }

  for (size_t i = 0; i < metas.size(); ++i) {
    const bool is_head = (i == 0) && !working_dirty;
    add_row(
        metas[i].hash,
        metas[i].subject,
        metas[i].author,
        metas[i].date,
        layout[i].lane,
        layout[i].color,
        is_head,
        false,
        layout[i].top,
        layout[i].bottom);
  }

  graph_table_->refresh_children_order();

  // Nothing selected yet (fresh log): default to the working tree if dirty,
  // else the newest commit, so the Files/Diff panels show something useful
  // without requiring an explicit click.
  if (selected_hash_.empty() && graph_row_hashes_.empty()) {
    // No rows at all (brand-new/empty repo) -- nothing to select.
  } else if (!graph_row_hashes_.empty()) {
    bool have_selection = false;
    for (auto& h : graph_row_hashes_) {
      if (h == selected_hash_) {
        have_selection = true;
        break;
      }
    }
    if (!have_selection)
      select_row(0);
  }
}

void git_repo::select_row(int32_t index) {
  if (index < 0 || static_cast<size_t>(index) >= graph_row_hashes_.size())
    return;
  selected_hash_ = graph_row_hashes_[static_cast<size_t>(index)];
  selected_path_.clear();

  if (selected_hash_.empty()) {
    status_mode_working_ = true;
    if (files_title_label_)
      files_title_label_["text"_key] = "Uncommitted changes";
    do_update_status(last_status_args_);
  } else {
    status_mode_working_ = false;
    if (files_title_label_)
      files_title_label_["text"_key] = "Commit " + selected_hash_.substr(0, 8);
    emit("commit_files_requested"_key, payload1("hash"_key, selected_hash_));
  }
}

// ── update_status / update_commit_files ───────────────────────────────────────

void git_repo::clear_file_rows() {
  if (!files_table_)
    return;
  auto* children_p = files_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
  file_rows_.clear();
  next_file_row_key_ = 0; // see add_file_row()'s comment on why this must be per-table.
}

void git_repo::add_file_row(const std::string& path, const std::string& status, bool staged, bool show_checkbox) {
  if (!files_table_)
    return;
  auto* children_p = files_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
  assign_id(row);

  ui_element_ptr marker;
  if (show_checkbox) {
    marker = ui_element_ptr{dynamic::instantiate("wish"_key, "Checkbox"_key)};
    marker["value"_key] = staged;
    assign_id(marker);
    checkbox_handlers_[wish_id_of(marker)] = [this, path](bool checked) {
      emit(checked ? "stage_requested"_key : "unstage_requested"_key, payload1("path"_key, path));
    };
  } else {
    marker = ui_element_ptr{dynamic::instantiate("wish"_key, "Label"_key)};
    marker["text"_key] = status;
    marker["text_color"_key] = status_color_hex(status);
    assign_id(marker);
  }

  ui_element_ptr path_sel{dynamic::instantiate("wish"_key, "Selectable"_key)};
  path_sel["label"_key] = path;
  assign_id(path_sel);
  selectable_handlers_[wish_id_of(path_sel)] = [this, path, staged] {
    selected_path_ = path;
    selected_staged_ = staged;
    request_diff_for_selected();
  };

  set_children_list(row, {marker, path_sel});

  // next_file_row_key_ is reset to 0 by clear_file_rows() (always called
  // before the first add_file_row() of a rebuild), giving this table its
  // own contiguous 0-based key sequence -- see rebuild_section()'s comment
  // on why child keys must never be shared across independent children maps.
  (*children)[next_file_row_key_++] = dynamic_ptr{row};

  file_row_entry entry;
  entry.row = row;
  entry.path = path;
  entry.staged = staged;
  file_rows_.push_back(std::move(entry));
}

void git_repo::request_diff_for_selected() {
  if (selected_path_.empty())
    return;
  emit(
      "diff_requested"_key,
      payload3("hash"_key, selected_hash_, "path"_key, selected_path_, "staged"_key, selected_staged_));
}

dynamic git_repo::do_update_status(const dynamic& args) {
  // dynamic's copy-assignment is deleted (bison_object.hpp) -- clone()
  // returns a fresh dynamic by value, which binds to move-assignment instead.
  last_status_args_ = args.clone();
  if (!status_mode_working_)
    return dynamic{};

  clear_file_rows();
  for_each_entry(args, "staged"_key, [&](const dynamic& e) {
    add_file_row(e.as<std::string>("path"_key), e.as<std::string>("status"_key), true, true);
  });
  for_each_entry(args, "unstaged"_key, [&](const dynamic& e) {
    add_file_row(e.as<std::string>("path"_key), e.as<std::string>("status"_key), false, true);
  });
  files_table_->refresh_children_order();
  return dynamic{};
}

dynamic git_repo::do_update_commit_files(const dynamic& args) {
  std::string hash = args.as<std::string>("hash"_key);
  if (hash != selected_hash_ || status_mode_working_)
    return dynamic{}; // stale response for a selection that has since changed.

  clear_file_rows();
  for_each_entry(args, "files"_key, [&](const dynamic& e) {
    add_file_row(e.as<std::string>("path"_key), e.as<std::string>("status"_key), false, false);
  });
  files_table_->refresh_children_order();
  return dynamic{};
}

// ── update_diff ────────────────────────────────────────────────────────────

void git_repo::clear_diff_rows() {
  if (!diff_table_)
    return;
  auto* children_p = diff_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  std::vector<key_t> to_erase;
  children->forEach([&](key_t k, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && f.as<dynamic_ptr>()->as<key_t>(dynamic::CLASS) == "TableRow"_key)
      to_erase.push_back(k);
  });
  for (auto k : to_erase)
    children->erase(k.id);
}

dynamic git_repo::do_update_diff(const dynamic& args) {
  std::string path = args.as<std::string>("path"_key);
  std::string hash = args.as<std::string>("hash"_key);
  bool staged = args.as<bool>("staged"_key);
  // Stale response for a selection that has since changed -- mirrors
  // do_update_commit_files()'s own guard just above. request_diff_for_
  // selected() re-reads selected_hash_/selected_path_/selected_staged_ at
  // the moment the file's Selectable is clicked, but the client's git
  // show/diff call and RMI round trip both take real wall-clock time; if
  // the user selects a different commit or file (or the working row) while
  // that request is still in flight, its eventual response must not
  // silently overwrite whatever is now displayed -- a real, reproducible
  // hazard since this method previously had no such check at all (see
  // DESIGN.md's "Design Decisions" entry on this fix).
  if (path != selected_path_ || hash != selected_hash_ || staged != selected_staged_)
    return dynamic{};
  if (diff_title_label_)
    diff_title_label_["text"_key] = path;
  if (!diff_table_)
    return dynamic{};

  clear_diff_rows();
  auto* children_p = diff_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return dynamic{};
  auto& children = *children_p;

  // Local counter: this table's children map was just fully cleared above
  // by clear_diff_rows(), so a 0-based sequence here matches dynamic::
  // size()'s "highest numeric key + 1" semantics -- see rebuild_section()'s
  // comment on why child keys must never be shared across independent
  // children maps.
  size_t row_key = 0;
  for_each_entry(args, "lines"_key, [&](const dynamic& e) {
    std::string kind = e.as<std::string>("kind"_key);
    std::string text = e.as<std::string>("text"_key);

    std::string gutter = "add" == kind ? "+" : "del" == kind ? "-" : " ";
    std::string color_hex = "add" == kind ? "#98C379FF" : "del" == kind ? "#E06C75FF" : "header" == kind ? "#61AFEFFF" : "#ABB2BFFF";

    ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
    assign_id(row);

    ui_element_ptr gutter_l{dynamic::instantiate("wish"_key, "Label"_key)};
    gutter_l["text"_key] = gutter;
    gutter_l["text_color"_key] = color_hex;
    assign_id(gutter_l);

    ui_element_ptr text_l{dynamic::instantiate("wish"_key, "Label"_key)};
    text_l["text"_key] = text;
    text_l["text_color"_key] = color_hex;
    assign_id(text_l);

    set_children_list(row, {gutter_l, text_l});
    (*children)[row_key++] = dynamic_ptr{row};
  });

  diff_table_->refresh_children_order();
  return dynamic{};
}

// ── command_result ─────────────────────────────────────────────────────────

dynamic git_repo::do_command_result(const dynamic& args) {
  std::string command = args.as<std::string>("command"_key);
  bool ok = args.as<bool>("ok"_key);
  std::string output = args.as<std::string>("output"_key);
  if (status_label_) {
    std::string text = ok ? (command + ": OK") : (command + " failed: " + output);
    status_label_["text"_key] = text;
    status_label_["text_color"_key] = ok ? std::string{"#98C379FF"} : std::string{"#E06C75FF"};
  }
  return dynamic{};
}

// ── Log (git-command trace) ─────────────────────────────────────────────────

void git_repo::append_log_row(const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
  if (!log_table_)
    return;
  auto* children_p = log_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  std::string color_hex = ok ? "#98C379FF" : "#E06C75FF";

  ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
  assign_id(row);

  ui_element_ptr cell_seq{dynamic::instantiate("wish"_key, "Label"_key)};
  cell_seq["text"_key] = std::to_string(++log_seq_);
  assign_id(cell_seq);

  ui_element_ptr cell_command{dynamic::instantiate("wish"_key, "Label"_key)};
  cell_command["text"_key] = command;
  cell_command["text_color"_key] = color_hex;
  assign_id(cell_command);

  ui_element_ptr cell_exit{dynamic::instantiate("wish"_key, "Label"_key)};
  cell_exit["text"_key] = std::to_string(exit_code);
  cell_exit["text_color"_key] = color_hex;
  assign_id(cell_exit);

  ui_element_ptr cell_output{dynamic::instantiate("wish"_key, "Label"_key)};
  cell_output["text"_key] = output;
  cell_output["text_color"_key] = color_hex;
  assign_id(cell_output);

  // Right-click any row for "Copy Entry" (this row's command/exit code/
  // output, via MenuItem.copy_text -- the renderer copies it to the OS
  // clipboard directly on click, no round trip needed) and "Clear Log"
  // (every row, not just this one -- offered from every row's menu purely
  // for discoverability, same as a real log viewer's right-click menu).
  ui_element_ptr context_menu{dynamic::instantiate("wish"_key, "ContextMenu"_key)};
  assign_id(context_menu);

  ui_element_ptr copy_item{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  copy_item["label"_key] = std::string{"Copy Entry"};
  copy_item["copy_text"_key] = command + "\nexit: " + std::to_string(exit_code) + "\n" + output;
  assign_id(copy_item);
  click_handlers_[wish_id_of(copy_item)] = [this] {
    if (status_label_) {
      status_label_["text"_key] = "Copied log entry to clipboard.";
      status_label_["text_color"_key] = std::string{"#98C379FF"};
    }
  };

  ui_element_ptr clear_item{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  clear_item["label"_key] = std::string{"Clear Log"};
  assign_id(clear_item);
  click_handlers_[wish_id_of(clear_item)] = [this] { clear_log_rows(); };

  set_children_list(context_menu, {copy_item, clear_item});
  set_children_list(row, {cell_seq, cell_command, cell_exit, cell_output, context_menu});

  size_t child_key = next_log_child_key_++;
  (*children)[child_key] = dynamic_ptr{row};
  log_rows_.push_back(
      {child_key, wish_id_of(row), wish_id_of(cell_seq), wish_id_of(cell_command), wish_id_of(cell_exit),
       wish_id_of(cell_output)});

  if (log_rows_.size() > kMaxLogRows) {
    erase_log_row_objects(log_rows_.front());
    children->erase(log_rows_.front().child_key);
    log_rows_.pop_front();
  }

  log_table_->refresh_children_order();
}

void git_repo::erase_log_row_objects(const log_row_entry& entry) {
  ctx().objects.erase(entry.row_id.id);
  ctx().objects.erase(entry.cell_seq_id.id);
  ctx().objects.erase(entry.cell_command_id.id);
  ctx().objects.erase(entry.cell_exit_id.id);
  ctx().objects.erase(entry.cell_output_id.id);
}

void git_repo::clear_log_rows() {
  if (!log_table_)
    return;
  auto* children_p = log_table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto& entry : log_rows_) {
    erase_log_row_objects(entry);
    children->erase(entry.child_key);
  }
  log_rows_.clear();
  log_seq_ = 0;
  log_table_->refresh_children_order();

  if (status_label_) {
    status_label_["text"_key] = "Log cleared.";
    status_label_["text_color"_key] = std::string{"#98C379FF"};
  }
}

dynamic git_repo::do_append_command_log(const dynamic& args) {
  append_log_row(
      args.as<std::string>("command"_key), args.as<int32_t>("exit_code"_key), args.as<bool>("ok"_key),
      args.as<std::string>("output"_key));
  return dynamic{};
}

// ── Event routing ─────────────────────────────────────────────────────────────

void git_repo::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_objects_at(files_root_key_);
    remove_objects_at(diff_root_key_);
    remove_objects_at(log_root_key_);
    remove_internal_objects();
    return;
  }

  if (event == "clicked"_key) {
    auto it = click_handlers_.find(id);
    if (it != click_handlers_.end())
      it->second();
    return;
  }

  if (event == "changed"_key) {
    if (id == commit_message_input_id_) {
      commit_message_text_ = payload.as<std::string>("value"_key);
      return;
    }
    if (id == new_branch_input_id_) {
      // Deliberately no EnterReturnsTrue here (unlike e.g. mc.cpp's rename
      // field): this field's "changed" fires per keystroke so
      // new_branch_name_text_ always reflects what's currently typed,
      // which the adjacent "Create" button's click handler (submit_new_
      // branch()) depends on -- EnterReturnsTrue would leave this field
      // (and the button) reading stale/empty text between the last
      // keystroke and an Enter press that may never come, since a plain
      // Button click carries no text payload of its own to fall back on.
      new_branch_name_text_ = payload.as<std::string>("value"_key);
      return;
    }
    auto cbit = checkbox_handlers_.find(id);
    if (cbit != checkbox_handlers_.end()) {
      cbit->second(payload.as<bool>("value"_key));
      return;
    }
    if (payload.as<bool>("selected"_key)) {
      auto selit = selectable_handlers_.find(id);
      if (selit != selectable_handlers_.end())
        selit->second();
    }
    return;
  }

  if (id == graph_table_id_ && (event == "row_selected"_key || event == "row_activated"_key)) {
    select_row(payload.as<int32_t>("index"_key));
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_git() {
  auto proto = dynamic_ptr{"GitRepo"_key, {}};

  proto->addMethod(
      "update_refs"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_update_refs(args);
      }});
  proto->addMethod(
      "update_log"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_update_log(args);
      }});
  proto->addMethod(
      "update_status"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_update_status(args);
      }});
  proto->addMethod(
      "update_commit_files"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_update_commit_files(args);
      }});
  proto->addMethod(
      "update_diff"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_update_diff(args);
      }});
  proto->addMethod(
      "command_result"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_command_result(args);
      }});
  proto->addMethod(
      "append_command_log"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<git_repo&>(self).do_append_command_log(args);
      }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("GitRepo"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "SourceTree-style git GUI: commit graph, branches/tags/stashes/remotes sidebar, working-"
      "directory staging, and a diff viewer. All git invocation happens client-side; this form "
      "only renders whatever snapshot it was last given. Listen for the 'closed' event to detect "
      "when the user is done, and the various '*_requested' events to react to user actions -- "
      "see git.hpp's class doc comment for the full contract."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<git_repo>("wish"_key, "GitRepo"_key));
}

} // namespace bdg::wish
