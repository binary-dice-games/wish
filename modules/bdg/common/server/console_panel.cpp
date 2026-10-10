// MIT License © 2026 Binary Dice Games
/// @file console_panel.cpp
/// @brief Implementation of common::console_panel.
#include "modules/bdg/common/server/console_panel.hpp"

#include "modules/bdg/common/server/tool_form.hpp"
#include "modules/bdg/common/server/ui_helpers.hpp"

namespace bdg::wish::common {

using namespace bison;

namespace {

// "auto_scroll": true so the table follows the newest row. Title, width,
// table id and the Command column's width are set per tool in build().
constexpr const char* kConsoleLayout = R"json({
  "type": "Window", "title": "Console", "width": 940, "height": 240,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "table": {
      "type": "Table", "id": "##console_table", "columns": 4,
      "flags": "Resizable|RowBg|Borders|ScrollX|ScrollY", "resize_pushes": true, "cell_tooltips": true, "headers": true,
      "height": -1, "outer_height": -1, "auto_scroll": true,
      "children": {
        "col_seq":     { "type": "TableColumn", "label": "#",       "flags": "WidthFixed", "init_width": 44,  "column_id": 0 },
        "col_command": { "type": "TableColumn", "label": "Command", "flags": "WidthFixed", "init_width": 360, "column_id": 1 },
        "col_exit":    { "type": "TableColumn", "label": "Exit",    "flags": "WidthFixed", "init_width": 50,  "column_id": 2 },
        "col_output":  { "type": "TableColumn", "label": "Output",  "flags": "WidthStretch",                     "column_id": 3 }
      }
    }
  } } }
})json";

} // namespace

void console_panel::build(tool_form& owner, const std::string& root_key, console_options options) {
  owner_ = &owner;
  options_ = std::move(options);
  root_key_ = root_key;
  owner.build_window(root_key_, kConsoleLayout, window_id_, [&](ui_tree& tree) {
    tree.with("", [&](const auto& e) {
      e["title"_key] = options_.title;
      e["width"_key] = options_.width;
      e["closable"_key] = options_.closable;
    });
    tree.with("vbox.table", [&](const auto& e) {
      e["id"_key] = options_.table_id;
      table_ = e;
    });
    tree.with("vbox.table.col_command", [&](const auto& e) {
      e["init_width"_key] = static_cast<float>(options_.command_width);
    });
  });
}

void console_panel::append(const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
  if (!owner_ || !table_)
    return;
  auto* children_p = table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  auto& owner = *owner_;

  const theme_color& color = ok ? kOk : kBad;

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  owner.assign_id(row);

  ui_element_ptr cell_seq = owner.make_label(std::to_string(++seq_), kIdle);
  ui_element_ptr cell_command = owner.make_label(command, color);
  ui_element_ptr cell_exit = owner.make_label(std::to_string(exit_code), color);
  ui_element_ptr cell_output = owner.make_label(output, color);

  // Right-click any row for "Copy Entry" (this row's command/exit/output,
  // via MenuItem.copy_text) and "Clear <title>" (every row).
  ui_element_ptr context_menu = ui_element_ptr::create("wish"_key, "ContextMenu"_key);
  owner.assign_id(context_menu);

  ui_element_ptr copy_item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  copy_item["label"_key] = std::string{"Copy Entry"};
  set_icon(copy_item, "copy");
  copy_item["copy_text"_key] = command + "\nexit: " + std::to_string(exit_code) + "\n" + output;
  owner.assign_id(copy_item);
  if (options_.on_copied)
    owner.on_click(wish_id_of(copy_item), options_.on_copied);

  ui_element_ptr clear_item = ui_element_ptr::create("wish"_key, "MenuItem"_key);
  clear_item["label"_key] = "Clear " + options_.title;
  set_icon(clear_item, "delete");
  owner.assign_id(clear_item);
  owner.on_click(wish_id_of(clear_item), [this] {
    clear();
    if (options_.on_cleared)
      options_.on_cleared();
  });

  set_children_list(context_menu, {copy_item, clear_item});
  set_children_list(row, {cell_seq, cell_command, cell_exit, cell_output, context_menu});

  row_entry entry;
  entry.child_key = next_child_key_++;
  entry.object_ids = {
      wish_id_of(row),
      wish_id_of(cell_seq),
      wish_id_of(cell_command),
      wish_id_of(cell_exit),
      wish_id_of(cell_output),
      wish_id_of(context_menu),
      wish_id_of(copy_item),
      wish_id_of(clear_item)};
  (*children)[entry.child_key] = dynamic_ptr{row};
  rows_.push_back(std::move(entry));

  if (rows_.size() > kMaxRows) {
    owner.erase_objects(rows_.front().object_ids);
    children->erase(rows_.front().child_key);
    rows_.pop_front();
  }
  table_->refresh_children_order();
}

void console_panel::append_from(const dynamic& args) {
  const auto* exit_f = args.findField<int32_t>("exit_code"_key);
  append(str_of(args, "command"_key), exit_f ? *exit_f : 0, flag_of(args, "ok"_key), str_of(args, "output"_key));
}

void console_panel::clear() {
  if (!owner_ || !table_)
    return;
  auto* children_p = table_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  for (auto& entry : rows_) {
    owner_->erase_objects(entry.object_ids);
    children->erase(entry.child_key);
  }
  rows_.clear();
  seq_ = 0;
  next_child_key_ = 0; // every numeric child key was just erased.
  table_->refresh_children_order();
}

} // namespace bdg::wish::common
