// MIT License © 2026 Binary Dice Games
/// @file du.hpp
/// @brief Server-side form for du (UI only, no local file access).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Disk usage analyzer: shows where the space of a folder on the
/// *client's* machine went, as a size-sorted table and a treemap.
///
/// Two dockable panels side by side inside the tool's own nested dockspace
/// (see docs/dock-layout.md), like zip, top and pix: **Files** (a
/// fixed-width panel on the left, and the form's main
/// root: Rescan/Up buttons, the path bar, a summary line, the table of the
/// current folder's contents with each entry's share of it, and the status
/// line) and **Treemap** (filling the rest: the current folder's whole subtree as nested
/// rectangles sized by bytes, files colored by type).
///
/// Like zip, this form has **no filesystem access at all**: the client
/// scans its own disk and keeps the resulting tree; the form only ever
/// holds the one folder being displayed. The handshake:
///
///   - `"on_scan_requested"` (`{path}`) -- Enter in the path bar, or Rescan.
///     The client scans `path` (reporting progress via
///     `set({"status": ..., "scanning": true})`) and then calls
///     `show_directory()` for the scanned root.
///   - `"on_cancel_requested"` -- the Rescan button, which reads "Stop"
///     while `scanning` is true. The client abandons the scan.
///   - `"on_navigate"` (`{path}`, `/`-separated and relative to the scanned
///     root, `""` for the root itself) -- a folder was opened (double click
///     on its table row or on its treemap rectangle) or Up was pressed. The
///     client calls `show_directory()` for that folder.
///   - `"closed"` -- either panel's X button; both panels are removed.
///
/// Selecting a table row outlines its rectangle in the treemap; clicking a
/// rectangle selects the row of the top-level entry it belongs to and shows
/// the clicked item's full path and size in the status line.
class du : public form {
 public:
  explicit du(bison::dynamic&& base);
  /// @brief Removes the Treemap panel; ~form() removes the Files panel and
  /// the dock layout.
  ~du() override;

  /// @brief RMI method: display one folder of the scanned tree.
  ///
  /// @p args holds:
  ///   - `root` (string): the scanned folder's absolute path;
  ///   - `path` (string): the displayed folder, relative to `root` (`""`);
  ///   - `size` (string, decimal bytes -- bison has no 64-bit integer
  ///     field), `files`, `dirs` (int32): the displayed folder's totals;
  ///   - `entries`: dynamic array of `{name, type ("file"/"dir"/"rest"),
  ///     size (decimal bytes string), items (int32)}`, the folder's direct
  ///     children (`"rest"` is one row standing for the entries left out of
  ///     a very long listing);
  ///   - `parents`, `sizes`, `colors` (arrays) and `labels`, `details`
  ///     (newline-separated strings): the `Treemap` element's fields for
  ///     the folder's subtree, forwarded as they are; node 0 is the folder
  ///     and its direct children are the nodes whose parent is 0;
  ///   - `kinds` (int32 array, parallel): 0 file, 1 folder, 2 merged small
  ///     items.
  bison::dynamic do_show_directory(const bison::dynamic& args);

  /// @brief Called from the `__setter` prototype method for every set() call.
  /// Mirrors `status` into the status line and `scanning` into the
  /// Rescan/Stop button.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  /// @brief Reacts to a panel's `"closed"`, the path bar's `"changed"`, the
  /// buttons' `"clicked"`, the table's row/sort events and the treemap's
  /// `"clicked"`/`"activated"`.
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct row {
    std::string name;
    std::string type; ///< "file", "dir" or "rest"
    std::uint64_t size{0};
    std::int32_t items{0}; ///< Files and folders below a directory.
  };

  /// @brief Import @p layout_json, assign every element an RMI id, run
  /// @p wire to capture element pointers, and merge the tree under
  /// @p root_key (registering it as its own top-level object unless it is
  /// the main `internal_root_key_`, which form::init() registers itself).
  void build_window(
      const char* layout_json, const std::string& root_key, bison::key_t& window_id_out,
      const std::function<void(ui_tree&)>& wire);
  /// @brief Remove the Treemap panel and forget its key. Safe to call twice.
  void remove_panel_objects();

  /// @brief Rebuild the table from `rows_`, preceded by a ".." row when the
  /// displayed folder is not the scanned root.
  void fill_table();
  /// @brief Sort `rows_` by the table column (0 Name, 1 %, 2 Size, 3
  /// Items), keeping a `"rest"` row last.
  void sort_rows();
  void set_status(const std::string& message);
  /// @brief Select the entry named @p name (empty: nothing) in the table
  /// and outline its rectangle in the treemap.
  void select(const std::string& name);
  /// @brief Emit `on_navigate` for @p rel_path.
  void navigate(const std::string& rel_path);
  /// @brief Emit `on_navigate` for the displayed folder's child @p name, or
  /// its parent for `".."`.
  void open_entry(const std::string& name);
  /// @brief The displayed folder's absolute path, `/`-joined.
  std::string display_path() const;
  /// @brief True when the displayed folder is below the scanned root.
  bool has_parent() const { return !rel_path_.empty(); }
  /// @brief The treemap node that is a direct child of the displayed folder
  /// and contains node @p index (itself, if it is one); -1 if @p index is
  /// the root or out of range.
  std::int32_t top_level_node(std::int32_t index) const;

  std::string treemap_root_key_; ///< internal_root_key_ + "_treemap".

  bison::key_t window_id_; ///< Files panel (main root).
  bison::key_t treemap_window_id_;
  bison::key_t path_input_id_;
  bison::key_t btn_scan_id_;
  bison::key_t btn_up_id_;
  bison::key_t table_id_;
  bison::key_t treemap_id_;

  ui_element_ptr path_input_ptr_;
  ui_element_ptr btn_scan_ptr_;
  ui_element_ptr summary_label_ptr_;
  ui_element_ptr table_ptr_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr treemap_ptr_;

  std::string root_;     ///< Scanned folder, as reported by show_directory().
  std::string rel_path_; ///< Displayed folder relative to root_.
  std::uint64_t dir_size_{0};
  std::vector<row> rows_;
  std::string selected_name_;
  std::int32_t sort_column_id_{2};
  bool sort_ascending_{false};
  bool scanning_{false};

  // The treemap's nodes, kept to resolve a clicked index.
  std::vector<std::int32_t> node_parents_;
  std::vector<std::int32_t> node_kinds_;
  std::vector<std::string> node_labels_;
  std::vector<std::string> node_details_;
};

/// @brief Register Du in the "wish" bison namespace.
void register_du();

} // namespace bdg::wish
