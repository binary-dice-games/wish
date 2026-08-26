// MIT License © 2025 Binary Dice Games
/// @file pix.cpp
/// @brief Implementation of the PixViewer form.
#include "pix.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"
#include "ui/forms/file_browser_utils.hpp"

#include <context/file_service.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace bdg::wish {

using namespace bison;
namespace fs = std::filesystem;

namespace {

// Thumbnail grid: kGridColumns cells per row (see kPixLayout's grid_table,
// which must declare exactly that many TableColumn children); each thumbnail
// is kThumbPx square.
constexpr int32_t kGridColumns = 3;
constexpr float kThumbPx = 84.0f;
// Extra height reserved below the thumbnail for its filename caption, on
// top of kThumbPx -- see rebuild_grid()'s per-cell Selectable, which needs
// an explicit height covering both (see render_selectable()'s
// children-overlay doc comment in imgui_ui_renderer.cpp for why 0/auto
// isn't enough here).
constexpr float kCellCaptionPx = 24.0f;

// Largest w x h that fits within kThumbPx x kThumbPx while preserving
// src_w:src_h's aspect ratio (never upscaling) -- mirrors client/pix.cpp's
// identical fit_dims() helper, used there to size the generated thumbnail
// file itself; this one instead sizes the Image element that *displays*
// it, so a non-square thumbnail isn't stretched back out to a square grid
// cell. See do_set_thumbnail()'s doc comment.
void fit_dims(int32_t src_w, int32_t src_h, float max_w, float max_h, int32_t& out_w, int32_t& out_h) {
  if (src_w <= 0 || src_h <= 0) {
    out_w = out_h = 0;
    return;
  }
  double scale = std::min({1.0, static_cast<double>(max_w) / src_w, static_cast<double>(max_h) / src_h});
  out_w = std::max(1, static_cast<int32_t>(src_w * scale));
  out_h = std::max(1, static_cast<int32_t>(src_h * scale));
}

// kPixLayout's grid_table's "flags": "RowBg|Borders|ScrollY|NoPadInnerX"
// ("Borders" is the BordersH|BordersV composite -- see table.cpp's flags
// map -- so both cell grid lines render, not just the horizontal ones;
// "NoPadInnerX" drops the gap ImGui otherwise reserves between adjacent
// columns' content, so thumbnails sit close together like a real file
// browser's icon grid instead of visibly separated).
// preview_table adds "ScrollX" on top: "RowBg|Borders|ScrollX|ScrollY" --
// see pix.hpp's class comment for why the preview panel is a scrollable
// Table rather than a plain Image: its native scrollbars are the pan
// control once "zoom" grows the Image past the viewport (see grid_table/
// preview_table's own "width"/"height": -1 fields, just below, for how that
// viewport now tracks its enclosing pane's size instead of a fixed pixel
// box). Every TableColumn's own "flags": "WidthFixed" is required for
// init_width to take effect at all (ImGui errors "can only specify
// width/weight if sizing policy is set explicitly" otherwise); mirrors
// file_dialog.cpp's col_name/col_type columns.
// kPixLayout's path_input "flags": "EnterReturnsTrue" (fire "changed" only
// on Enter, not per keystroke).
//
// wish_id_of() is declared in file_browser_utils.hpp (included above) --
// not redeclared here, to avoid an ambiguous redefinition in this
// translation unit.

// Pre-C++20-portable file_time_type -> Unix-epoch-seconds conversion (no
// std::chrono::clock_cast, whose libstdc++ availability lags MSVC's).
// Truncated to int32_t when stored in a dynamic field -- see
// do_stat_files()'s own comment.
int64_t file_time_to_unix_seconds(const fs::file_time_type& ftime) {
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return static_cast<int64_t>(std::chrono::system_clock::to_time_t(sctp));
}

} // namespace

// ── Hardcoded UI layout ───────────────────────────────────────────────────────
//
// "body" is a plain HorizontalLayout: left_panel's own "width": 320 is a
// fixed pixel size (not a resizable Splitter pane -- the grid's column
// count is itself fixed at kGridColumns, so a user-resizable left_panel
// would just grow/shrink dead margin around the same 3 columns rather than
// showing more of them; see rebuild_grid()'s own comment on why the column
// count doesn't adapt to available width), sized to comfortably fit those
// 3 fixed-width columns plus borders/scrollbar. right_panel's "width": -1
// makes it the sole stretch column, so it takes all remaining horizontal
// space. Both panels (and "body" itself) carry "height": -1 so they
// stretch-fill whatever vertical space toolbar/status_label don't use --
// unlike a Splitter, a plain HorizontalLayout only gives a child the full
// cross-axis extent when that child's own "height" hint asks for it (see
// Layout.height's doc comment in docs/ui-elements.md), hence setting it on
// both. grid_table/preview_table each carry "width"/"height": -1 in turn
// so they fill their own panel rather than sitting at a fixed pixel size --
// still independently scrollable regardless of the enclosing panel's own
// auto-sizing, same as a fixed size would give, just now responsive to
// window resizing. See pix.hpp's class comment for why a Table (not a
// bespoke scroll widget) is used for both the thumbnail grid and the
// pannable preview viewport.
// R"json(...)json" (not the plain R"(...)" delimiter) because the pan hint
// label's text ends in a literal ")" -- "pan)" immediately followed by the
// JSON string's closing '"' would otherwise spell out the plain delimiter's
// own ')"' terminator early and silently truncate/corrupt the rest of this
// translation unit (see modules/bdg/dev/editor's kEditorLayout for the same
// gotcha).
static constexpr const char* kPixLayout = R"json({
  "type": "Window",
  "title": "Image Viewer",
  "width": 1000, "height": 700,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "spacing": 6.0,
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 8.0,
          "children": {
            "path_input": {
              "type": "InputText", "value": "", "hint": "Folder path...",
              "max_length": 4096, "flags": "EnterReturnsTrue", "width": 520.0
            },
            "btn_browse": { "type": "Button", "label": "Browse...", "width": 100 },
            "btn_open_explorer": { "type": "Button", "label": "Open Sandbox in Explorer", "width": 220 }
          }
        },
        "status_label": { "type": "Label", "text": "" },
        "body": {
          "type": "HorizontalLayout",
          "spacing": 8.0,
          "height": -1,
          "children": {
            "left_panel": {
              "type": "VerticalLayout",
              "width": 320,
              "height": -1,
              "children": {
                "grid_table": {
                  "type": "Table",
                  "columns": 3,
                  "flags": "RowBg|BordersOuter|ScrollY",
                  "width": -1,
                  "height": -1,
                  "headers": false,
                  "children": {
                    "col0": { "type": "TableColumn", "flags": "WidthFixed", "init_width": 100 },
                    "col1": { "type": "TableColumn", "flags": "WidthFixed", "init_width": 100 },
                    "col2": { "type": "TableColumn", "flags": "WidthFixed", "init_width": 100 }
                  }
                }
              }
            },
            "right_panel": {
              "type": "VerticalLayout",
              "width": -1,
              "height": -1,
              "spacing": 4.0,
              "children": {
                "zoom_bar": {
                  "type": "HorizontalLayout",
                  "spacing": 6.0,
                  "children": {
                    "btn_zoom_out": { "type": "Button", "label": "-", "width": 32 },
                    "zoom_label": { "type": "Label", "text": "--" },
                    "btn_zoom_in": { "type": "Button", "label": "+", "width": 32 },
                    "btn_zoom_fit": { "type": "Button", "label": "Fit", "width": 56 },
                    "btn_zoom_100": { "type": "Button", "label": "100%", "width": 56 },
                    "pan_hint": { "type": "Label", "text": "(scroll the preview to pan)" }
                  }
                },
                "preview_table": {
                  "type": "Table",
                  "columns": 1,
                  "flags": "RowBg|ScrollX|ScrollY",
                  "width": -1,
                  "height": -1,
                  "headers": false,
                  "children": {
                    "pcol0": { "type": "TableColumn", "flags": "WidthFixed", "init_width": 600 },
                    "prow0": {
                      "type": "TableRow",
                      "children": {
                        "preview_image": { "type": "Image", "src": "", "width": 0, "height": 0 }
                      }
                    }
                  }
                },
                "info_panel": {
                  "type": "VerticalLayout",
                  "spacing": 2.0,
                  "children": {
                    "info_filename": { "type": "Label", "text": "" },
                    "info_resolution": { "type": "Label", "text": "" },
                    "info_format": { "type": "Label", "text": "" },
                    "info_size": { "type": "Label", "text": "" },
                    "info_modified": { "type": "Label", "text": "" }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})json";

// ── pix_viewer ────────────────────────────────────────────────────────────────

pix_viewer::pix_viewer(dynamic&& base) : form(std::move(base)) {}

void pix_viewer::on_init() {
  // See internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__pix_");

  auto tree = import_json(kPixLayout);

  // Assign each imported element a bison RMI ID so the renderer can emit
  // events with the correct object ID -- same pattern as every other form.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();

  tree.with("vbox.toolbar.path_input", [&](const auto& e) {
    path_input_ptr_ = e;
    path_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.toolbar.btn_browse", [&](const auto& e) { btn_browse_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_open_explorer", [&](const auto& e) { btn_open_explorer_id_ = wish_id_of(e); });
  tree.with("vbox.status_label", [&](const auto& e) { status_label_ptr_ = e; });
  tree.with("vbox.body.left_panel.grid_table", [&](const auto& e) { grid_table_ptr_ = e; });
  tree.with("vbox.body.right_panel.zoom_bar.zoom_label", [&](const auto& e) { zoom_label_ptr_ = e; });
  tree.with("vbox.body.right_panel.zoom_bar.btn_zoom_out", [&](const auto& e) { btn_zoom_out_id_ = wish_id_of(e); });
  tree.with("vbox.body.right_panel.zoom_bar.btn_zoom_in", [&](const auto& e) { btn_zoom_in_id_ = wish_id_of(e); });
  tree.with("vbox.body.right_panel.zoom_bar.btn_zoom_fit", [&](const auto& e) { btn_zoom_fit_id_ = wish_id_of(e); });
  tree.with("vbox.body.right_panel.zoom_bar.btn_zoom_100", [&](const auto& e) { btn_zoom_100_id_ = wish_id_of(e); });
  tree.with(
      "vbox.body.right_panel.preview_table.prow0.preview_image", [&](const auto& e) { preview_image_ptr_ = e; });
  tree.with("vbox.body.right_panel.preview_table.pcol0", [&](const auto& e) { preview_col_ptr_ = e; });
  tree.with("vbox.body.right_panel.info_panel.info_filename", [&](const auto& e) { info_filename_ptr_ = e; });
  tree.with("vbox.body.right_panel.info_panel.info_resolution", [&](const auto& e) { info_resolution_ptr_ = e; });
  tree.with("vbox.body.right_panel.info_panel.info_format", [&](const auto& e) { info_format_ptr_ = e; });
  tree.with("vbox.body.right_panel.info_panel.info_size", [&](const auto& e) { info_size_ptr_ = e; });
  tree.with("vbox.body.right_panel.info_panel.info_modified", [&](const auto& e) { info_modified_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

// ── grid rebuilding ──────────────────────────────────────────────────────────

void pix_viewer::rebuild_grid() {
  if (!grid_table_ptr_)
    return;
  auto* children_p = grid_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  // Remove previous row entries (indexed); named TableColumn children remain.
  children->clear();
  next_child_key_ = 0;
  selectable_id_to_index_.clear();
  selected_index_ = kNoSelection;

  size_t row_count = (images_.size() + kGridColumns - 1) / kGridColumns;
  for (size_t row_idx = 0; row_idx < row_count; ++row_idx) {
    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row["order"_key] = static_cast<int32_t>(row_idx);

    auto row_children = dynamic_ptr{key_t{0U}, {}};
    for (int32_t col = 0; col < kGridColumns; ++col) {
      size_t idx = row_idx * kGridColumns + static_cast<size_t>(col);
      ui_element_ptr cell;
      if (idx < images_.size()) {
        auto& entry = images_[idx];

        // Caption first (order 0, fixed at the cell's top edge) and image
        // second (order 1, below it): a landscape/portrait thumbnail's
        // aspect-fit height (see do_set_thumbnail()) varies cell to cell,
        // so anchoring the caption below a variable-height image would
        // leave captions at different y positions across the row.
        // Anchoring it above instead keeps every caption in a row (and
        // across rows) aligned to the same y, matching a real file
        // browser's grid look.
        ui_element_ptr name_label = ui_element_ptr::create("wish"_key, "Label"_key);
        name_label["text"_key] = entry.name;
        name_label["order"_key] = int32_t{0};

        ui_element_ptr img = ui_element_ptr::create("wish"_key, "Image"_key);
        img["src"_key] = std::string{"res/icons/image.png"};
        img["width"_key] = static_cast<int32_t>(kThumbPx);
        img["height"_key] = static_cast<int32_t>(kThumbPx);
        img["order"_key] = int32_t{1};

        // The Selectable itself is the cell: img/name_label render as its
        // overlay children (see render_selectable()'s doc comment), so
        // clicking the thumbnail image selects the entry just as clicking
        // its filename caption already did.
        ui_element_ptr sel = ui_element_ptr::create("wish"_key, "Selectable"_key);
        sel["label"_key] = std::string{};
        sel["width"_key] = kThumbPx;
        sel["height"_key] = kThumbPx + kCellCaptionPx;
        sel["order"_key] = col;

        key_t sel_id = rmi::shared::generate_id();
        ctx().put_object(sel_id, sel);
        sel["__wish_id"_key] = sel_id;

        auto sel_children = dynamic_ptr{key_t{0U}, {}};
        (*sel_children)[size_t{0}] = dynamic_ptr{name_label};
        (*sel_children)[size_t{1}] = dynamic_ptr{img};
        sel["children"_key] = sel_children;
        sel->refresh_children_order();

        entry.cell_ptr = sel;
        entry.image_ptr = img;
        entry.selectable_ptr = sel;
        entry.selectable_id = sel_id;
        selectable_id_to_index_[sel_id.id] = idx;

        cell = sel;
      } else {
        // Pad the last row so every TableRow has exactly kGridColumns cells.
        cell = ui_element_ptr::create("wish"_key, "Label"_key);
        cell["text"_key] = std::string{};
        cell["order"_key] = col;
      }

      (*row_children)[static_cast<size_t>(col)] = dynamic_ptr{cell};
    }
    row["children"_key] = row_children;

    (*children)[static_cast<size_t>(next_child_key_++)] = dynamic_ptr{row};
  }

  grid_table_ptr_->refresh_children_order();
}

void pix_viewer::select_index(size_t index) {
  if (selected_index_ != kNoSelection && selected_index_ < images_.size())
    (*images_[selected_index_].selectable_ptr)["selected"_key] = false;

  selected_index_ = index;
  if (selected_index_ != kNoSelection && selected_index_ < images_.size())
    (*images_[selected_index_].selectable_ptr)["selected"_key] = true;

  if (selected_index_ == kNoSelection || selected_index_ >= images_.size())
    return;

  dynamic payload;
  payload["name"_key] = images_[selected_index_].name;
  emit("on_image_selected"_key, std::move(payload));
}

// ── RMI methods ───────────────────────────────────────────────────────────────

dynamic pix_viewer::do_set_images(const dynamic& args) {
  images_.clear();
  if (auto* imgs = args.findField<dynamic_ptr>("images"_key); imgs && *imgs) {
    (**imgs).forEach([&](key_t, const field& entry_field) {
      auto* ep = entry_field.get<dynamic_ptr>();
      if (!ep || !*ep)
        return;
      grid_entry e;
      e.name = (**ep).as<std::string>("name"_key);
      images_.push_back(std::move(e));
    });
  }
  rebuild_grid();

  if (preview_image_ptr_)
    (*preview_image_ptr_)["src"_key] = std::string{};
  if (zoom_label_ptr_)
    (*zoom_label_ptr_)["text"_key] = std::string{"--"};
  do_set_info(dynamic{});
  return dynamic{};
}

dynamic pix_viewer::do_set_thumbnail(const dynamic& args) {
  auto name = args.as<std::string>("name"_key);
  auto thumb_path = args.as<std::string>("thumb_path"_key);
  // "width"/"height" (optional): the uploaded thumbnail file's own actual
  // pixel size -- already aspect-preserving (client/pix.cpp's
  // generate_and_upload_thumbnail() fits it within a square before
  // encoding), but that square may itself not be kThumbPx x kThumbPx (see
  // client/pix.cpp's kThumbMax doc comment on why the two constants don't
  // need to match). Fit it again into the display cell's square so a
  // non-square source image isn't stretched back out by the Image element
  // scaling an unrelated width/height onto it.
  int32_t src_w = 0, src_h = 0;
  if (auto* w = args.findField<int32_t>("width"_key))
    src_w = *w;
  if (auto* h = args.findField<int32_t>("height"_key))
    src_h = *h;
  for (auto& e : images_) {
    if (e.name == name && e.image_ptr) {
      (*e.image_ptr)["src"_key] = thumb_path;
      if (src_w > 0 && src_h > 0) {
        int32_t disp_w, disp_h;
        fit_dims(src_w, src_h, kThumbPx, kThumbPx, disp_w, disp_h);
        (*e.image_ptr)["width"_key] = disp_w;
        (*e.image_ptr)["height"_key] = disp_h;
      }
      break;
    }
  }
  return dynamic{};
}

dynamic pix_viewer::do_set_preview(const dynamic& args) {
  bool loading = false;
  if (auto* f = args.findField<bool>("loading"_key))
    loading = *f;

  if (loading) {
    if (zoom_label_ptr_)
      (*zoom_label_ptr_)["text"_key] = std::string{"Loading..."};
    return dynamic{};
  }

  if (preview_image_ptr_) {
    (*preview_image_ptr_)["src"_key] = args.as<std::string>("src"_key);
    if (auto* w = args.findField<int32_t>("width"_key)) {
      (*preview_image_ptr_)["width"_key] = *w;
      // preview_table's sole column has a fixed pixel width (ImGui table
      // columns don't auto-expand to an oversized cell's content) -- track
      // it to the image's own current (zoomed) width so ScrollX's pannable
      // range actually reaches the image's full width instead of clipping
      // it at whatever init_width happened to be set to last. A width
      // smaller than the viewport is harmless (just no horizontal scroll).
      if (preview_col_ptr_ && *w > 0)
        (*preview_col_ptr_)["init_width"_key] = static_cast<float>(*w);
    }
    if (auto* h = args.findField<int32_t>("height"_key))
      (*preview_image_ptr_)["height"_key] = *h;
  }
  if (zoom_label_ptr_) {
    float zoom = 100.0f;
    if (auto* z = args.findField<float>("zoom_percent"_key))
      zoom = *z;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << zoom << "%";
    (*zoom_label_ptr_)["text"_key] = oss.str();
  }
  return dynamic{};
}

dynamic pix_viewer::do_set_info(const dynamic& args) {
  auto set_row = [&](const ui_element_ptr& ptr, key_t field_key, const char* prefix) {
    if (!ptr)
      return;
    std::string value;
    if (auto* f = args.findField<std::string>(field_key); f && !f->empty())
      value = std::string{prefix} + *f;
    (*ptr)["text"_key] = value;
  };
  set_row(info_filename_ptr_, "filename"_key, "");
  set_row(info_resolution_ptr_, "resolution"_key, "Resolution: ");
  set_row(info_format_ptr_, "format"_key, "Format: ");
  set_row(info_size_ptr_, "size"_key, "Size: ");
  set_row(info_modified_ptr_, "modified"_key, "Modified: ");
  return dynamic{};
}

dynamic pix_viewer::do_set_status(const dynamic& args) {
  if (status_label_ptr_)
    (*status_label_ptr_)["text"_key] = args.as<std::string>("message"_key);
  return dynamic{};
}

dynamic pix_viewer::do_stat_files(const dynamic& args) {
  // Called via addMethod() -- i.e. from within RMI dispatch, where the
  // dispatch wlock is already held -- so resource_dir/allow_absolute_paths
  // are read via sess(), not context_rlock (which would self-deadlock; see
  // form::sess()'s doc comment and tree::navigate_sandbox()'s).
  const fs::path& resource_dir = sess().resource_dir;
  bool allow_absolute = sess().allow_absolute_paths;

  dynamic results;
  size_t i = 0;
  if (auto* paths = args.findField<dynamic_ptr>("paths"_key); paths && *paths) {
    (**paths).forEach([&](key_t, const field& path_field) {
      if (!path_field.is<std::string>())
        return;
      const auto& rel = path_field.as<std::string>();

      auto entry = std::make_shared<dynamic>();
      (*entry)["path"_key] = rel;

      auto resolved = file_service::resolve_path(rel, resource_dir, allow_absolute);
      std::error_code ec;
      bool exists = !resolved.empty() && fs::is_regular_file(resolved, ec);
      (*entry)["exists"_key] = exists;
      // mtime is Unix seconds truncated to int32_t -- bison::field has no
      // int64 alternative (see bison_common.hpp's field_base variant) --
      // which is fine for a same-machine freshness comparison (client vs.
      // sandbox mtime) until year 2038.
      (*entry)["mtime"_key] =
          exists ? static_cast<int32_t>(file_time_to_unix_seconds(fs::last_write_time(resolved, ec))) : int32_t{0};
      results[i++] = dynamic_ptr{entry};
    });
  }
  return results;
}

dynamic pix_viewer::do_delete_file(const dynamic& args) {
  // Sandbox-local action (like "Open Sandbox in Explorer") -- the client's
  // full-image LRU cache (see client/pix.cpp's touch_full_cache()) evicts
  // its least-recently-used entry by asking the server to delete the file
  // directly, rather than needing its own sandbox filesystem access.
  // Called via addMethod() -- see do_stat_files()'s identical doc comment
  // on why resource_dir/allow_absolute_paths come from sess(), not
  // context_rlock.
  auto rel = args.as<std::string>("path"_key);
  auto resolved = file_service::resolve_path(rel, sess().resource_dir, sess().allow_absolute_paths);
  if (!resolved.empty()) {
    std::error_code ec;
    fs::remove(resolved, ec); // Best-effort: a missing file is not an error.
  }
  return dynamic{};
}

dynamic pix_viewer::on_set(const dynamic& patch) {
  if (auto* p = patch.findField<std::string>("path"_key); p && path_input_ptr_)
    (*path_input_ptr_)["value"_key] = *p;
  return patch;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void pix_viewer::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == path_input_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<std::string>("value"_key)) {
      (*this)["path"_key] = *v;
      dynamic args;
      args["path"_key] = *v;
      emit("on_path_submitted"_key, std::move(args));
    }
    return;
  }

  if (id == btn_browse_id_ && event == "clicked"_key) {
    emit("on_browse_clicked"_key);
    return;
  }

  if (id == btn_open_explorer_id_ && event == "clicked"_key) {
    fs::path resource_dir;
    {
      auto s = context_rlock{*sync_ctx_};
      resource_dir = s->resource_dir;
    }
    dynamic status_args;
    status_args["message"_key] = open_in_host_explorer(resource_dir)
        ? std::string{"Opened sandbox in host file explorer."}
        : std::string{"Could not open host file explorer."};
    do_set_status(status_args);
    return;
  }

  if (id == btn_zoom_out_id_ && event == "clicked"_key) {
    dynamic a;
    a["action"_key] = std::string{"zoom_out"};
    emit("on_view_control"_key, std::move(a));
    return;
  }
  if (id == btn_zoom_in_id_ && event == "clicked"_key) {
    dynamic a;
    a["action"_key] = std::string{"zoom_in"};
    emit("on_view_control"_key, std::move(a));
    return;
  }
  if (id == btn_zoom_fit_id_ && event == "clicked"_key) {
    dynamic a;
    a["action"_key] = std::string{"zoom_fit"};
    emit("on_view_control"_key, std::move(a));
    return;
  }
  if (id == btn_zoom_100_id_ && event == "clicked"_key) {
    dynamic a;
    a["action"_key] = std::string{"zoom_100"};
    emit("on_view_control"_key, std::move(a));
    return;
  }

  if (auto it = selectable_id_to_index_.find(id.id); it != selectable_id_to_index_.end() && event == "changed"_key) {
    select_index(it->second);
    return;
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_pix() {
  auto proto = dynamic_ptr{"PixViewer"_key, {}};

  proto->addField(
      "path"_key,
      field{
          std::string{},
          attr<DisplayName>("Path"),
          attr<Description>("Local directory currently being browsed (client-owned; the form only "
                            "displays it)."),
          attr<Category>("State")});

  proto->addMethod("set_images"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_set_images(args);
                   }});
  proto->addMethod("set_thumbnail"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_set_thumbnail(args);
                   }});
  proto->addMethod("set_preview"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_set_preview(args);
                   }});
  proto->addMethod("set_info"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_set_info(args);
                   }});
  proto->addMethod("set_status"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_set_status(args);
                   }});
  proto->addMethod("stat_files"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_stat_files(args);
                   }});
  proto->addMethod("delete_file"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<pix_viewer&>(self).do_delete_file(args);
                   }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& self, const dynamic& patch) -> dynamic {
                     return static_cast<pix_viewer&>(self).on_set(patch);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Pix"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Local image folder browser: thumbnail grid + zoomable/pannable full "
                        "preview. Thumbnails and full images are generated/uploaded client-side; "
                        "this form only owns UI structure and sandbox-local actions (Open in "
                        "Explorer)."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<pix_viewer>("wish"_key, "PixViewer"_key));
}

} // namespace bdg::wish
