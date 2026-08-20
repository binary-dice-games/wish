// MIT License © 2025 Binary Dice Games
/// @file file_explorer.cpp
/// @brief Implementation of the FileExplorer form.
#include "file_explorer.hpp"

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
#include <sstream>

namespace bdg::wish {

using namespace bison;
namespace fs = std::filesystem;

// open_in_host_explorer() lives in file_browser_utils.hpp/.cpp -- shared
// with PixViewer's own "Open in Explorer" button. format_bytes()/
// format_modified() are deliberately still duplicated per module (see
// zip_tool.cpp's own format_bytes() doc comment).
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
// Mirrors examples/file_explorer_sample_ui.json (the JSON mock validated
// interactively in the editor) with the example rows removed -- rows are
// built at runtime by fill_table(). ImGuiTableFlags: Resizable(1) +
// RowBg(64) + BordersInnerH(128) + BordersOuterH(256) + Sortable(8) +
// ScrollY(1<<25) = 33554889 (FileDialog's table adds the same Sortable(8)
// bit to its own 33554881). col_name/col_size/col_modified's "column_id"
// (0/1/2) is echoed back in each Table's "sorted" event payload -- see
// on_table_sorted()'s doc comment. InputText EnterReturnsTrue=32 so path
// bars only fire "changed" on Enter, not per keystroke.
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
                  "flags": 32, "width": -1
                },
                "left_selected": { "type": "Label", "text": "Selected: (none)" },
                "left_table": {
                  "type": "Table", "id": "##local_table", "columns": 3, "headers": true,
                  "flags": 33554889, "outer_width": 0, "height": -1,
                  "children": {
                    "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
                    "col_size":     { "type": "TableColumn", "label": "Size", "flags": 16, "init_width": 90, "column_id": 1 },
                    "col_modified": { "type": "TableColumn", "label": "Modified", "flags": 16, "init_width": 130, "column_id": 2 }
                  }
                }
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
                  "flags": 32, "width": -1
                },
                "right_selected": { "type": "Label", "text": "Selected: (none)" },
                "right_table": {
                  "type": "Table", "id": "##sandbox_table", "columns": 3, "headers": true,
                  "flags": 33554889, "outer_width": 0, "height": -1,
                  "children": {
                    "col_name":     { "type": "TableColumn", "label": "Name", "column_id": 0 },
                    "col_size":     { "type": "TableColumn", "label": "Size", "flags": 16, "init_width": 90, "column_id": 1 },
                    "col_modified": { "type": "TableColumn", "label": "Modified", "flags": 16, "init_width": 130, "column_id": 2 }
                  }
                }
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

// ── file_explorer ─────────────────────────────────────────────────────────────

file_explorer::file_explorer(dynamic&& base) : form(std::move(base)) {}

void file_explorer::on_init() {
  internal_root_key_ = next_available_key("__fileexplorer_");

  auto tree = import_json(kLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"File Explorer"};

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("main.panels.left.left_path", [&](const auto& e) {
    left_path_ptr_ = e;
    left_path_id_ = wish_id_of(e);
  });
  tree.with("main.panels.left.left_table", [&](const auto& e) {
    left_table_ptr_ = e;
    left_table_id_ = wish_id_of(e);
  });
  tree.with("main.panels.left.left_selected", [&](const auto& e) { left_selected_ptr_ = e; });
  tree.with("main.panels.right.right_path", [&](const auto& e) {
    right_path_ptr_ = e;
    right_path_id_ = wish_id_of(e);
  });
  tree.with("main.panels.right.right_table", [&](const auto& e) {
    right_table_ptr_ = e;
    right_table_id_ = wish_id_of(e);
  });
  tree.with("main.panels.right.right_selected", [&](const auto& e) { right_selected_ptr_ = e; });
  tree.with(
      "main.panels.right.right_header.open_explorer", [&](const auto& e) { open_explorer_id_ = wish_id_of(e); });
  tree.with("main.panels.middle.upload", [&](const auto& e) { upload_id_ = wish_id_of(e); });
  tree.with("main.panels.middle.download", [&](const auto& e) { download_id_ = wish_id_of(e); });
  tree.with("main.status", [&](const auto& e) { status_label_ptr_ = e; });
  tree.with("main.transfer_progress", [&](const auto& e) { transfer_progress_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);

  // Populate the sandbox panel immediately -- unlike the local panel, this
  // form has direct filesystem access to it, so no client round trip is
  // needed before the right table shows something.
  navigate_sandbox("", sess().resource_dir, sess().allow_absolute_paths);
}

// ── Table population ─────────────────────────────────────────────────────────

void file_explorer::fill_table(
    const ui_element_ptr& table, const std::vector<file_row>& entries, int32_t selected_index) {
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
    row["selected"_key] = idx == selected_index;

    auto make_label = [&](const std::string& text, int32_t order) {
      ui_element_ptr lbl{dynamic::instantiate("wish"_key, "Label"_key)};
      lbl["text"_key] = text;
      lbl["order"_key] = order;
      return lbl;
    };

    std::string display_name = entry.name == ".." ? std::string{".. [Up]"}
        : entry.type == "dir"                     ? ("[" + entry.name + "]")
                                                    : entry.name;

    // Name column shows a small type icon ahead of the label, mirroring
    // file_dialog.cpp's file table (see make_name_cell()'s doc comment).
    ui_element_ptr name_cell = make_name_cell(entry.name, entry.type, display_name);

    auto row_children = dynamic_ptr{key_t{0U}, {}};
    (*row_children)[size_t{0}] = dynamic_ptr{name_cell};
    (*row_children)[size_t{1}] = dynamic_ptr{make_label(entry.type == "dir" ? std::string{} : entry.size, 1)};
    (*row_children)[size_t{2}] = dynamic_ptr{make_label(entry.modified, 2)};
    row["children"_key] = row_children;

    (*children)[static_cast<size_t>(idx)] = dynamic_ptr{row};
    ++idx;
  }
  table->refresh_children_order();
}

void file_explorer::sort_entries(std::vector<file_row>& entries, int32_t sort_column_id, bool ascending) const {
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

void file_explorer::on_table_sorted(
    const dynamic& payload, std::vector<file_row>& entries, const ui_element_ptr& table, int32_t& sort_column_id,
    bool& sort_ascending) {
  auto* col_f = payload.findField<int32_t>("column_id"_key);
  auto* asc_f = payload.findField<bool>("ascending"_key);
  if (!col_f || !asc_f)
    return;
  sort_column_id = *col_f;
  sort_ascending = *asc_f;
  sort_entries(entries, sort_column_id, sort_ascending);
  fill_table(table, entries);
}

void file_explorer::set_status(const std::string& message) {
  (*this)["status"_key] = message;
  if (status_label_ptr_)
    status_label_ptr_["text"_key] = message;
}

bool file_explorer::sandbox_has_file(const std::string& name) const {
  for (auto& e : sandbox_entries_)
    if (e.type == "file" && e.name == name)
      return true;
  return false;
}

bool file_explorer::local_has_file(const std::string& name) const {
  for (auto& e : local_entries_)
    if (e.type == "file" && e.name == name)
      return true;
  return false;
}

// ── Sandbox navigation (server-owned) ────────────────────────────────────────

void file_explorer::navigate_sandbox(
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

  for (auto& dirent : fs::directory_iterator{full, ec}) {
    file_row row;
    row.name = dirent.path().filename().string();
    bool is_dir = dirent.is_directory(ec);
    row.type = is_dir ? "dir" : "file";
    row.size = is_dir ? std::string{} : format_bytes(dirent.file_size(ec));
    auto ftime = dirent.last_write_time(ec);
    row.modified = ec ? std::string{} : format_modified(ftime);
    entries.push_back(std::move(row));
  }

  sandbox_path_ = relative_path;
  sandbox_entries_ = std::move(entries);
  // Re-apply whatever sort column the user last clicked, so navigating
  // away and back doesn't silently drop it (matches Explorer's own
  // persisted-sort behavior).
  sort_entries(sandbox_entries_, sandbox_sort_column_id_, sandbox_sort_ascending_);
  fill_table(right_table_ptr_, sandbox_entries_);

  std::string display = "/" + relative_path;
  if (right_path_ptr_)
    right_path_ptr_["value"_key] = display;
  (*this)["sandbox_path"_key] = relative_path;
  set_status("Ready.");

  selected_sandbox_name_.clear();
  selected_sandbox_is_dir_ = false;
  if (right_selected_ptr_)
    right_selected_ptr_["text"_key] = "Selected: (none)";
}

// ── RMI methods ───────────────────────────────────────────────────────────────

dynamic file_explorer::do_update_local_listing(const dynamic& args) {
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

  sort_entries(local_entries_, local_sort_column_id_, local_sort_ascending_);
  fill_table(left_table_ptr_, local_entries_);
  if (left_path_ptr_)
    left_path_ptr_["value"_key] = local_path_;
  (*this)["local_path"_key] = local_path_;

  selected_local_name_.clear();
  selected_local_is_dir_ = false;
  if (left_selected_ptr_)
    left_selected_ptr_["text"_key] = "Selected: (none)";
  return dynamic{};
}

dynamic file_explorer::do_refresh_sandbox(const dynamic& /*args*/) {
  navigate_sandbox(sandbox_path_, sess().resource_dir, sess().allow_absolute_paths);
  return dynamic{};
}

dynamic file_explorer::on_set(const dynamic& patch) {
  if (auto* v = patch.findField<std::string>("status"_key); v && status_label_ptr_)
    status_label_ptr_["text"_key] = *v;
  if (auto* v = patch.findField<float>("transfer_progress"_key); v && transfer_progress_ptr_)
    transfer_progress_ptr_["value"_key] = *v;
  if (auto* v = patch.findField<std::string>("transfer_label"_key); v && transfer_progress_ptr_)
    transfer_progress_ptr_["label"_key] = *v;
  return patch;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void file_explorer::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == left_table_id_) {
    if (event == "row_selected"_key) {
      int32_t idx = payload.as<int32_t>("index"_key);
      if (idx >= 0 && static_cast<size_t>(idx) < local_entries_.size()) {
        selected_local_name_ = local_entries_[static_cast<size_t>(idx)].name;
        selected_local_is_dir_ = local_entries_[static_cast<size_t>(idx)].type == "dir";
        if (left_selected_ptr_)
          left_selected_ptr_["text"_key] = "Selected: " + selected_local_name_;
        fill_table(left_table_ptr_, local_entries_, idx);
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
      // Row positions change under the new sort order, so the previous
      // selection index no longer points at the same entry -- reset it,
      // mirroring do_update_local_listing()'s own reset-on-reload behavior.
      selected_local_name_.clear();
      selected_local_is_dir_ = false;
      if (left_selected_ptr_)
        left_selected_ptr_["text"_key] = "Selected: (none)";
      on_table_sorted(payload, local_entries_, left_table_ptr_, local_sort_column_id_, local_sort_ascending_);
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
        selected_sandbox_name_ = sandbox_entries_[static_cast<size_t>(idx)].name;
        selected_sandbox_is_dir_ = sandbox_entries_[static_cast<size_t>(idx)].type == "dir";
        if (right_selected_ptr_)
          right_selected_ptr_["text"_key] = "Selected: " + selected_sandbox_name_;
        fill_table(right_table_ptr_, sandbox_entries_, idx);
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
      selected_sandbox_name_.clear();
      selected_sandbox_is_dir_ = false;
      if (right_selected_ptr_)
        right_selected_ptr_["text"_key] = "Selected: (none)";
      on_table_sorted(payload, sandbox_entries_, right_table_ptr_, sandbox_sort_column_id_, sandbox_sort_ascending_);
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
    if (selected_local_name_.empty() || selected_local_is_dir_) {
      set_status("Select a local file to upload.");
      return;
    }
    dynamic req;
    req["name"_key] = selected_local_name_;
    req["local_path"_key] = local_path_;
    // A conflicting target asks the client to confirm (via its own
    // MessageBox, "on_upload_conflict") instead of this form building a
    // second internal modal -- see file_explorer.hpp's class doc comment.
    emit(sandbox_has_file(selected_local_name_) ? "on_upload_conflict"_key : "on_upload_requested"_key, std::move(req));
    return;
  }

  if (id == download_id_ && event == "clicked"_key) {
    if (selected_sandbox_name_.empty() || selected_sandbox_is_dir_) {
      set_status("Select a sandbox file to download.");
      return;
    }
    dynamic req;
    req["name"_key] = selected_sandbox_name_;
    emit(local_has_file(selected_sandbox_name_) ? "on_download_conflict"_key : "on_download_requested"_key, std::move(req));
    return;
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_file_explorer() {
  auto proto = dynamic_ptr{"FileExplorer"_key, {}};

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
        return static_cast<file_explorer&>(self).do_update_local_listing(args);
      }});
  proto->addMethod(
      "refresh_sandbox"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<file_explorer&>(self).do_refresh_sandbox(args);
      }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<file_explorer&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("FileExplorer"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Two-panel file browser: local machine (left, client-driven) vs. session "
                        "sandbox (right, server-driven), with upload/download transfer buttons and "
                        "an \"Open in Explorer\" shortcut for the sandbox side. Listen for "
                        "on_local_navigate/on_upload_requested/on_download_requested to drive the "
                        "client half of the handshake, and 'closed' to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<file_explorer>("wish"_key, "FileExplorer"_key));
}

} // namespace bdg::wish
