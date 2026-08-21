// MIT License © 2025 Binary Dice Games
/// @file pix.hpp
/// @brief Server-side PixViewer form: a two-panel local image browser.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Image viewer form: a folder-of-images browser with a thumbnail
/// grid on the left and a zoomable/pannable full preview + metadata panel
/// on the right.
///
/// PixViewer has no direct access to the client's local machine, so (like
/// mc's left panel and nano's open/save flow) it only owns the
/// UI structure and forwards user intent as high-level events; the client
/// runner (`run_pix`) does all local-filesystem enumeration, image
/// decoding, thumbnail/preview generation, and sandbox uploads (via
/// `client::upload_file`), then pushes results back through this form's
/// RMI methods (`set_images`, `set_thumbnail`, `set_preview`, `set_info`).
///
/// The "Open Sandbox in Explorer" button is the one piece of this form
/// that's entirely server-owned (mirrors mc's own button): the
/// sandbox directory lives on this machine, so there's nothing for the
/// client to do.
///
/// Thumbnails are laid out as a `Table` (fixed column count, `outer_height`
/// clipped/scrollable) so a folder with many images scrolls independently
/// of the rest of the window, without requiring any new renderer
/// primitive: each cell is a small `VerticalLayout` holding an `Image`
/// (thumbnail, not itself clickable -- Image has no events) and a
/// `Selectable` (the filename, click target) with its own `__wish_id` for
/// precise per-cell click routing, entirely independent of the Table's own
/// `row_selected`/`row_activated` events (which this form does not use).
///
/// The right preview panel reuses the exact same "Table as a scrollable
/// clipped viewport" trick for panning: a single-cell `Table` with
/// `ScrollX|ScrollY` hosts the (possibly zoomed-beyond-the-viewport)
/// preview `Image`, so the Table's own native scrollbars/mouse-wheel are
/// the pan control -- zoom (`+`/`-`/Fit/100%) only ever changes the
/// Image's `width`/`height` fields, never re-uploads anything.
class pix_viewer : public form {
 public:
  explicit pix_viewer(bison::dynamic&& base);

  /// @brief RMI method: replace the thumbnail grid. @p args holds `images`
  /// (array of `{name}`). Every cell starts out showing the generic
  /// placeholder thumbnail (`res/icons/image.png`) until `set_thumbnail` is
  /// called for that name. Clears the current selection, preview, and info
  /// panel.
  bison::dynamic do_set_images(const bison::dynamic& args);

  /// @brief RMI method: update one grid cell's thumbnail once it has been
  /// generated and uploaded. @p args holds `name` and `thumb_path`
  /// (sandbox-relative). A no-op if `name` is no longer in the grid (the
  /// directory changed while the thumbnail was being generated).
  bison::dynamic do_set_thumbnail(const bison::dynamic& args);

  /// @brief RMI method: update the right-panel preview. @p args holds
  /// `loading` (bool), and when `loading` is false: `src` (sandbox-relative
  /// path), `width`/`height` (int32, the zoomed display size), and
  /// `zoom_percent` (float, shown in the zoom label).
  bison::dynamic do_set_preview(const bison::dynamic& args);

  /// @brief RMI method: update the info panel. @p args holds `filename`,
  /// `resolution`, `format`, `size`, `modified` (all strings; an absent or
  /// empty value clears that row).
  bison::dynamic do_set_info(const bison::dynamic& args);

  /// @brief RMI method: update the status label text. @p args holds `message`.
  bison::dynamic do_set_status(const bison::dynamic& args);

  /// @brief RMI method: batch-stat sandbox-relative paths, so the client
  /// can compare a local file's mtime against an already-uploaded
  /// thumbnail's/full image's mtime before deciding whether to regenerate
  /// or re-upload. @p args holds `paths` (array of strings). Returns an
  /// array of `{path, exists (bool), mtime (int32, Unix seconds truncated
  /// -- bison::field has no int64 alternative; 0 if !exists)}`, one entry
  /// per input path, in the same order -- paths that escape the sandbox
  /// are reported as
  /// `exists: false` rather than erroring.
  bison::dynamic do_stat_files(const bison::dynamic& args);

  /// @brief Called from the `__setter` prototype method for every set() call.
  /// Intercepts `path` to mirror it into the path_input widget.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct grid_entry {
    std::string name;
    ui_element_ptr cell_ptr;       ///< The per-image VerticalLayout cell.
    ui_element_ptr image_ptr;      ///< Thumbnail Image inside the cell.
    ui_element_ptr selectable_ptr; ///< Filename Selectable inside the cell.
    bison::key_t selectable_id;
  };

  /// @brief Rebuild grid_table_ptr_'s TableRow children from images_,
  /// kGridColumns per row (padding the last row with empty cells).
  void rebuild_grid();

  /// @brief Select image at @p index (or clear selection if npos), updating
  /// every Selectable's highlighted state and emitting on_image_selected.
  void select_index(size_t index);

  static constexpr size_t kNoSelection = static_cast<size_t>(-1);

  bison::key_t window_id_;
  bison::key_t path_input_id_;
  bison::key_t btn_browse_id_;
  bison::key_t btn_open_explorer_id_;
  bison::key_t btn_zoom_out_id_;
  bison::key_t btn_zoom_in_id_;
  bison::key_t btn_zoom_fit_id_;
  bison::key_t btn_zoom_100_id_;

  ui_element_ptr path_input_ptr_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr grid_table_ptr_;
  ui_element_ptr preview_image_ptr_;
  ui_element_ptr zoom_label_ptr_;
  ui_element_ptr info_filename_ptr_;
  ui_element_ptr info_resolution_ptr_;
  ui_element_ptr info_format_ptr_;
  ui_element_ptr info_size_ptr_;
  ui_element_ptr info_modified_ptr_;

  std::vector<grid_entry> images_;
  std::unordered_map<uint32_t, size_t> selectable_id_to_index_;
  size_t selected_index_{kNoSelection};
  size_t next_child_key_{0};
};

/// @brief Register PixViewer in the "wish" bison namespace.
void register_pix();

} // namespace bdg::wish
