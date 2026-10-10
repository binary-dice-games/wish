// MIT License © 2026 Binary Dice Games
/// @file list_panel.hpp
/// @brief Table rows with a per-row `...` menu, and the toolbar + status +
///        table list window the bdg/dev tool forms are built from.
///
/// table_rows<Meta> owns the C++-built rows of one `Table` element: it adds
/// a row from its cells (plus an optional `...` MenuButton whose items run
/// callbacks through the owner's click handlers), clears them -- with every
/// object they registered -- and applies a visibility filter over each
/// row's @p Meta (whatever the module filters or acts on: a name, a
/// namespace, a state).
///
/// list_panel<Meta> is one dockable list window: a layout whose
/// `vbox.status` Label and `vbox.table` Table it caches, plus a
/// table_rows<Meta> for that table. Header-only (templates).
#pragma once

#include "modules/bdg/common/server/tool_form.hpp"
#include "modules/bdg/common/server/ui_helpers.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_object.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bdg::wish::common {

/// @brief One item of a row's `...` menu. An empty @p label inserts a
/// Separator.
struct menu_item {
  std::string label;
  std::function<void()> on_click;
  /// The action asks for confirmation: the label gets a trailing "...".
  bool confirm{false};
};

template <typename Meta = std::monostate>
class table_rows {
 public:
  struct row {
    Meta meta;
    ui_element_ptr el;
    size_t child_key{0};
    std::vector<bison::key_t> object_ids; // erased together on clear()
  };

  /// @brief Manages the rows of @p table, whose objects are registered with
  /// @p owner. Rows declared in the table's layout (its TableColumns) are
  /// left alone.
  void attach(tool_form& owner, ui_element_ptr table) {
    owner_ = &owner;
    table_ = std::move(table);
  }

  /// @brief Removes every row added by add() and unregisters its objects
  /// and menu handlers.
  void clear() {
    if (auto* children = children_of_table()) {
      for (auto& r : rows_) {
        (*children)->erase(r.child_key);
        owner_->erase_objects(r.object_ids);
      }
    }
    rows_.clear();
    next_child_key_ = 0;
  }

  /// @brief Appends a row of @p cells -- elements already registered with
  /// the owner (tool_form::make_label() does that) -- plus, when @p menu is
  /// not empty, a `...` MenuButton cell. Call refresh() after the last row.
  /// @return The new row (valid until the next add() or clear()), or null
  ///         when the table is gone.
  row* add(Meta meta, const std::vector<ui_element_ptr>& cells, const std::vector<menu_item>& menu = {}) {
    auto* children = children_of_table();
    if (!children)
      return nullptr;
    auto& owner = *owner_;

    std::vector<bison::key_t> ids;
    for (auto& cell : cells)
      ids.push_back(wish_id_of(cell));

    ui_element_ptr el = ui_element_ptr::create("wish"_key, "TableRow"_key);
    owner.assign_id(el);
    ids.push_back(wish_id_of(el));

    std::vector<ui_element_ptr> row_cells = cells;
    if (!menu.empty()) {
      ui_element_ptr button = ui_element_ptr::create("wish"_key, "MenuButton"_key);
      button["label"_key] = std::string{"..."};
      owner.assign_id(button);
      ids.push_back(wish_id_of(button));

      std::vector<ui_element_ptr> items;
      for (auto& it : menu) {
        ui_element_ptr item;
        if (it.label.empty()) {
          item = ui_element_ptr::create("wish"_key, "Separator"_key);
          owner.assign_id(item);
        } else {
          item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
          item["label"_key] = it.confirm ? it.label + "..." : it.label;
          owner.assign_id(item);
          if (it.on_click)
            owner.on_click(wish_id_of(item), it.on_click);
        }
        ids.push_back(wish_id_of(item));
        items.push_back(item);
      }
      set_children_list(button, items);
      row_cells.push_back(button);
    }
    set_children_list(el, row_cells);

    row r;
    r.meta = std::move(meta);
    r.el = el;
    r.child_key = next_child_key_++;
    r.object_ids = std::move(ids);
    (**children)[r.child_key] = bison::dynamic_ptr{el};
    rows_.push_back(std::move(r));
    return &rows_.back();
  }

  /// @brief Re-sorts the table's children after rows were added or removed.
  void refresh() {
    if (table_)
      table_->refresh_children_order();
  }

  /// @brief Shows exactly the rows whose meta satisfies @p pred.
  template <typename Pred>
  void apply_filter(Pred&& pred) {
    for (auto& r : rows_)
      if (r.el)
        r.el["visible"_key] = static_cast<bool>(pred(r.meta));
  }

  std::vector<row>& rows() {
    return rows_;
  }
  const std::vector<row>& rows() const {
    return rows_;
  }
  const ui_element_ptr& table() const {
    return table_;
  }

 private:
  bison::dynamic_ptr* children_of_table() {
    if (!owner_ || !table_)
      return nullptr;
    auto* children_p = table_->findField<bison::dynamic_ptr>("children"_key);
    return children_p && *children_p ? children_p : nullptr;
  }

  tool_form* owner_{nullptr};
  ui_element_ptr table_;
  std::vector<row> rows_;
  size_t next_child_key_{0};
};

template <typename Meta = std::monostate>
class list_panel {
 public:
  using row = typename table_rows<Meta>::row;

  /// @brief Builds @p layout_json as @p owner's root @p root_key (see
  /// tool_form::build_window()), caching its `vbox.status` Label and
  /// `vbox.table` Table. @p wire binds the window's own toolbar.
  void build(
      tool_form& owner,
      const std::string& root_key,
      const char* layout_json,
      const std::function<void(ui_tree&)>& wire = {}) {
    root_key_ = root_key;
    ui_element_ptr table;
    owner.build_window(root_key_, layout_json, window_id_, [&](ui_tree& tree) {
      tree.with("vbox.status", [&](const auto& e) { status_label_ = e; });
      tree.with("vbox.table", [&](const auto& e) { table = e; });
      if (wire)
        wire(tree);
    });
    rows_.attach(owner, table);
  }

  /// @see table_rows::clear()
  void clear() {
    rows_.clear();
  }
  /// @see table_rows::add()
  row* add(Meta meta, const std::vector<ui_element_ptr>& cells, const std::vector<menu_item>& menu = {}) {
    return rows_.add(std::move(meta), cells, menu);
  }
  /// @see table_rows::refresh()
  void refresh() {
    rows_.refresh();
  }
  /// @see table_rows::apply_filter()
  template <typename Pred>
  void apply_filter(Pred&& pred) {
    rows_.apply_filter(std::forward<Pred>(pred));
  }

  /// @brief Writes the window's status label (idle grey, or red when not
  /// @p ok). @p first_line_only cuts @p text at its first line break.
  void set_status(const std::string& text, bool ok, bool first_line_only = false) {
    set_status_text(status_label_, first_line_only ? text.substr(0, text.find('\n')) : text, ok);
  }

  std::vector<row>& rows() {
    return rows_.rows();
  }
  const std::vector<row>& rows() const {
    return rows_.rows();
  }
  const ui_element_ptr& table() const {
    return rows_.table();
  }
  const ui_element_ptr& status_label() const {
    return status_label_;
  }
  bison::key_t window_id() const {
    return window_id_;
  }
  const std::string& root_key() const {
    return root_key_;
  }

 private:
  std::string root_key_;
  bison::key_t window_id_;
  ui_element_ptr status_label_;
  table_rows<Meta> rows_;
};

} // namespace bdg::wish::common
