// MIT License © 2025 Binary Dice Games
/// @file mc.cpp
/// @brief Implementation of the mc form.
#include "mc.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"
#include "ui/forms/file_browser_utils.hpp"

#include <context/file_service.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>

namespace bdg::wish {

using namespace bison;
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

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// Mirrors examples/mc_sample_ui.json (the JSON mock validated
// interactively in the editor) with the example rows removed -- rows are
// built at runtime by fill_table(). left_table/right_table's "flags" names
// the same full-border set git.cpp/tail.cpp use for their own grid-style
// tables, plus Sortable. Unlike FileDialog's borderless picker list
// (Resizable|RowBg|BordersH|Sortable|ScrollY -- no BordersV), mc's two
// side-by-side panels have no other visual cue marking where each table
// ends: without the outer vertical border, RowBg's alternating shading
// reads as flush against the panel edge with no margin at all, cropped
// rather than framed. Left/right borders fix that. col_name/col_size/
// col_modified's "column_id" (0/1/2) is echoed back in each Table's
// "sorted" event payload -- see on_table_sorted()'s doc comment. InputText
// EnterReturnsTrue so path bars only fire "changed" on Enter, not per
// keystroke.
//
// "left"/"right" each carry both "width": -1 and "height": -1: the former
// makes render_horizontal_layout() give each a real, computed pixel-width
// column (see that function's width pre-scan); the latter opts the same
// column into a real, computed pixel-height child window (that function's
// height pre-scan) instead of the default ImGuiChildFlags_AutoResizeY
// (auto height, sized to content) applied to columns that leave "height"
// unset. Without "height": -1, each column would size to its own content
// regardless of the "panels" row's actual allocated height (itself
// stretch-filled by render_vertical_layout() via "panels"'s own
// "height": -1) -- leaving a gap between the panels and the status bar
// when content is shorter than the row, or an unwanted extra scrollbar on
// the row when content is taller. left_table/right_table mirror this with
// their own "height": -1 (rather than a fixed pixel value) so each Table
// fills exactly the remaining space inside its column, letting ImGui's
// own ScrollY engage per-panel only when a listing doesn't fit -- no
// "outer_height" override needed, since 0 (the default) already means
// "fill the remaining space in the parent", and the parent here is now a
// real fixed-size child window rather than an auto-sizing one.
//
// "middle" (the upload/download button column) carries "height": -1 for the
// same reason as "left"/"right" -- a real fixed-height child window, not one
// auto-sized to its two buttons -- but for a different purpose: it gives the
// Spring elements flanking "upload"/"download" somewhere to actually expand
// into. Two Springs around one child centers it (see docs/ui-elements.md's
// Spring section); here they flank the *pair*, so the buttons stay adjacent
// (spacing: 10 between them) while sliding as a group to the vertical center
// of the column, rather than sitting pinned at the top like every other
// auto-height column would.
//
// "left_stats"/"left_disk" (and their "right_" twins) are auto-height Labels
// added *after* left_table/right_table -- render_vertical_layout()'s measure
// pass sizes them to their own natural single-line height first, and only
// hands left_table/right_table's height:-1 stretch share whatever's left, so
// they read as a small summary strip pinned to the bottom of each panel
// without stealing a fixed chunk of the table's own scrollable area. "_stats"
// holds "<N> files, <size>" for the currently-listed directory
// (non-recursive: just the files in view, not their subdirectories'
// contents); "_disk" holds the used/free/total space of the filesystem that
// directory lives on. Both start blank and are filled in by
// do_update_local_listing() (client-reported, since only the client can see
// the local machine's disk) and navigate_sandbox() (computed directly via
// std::filesystem, since the sandbox lives on this machine).

// Tagged delimiter (R"json(...)json") rather than the untagged R"(...)"
// convention used elsewhere: "Sandbox (Server)" ends in a ")" immediately
// followed by the JSON string's closing quote, which is exactly the byte
// sequence R"(...)" treats as its own terminator -- an untagged literal
// would truncate here.
static constexpr const char* kLayout = R"json({
  "type": "Window",
  "width": 920, "height": 540,
  "closable": true,
  "children": {
    "main": {
      "type": "VerticalLayout",
      "children": {
        "panels": {
          "type": "HorizontalLayout",
          "height": -1,
          "spacing": 10,
          "children": {
            "left": {
              "type": "VerticalLayout",
              "spacing": 4,
              "width": -1, "height": -1,
              "children": {
                "left_label": { "type": "Label", "text": "Local Machine" },
                "left_path": {
                  "type": "InputText", "hint": "Local path...", "value": "",
                  "flags": "EnterReturnsTrue", "width": -1
                },
                "left_selected": { "type": "Label", "text": "Selected: (none)" },
                "left_table": {
                  "type": "Table", "id": "##local_table", "columns": 3, "headers": true,
                  "flags": "Resizable|RowBg|Borders|Sortable|ScrollY",
                  "outer_width": 0, "height": -1,
                  "children": {
                    "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
                    "col_size":     { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 90, "column_id": 1 },
                    "col_modified": { "type": "TableColumn", "label": "Modified", "flags": "WidthFixed", "init_width": 130, "column_id": 2 }
                  }
                },
                "left_stats": { "type": "Label", "text": "" },
                "left_disk":  { "type": "Label", "text": "" }
              }
            },
            "middle": {
              "type": "VerticalLayout",
              "spacing": 10,
              "width": 80, "height": -1,
              "children": {
                "middle_spring_top":    { "type": "Spring" },
                "upload":   { "type": "Button", "label": ">>", "width": 60, "height": 36 },
                "download": { "type": "Button", "label": "<<", "width": 60, "height": 36 },
                "middle_spring_bottom": { "type": "Spring" }
              }
            },
            "right": {
              "type": "VerticalLayout",
              "spacing": 4,
              "width": -1, "height": -1,
              "children": {
                "right_header": {
                  "type": "HorizontalLayout",
                  "spacing": 8,
                  "children": {
                    "right_label": { "type": "Label", "text": "Sandbox (Server)" },
                    "open_explorer": { "type": "Button", "label": "Open in Explorer" }
                  }
                },
                "right_path": {
                  "type": "InputText", "hint": "Sandbox path...", "value": "/",
                  "flags": "EnterReturnsTrue", "width": -1
                },
                "right_selected": { "type": "Label", "text": "Selected: (none)" },
                "right_table": {
                  "type": "Table", "id": "##sandbox_table", "columns": 3, "headers": true,
                  "flags": "Resizable|RowBg|Borders|Sortable|ScrollY",
                  "outer_width": 0, "height": -1,
                  "children": {
                    "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
                    "col_size":     { "type": "TableColumn", "label": "Size", "flags": "WidthFixed", "init_width": 90, "column_id": 1 },
                    "col_modified": { "type": "TableColumn", "label": "Modified", "flags": "WidthFixed", "init_width": 130, "column_id": 2 }
                  }
                },
                "right_stats": { "type": "Label", "text": "" },
                "right_disk":  { "type": "Label", "text": "" }
              }
            }
          }
        },
        "status": { "type": "Label", "text": "Ready." },
        "transfer_progress": { "type": "ProgressBar", "value": 0.0, "label": "", "width": -1 }
      }
    }
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

// Properties dialog -- shared by both panels; every field is already known
// server-side by show-time (see mc.hpp's class doc comment), unlike top.cpp's
// Properties dialog which has to wait on a client round trip.
// "vbox" wraps grid/sep/close_row (rather than leaving them direct Window
// children) so grid's "height": -1 has a VerticalLayout parent to actually
// read that stretch hint from -- Window itself doesn't distribute space to
// its children the way VerticalLayout/HorizontalLayout do, so an unwrapped
// grid would only ever auto-size to its own 5 labels' natural height,
// leaving dead space below close_row whenever the (user-resizable) window
// is taller than that. kRenameLayout above uses the identical pattern.
static constexpr const char* kPropertiesLayout = R"json({
  "type": "Window", "title": "Properties", "modal": true,
  "flags": "NoCollapse",
  "width": 480, "height": 280,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "grid": {
          "type": "VerticalLayout",
          "height": -1,
          "children": {
            "name_row": { "type": "Label", "text": "" },
            "type_row": { "type": "Label", "text": "" },
            "size_row": { "type": "Label", "text": "" },
            "modified_row": { "type": "Label", "text": "" },
            "path_row": { "type": "Label", "text": "", "wrap": true }
          }
        },
        "sep": { "type": "Separator" },
        "close_row": { "type": "HorizontalLayout", "children": {
          "btn_close": { "type": "Button", "label": "Close", "height": 32 }
        } }
      }
    }

  }
})json";

// ── mc ─────────────────────────────────────────────────────────────

mc::mc(dynamic&& base) : form(std::move(base)) {}

void mc::on_init() {
  internal_root_key_ = next_available_key("__mc_");

  auto ui_tree = import_json(kLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*ui_tree[""])["title"_key] = title_f ? *title_f : std::string{"File Explorer"};

  auto& c = ctx();
  for (auto& [key, elem] : ui_tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*ui_tree[""])["__wish_id"_key].as<key_t>();
  ui_tree.with("main.panels.left.left_path", [&](const auto& e) {
    left_path_ptr_ = e;
    left_path_id_ = wish_id_of(e);
  });
  ui_tree.with("main.panels.left.left_table", [&](const auto& e) {
    left_table_ptr_ = e;
    left_table_id_ = wish_id_of(e);
  });
  ui_tree.with("main.panels.left.left_selected", [&](const auto& e) { left_selected_ptr_ = e; });
  ui_tree.with("main.panels.left.left_stats", [&](const auto& e) { left_stats_ptr_ = e; });
  ui_tree.with("main.panels.left.left_disk", [&](const auto& e) { left_disk_ptr_ = e; });
  ui_tree.with("main.panels.right.right_path", [&](const auto& e) {
    right_path_ptr_ = e;
    right_path_id_ = wish_id_of(e);
  });
  ui_tree.with("main.panels.right.right_table", [&](const auto& e) {
    right_table_ptr_ = e;
    right_table_id_ = wish_id_of(e);
  });
  ui_tree.with("main.panels.right.right_selected", [&](const auto& e) { right_selected_ptr_ = e; });
  ui_tree.with("main.panels.right.right_stats", [&](const auto& e) { right_stats_ptr_ = e; });
  ui_tree.with("main.panels.right.right_disk", [&](const auto& e) { right_disk_ptr_ = e; });
  ui_tree.with(
      "main.panels.right.right_header.open_explorer", [&](const auto& e) { open_explorer_id_ = wish_id_of(e); });
  ui_tree.with("main.panels.middle.upload", [&](const auto& e) { upload_id_ = wish_id_of(e); });
  ui_tree.with("main.panels.middle.download", [&](const auto& e) { download_id_ = wish_id_of(e); });
  ui_tree.with("main.status", [&](const auto& e) { status_label_ptr_ = e; });
  ui_tree.with("main.transfer_progress", [&](const auto& e) { transfer_progress_ptr_ = e; });

  sess().ui_objects.merge(std::move(ui_tree), internal_root_key_);

  // Populate the sandbox panel immediately -- unlike the local panel, this
  // form has direct filesystem access to it, so no client round trip is
  // needed before the right table shows something.
  navigate_sandbox("", sess().resource_dir, sess().allow_absolute_paths);
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
    ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
    row["order"_key] = idx;
    row["selected"_key] = selected_names.count(entry.name) > 0;

    auto make_label = [&](const std::string& text, int32_t order) {
      ui_element_ptr lbl{dynamic::instantiate("wish"_key, "Label"_key)};
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

  ui_element_ptr menu{dynamic::instantiate("wish"_key, "ContextMenu"_key)};
  assign_id(menu);

  ui_element_ptr properties{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  properties["label"_key] = std::string{"Properties"};
  menu_targets[assign_id(properties)] = row_menu_target{row_menu_action::properties, is_sandbox, entry.name};

  ui_element_ptr rename{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  rename["label"_key] = std::string{"Rename..."};
  menu_targets[assign_id(rename)] = row_menu_target{row_menu_action::rename, is_sandbox, entry.name};

  ui_element_ptr sep{dynamic::instantiate("wish"_key, "Separator"_key)};
  assign_id(sep);

  ui_element_ptr copy_path{dynamic::instantiate("wish"_key, "MenuItem"_key)};
  copy_path["label"_key] = std::string{"Copy Path"};
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

void mc::set_status(const std::string& message) {
  (*this)["status"_key] = message;
  if (status_label_ptr_)
    status_label_ptr_["text"_key] = message;
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
    std::string relative_path, const fs::path& resource_dir, bool allow_absolute_paths) {
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
      set_status("Invalid or out-of-sandbox path.");
      return;
    }
  }

  std::error_code ec;
  if (!fs::is_directory(full, ec)) {
    set_status("Not a directory: " + relative_path);
    return;
  }

  std::vector<file_row> entries;
  if (!relative_path.empty())
    entries.push_back({"..", "dir", "", ""});

  uintmax_t file_count = 0;
  uintmax_t total_bytes = 0;
  for (auto& dirent : fs::directory_iterator{full, ec}) {
    file_row row;
    row.name = dirent.path().filename().string();
    bool is_dir = dirent.is_directory(ec);
    row.type = is_dir ? "dir" : "file";
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
  set_status("Ready.");

  if (right_selected_ptr_)
    right_selected_ptr_["text"_key] = describe_selection(selected_sandbox_names_);
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
    set_status("Invalid name.");
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
      set_status("Invalid or out-of-sandbox path.");
    } else if (fs::exists(new_full, ec)) {
      set_status("\"" + new_name + "\" already exists.");
    } else {
      fs::rename(old_full, new_full, ec);
      if (ec) {
        set_status("Rename failed: " + ec.message());
      } else {
        navigate_sandbox(sandbox_path_, s->resource_dir, s->allow_absolute_paths);
        set_status("Renamed.");
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
    set_status("Renaming...");
  }

  request_close_rename();
}

// ── Properties dialog ─────────────────────────────────────────────────────────

void mc::show_properties_dialog(bool is_sandbox, const file_row& entry) {
  if (!properties_root_key_.empty())
    remove_properties_objects();

  auto tree = import_json(kPropertiesLayout);
  (*tree[""])["title"_key] = "Properties - " + entry.name;

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  properties_window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.close_row.btn_close", [&](const auto& e) { properties_close_id_ = wish_id_of(e); });
  tree.with("vbox.grid.name_row", [&](const auto& e) { properties_name_ptr_ = e; });
  tree.with("vbox.grid.type_row", [&](const auto& e) { properties_type_ptr_ = e; });
  tree.with("vbox.grid.size_row", [&](const auto& e) { properties_size_ptr_ = e; });
  tree.with("vbox.grid.modified_row", [&](const auto& e) { properties_modified_ptr_ = e; });
  tree.with("vbox.grid.path_row", [&](const auto& e) { properties_path_ptr_ = e; });

  // Same path-composition rule as fill_table()'s Copy Path -- see that
  // call site for why the sandbox side stays relative rather than exposing
  // the server's absolute filesystem layout to the client.
  std::string path_display = is_sandbox
      ? "/" + (sandbox_path_.empty() ? entry.name : sandbox_path_ + "/" + entry.name)
      : (local_path_.empty() ? entry.name : (fs::path(local_path_) / entry.name).string());

  if (properties_name_ptr_)
    properties_name_ptr_["text"_key] = "Name: " + entry.name;
  if (properties_type_ptr_)
    properties_type_ptr_["text"_key] = std::string{"Type: "} + (entry.type == "dir" ? "Folder" : "File");
  if (properties_size_ptr_)
    properties_size_ptr_["text"_key] = entry.type == "dir" ? std::string{"Size: --"} : "Size: " + entry.size;
  if (properties_modified_ptr_) {
    properties_modified_ptr_["text"_key] =
        "Modified: " + (entry.modified.empty() ? std::string{"(unknown)"} : entry.modified);
  }
  if (properties_path_ptr_)
    properties_path_ptr_["text"_key] = "Path: " + path_display;

  // Same outside-dispatch secondary-root merge as show_rename_dialog() above.
  auto lock = context_wlock{*sync_ctx_};
  context& s = *lock;
  for (int i = 0;; ++i) {
    std::string candidate = "__mc_properties_" + std::to_string(i);
    if (s.top_level_objects.find(key_t{candidate}) == s.top_level_objects.end()) {
      properties_root_key_ = candidate;
      break;
    }
  }
  s.ui_objects.merge(std::move(tree), properties_root_key_);
  auto it = s.ui_objects.find(properties_root_key_);
  if (it != s.ui_objects.end()) {
    s.top_level_objects[key_t{properties_root_key_}] = it->second;
    (*it->second)["__path__"_key] = properties_root_key_;
    s.top_level_handlers[key_t{properties_root_key_}] = this;
  }
}

void mc::request_close_properties() {
  request_close_at(properties_root_key_);
}

void mc::remove_properties_objects() {
  remove_objects_at(properties_root_key_);
  properties_root_key_.clear();
  properties_name_ptr_.reset();
  properties_type_ptr_.reset();
  properties_size_ptr_.reset();
  properties_modified_ptr_.reset();
  properties_path_ptr_.reset();
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
  return dynamic{};
}

dynamic mc::do_refresh_sandbox(const dynamic& /*args*/) {
  navigate_sandbox(sandbox_path_, sess().resource_dir, sess().allow_absolute_paths);
  return dynamic{};
}

dynamic mc::on_set(const dynamic& patch) {
  if (auto* v = patch.findField<std::string>("status"_key); v && status_label_ptr_)
    status_label_ptr_["text"_key] = *v;
  if (auto* v = patch.findField<float>("transfer_progress"_key); v && transfer_progress_ptr_)
    transfer_progress_ptr_["value"_key] = *v;
  if (auto* v = patch.findField<std::string>("transfer_label"_key); v && transfer_progress_ptr_)
    transfer_progress_ptr_["label"_key] = *v;
  return patch;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void mc::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
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
      dynamic nav;
      nav["name"_key] = entry.name;
      nav["type"_key] = std::string{"dir"};
      emit("on_local_navigate"_key, std::move(nav));
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
    if (auto* v = payload.findField<std::string>("value"_key)) {
      dynamic nav;
      nav["name"_key] = *v;
      nav["type"_key] = std::string{"path"};
      emit("on_local_navigate"_key, std::move(nav));
    }
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
      navigate_sandbox(target, s->resource_dir, s->allow_absolute_paths);
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
      navigate_sandbox(p, s->resource_dir, s->allow_absolute_paths);
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
      set_status("Could not open host file explorer.");
    else
      set_status("Opened in host file explorer.");
    return;
  }

  if (id == upload_id_ && event == "clicked"_key) {
    auto files = selected_file_names(selected_local_names_, local_entries_);
    if (files.empty()) {
      set_status("Select a local file to upload.");
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
      emit("on_upload_requested"_key, std::move(req));
    }
    if (!conflicts.empty()) {
      dynamic req = make_names_payload(conflicts);
      req["local_path"_key] = local_path_;
      emit("on_upload_conflict"_key, std::move(req));
    }
    return;
  }

  if (id == download_id_ && event == "clicked"_key) {
    auto files = selected_file_names(selected_sandbox_names_, sandbox_entries_);
    if (files.empty()) {
      set_status("Select a sandbox file to download.");
      return;
    }
    std::vector<std::string> ready, conflicts;
    for (auto& name : files)
      (local_has_file(name) ? conflicts : ready).push_back(name);
    if (!ready.empty())
      emit("on_download_requested"_key, make_names_payload(ready));
    if (!conflicts.empty())
      emit("on_download_conflict"_key, make_names_payload(conflicts));
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
          set_status("Copied path for \"" + target.name + "\" to clipboard.");
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

  if (!properties_root_key_.empty()) {
    if (id == properties_window_id_ && event == "closed"_key) {
      remove_properties_objects();
      return;
    }
    if (id == properties_close_id_ && event == "clicked"_key) {
      request_close_properties();
      return;
    }
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_mc() {
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
          attr<Description>("Client-owned local directory currently shown in the left panel. "
                            "Updated via update_local_listing(); read-only from the client's "
                            "perspective otherwise."),
          attr<Category>("Data")});

  proto->addField(
      "sandbox_path"_key,
      field{
          std::string{""},
          attr<DisplayName>("Sandbox Path"),
          attr<Description>("Server-owned sandbox directory currently shown in the right panel, "
                            "relative to the session sandbox root (\"\" == root)."),
          attr<Category>("Data")});

  proto->addField(
      "status"_key,
      field{
          std::string{"Ready."},
          attr<DisplayName>("Status"),
          attr<Description>("Text shown in the status bar at the bottom of the window."),
          attr<Category>("Data")});

  proto->addField(
      "transfer_progress"_key,
      field{
          0.0f,
          attr<DisplayName>("Transfer Progress"),
          attr<Description>("Fill fraction (0..1) of the transfer progress bar. The client drives "
                            "this while an upload/download is in flight."),
          attr<Category>("Data")});

  proto->addField(
      "transfer_label"_key,
      field{
          std::string{""},
          attr<DisplayName>("Transfer Label"),
          attr<Description>("Text overlaid on the transfer progress bar."),
          attr<Category>("Data")});

  proto->addMethod(
      "update_local_listing"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<mc&>(self).do_update_local_listing(args);
      }});
  proto->addMethod(
      "refresh_sandbox"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<mc&>(self).do_refresh_sandbox(args);
      }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<mc&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Mc"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Two-panel file browser: local machine (left, client-driven) vs. session "
                        "sandbox (right, server-driven), with upload/download transfer buttons and "
                        "an \"Open in Explorer\" shortcut for the sandbox side. Listen for "
                        "on_local_navigate/on_upload_requested/on_download_requested to drive the "
                        "client half of the handshake, and 'closed' to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<mc>("wish"_key, "Mc"_key));
}

} // namespace bdg::wish
