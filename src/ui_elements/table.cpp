// MIT License © 2025 Binary Dice Games
/// @file table.cpp
/// @brief Registers Table, TableColumn, and TableRow prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_table() {
  // Table — maps to ImGui::BeginTable / EndTable.
  // Column setup is expressed as TableColumn children that precede any rows;
  // the renderer iterates them first to call TableSetupColumn, then calls
  // TableHeadersRow if headers == true, then renders the remaining children.
  {
    auto proto = dynamic_ptr{"Table"_key, {}};
    proto->addField(
        "id"_key,
        field{
            std::string{"##table"},
            attr<DisplayName>("ID"),
            attr<Description>("ImGui string identifier for this table."),
            attr<Category>("Behavior")});
    proto->addField(
        "columns"_key,
        field{
            int32_t{1},
            attr<DisplayName>("Columns"),
            attr<Description>("Number of columns. Must be >= 1."),
            attr<Category>("Layout"),
            attr<Range>(1, 64),
            attr<Step>(1)});
    proto->addField(
        "flags"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiTableFlags bitmask (e.g. Borders=1920, RowBg=64, Resizable=1)."),
            attr<Category>("Behavior")});
    proto->addField(
        "outer_width"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Outer Width"),
            attr<Description>("Outer container width; 0 fills available width."),
            attr<Category>("Layout")});
    proto->addField(
        "outer_height"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Outer Height"),
            attr<Description>("Outer container height; 0 means no clipping."),
            attr<Category>("Layout")});
    proto->addField(
        "inner_width"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Inner Width"),
            attr<Description>("Width allocated to contents; 0 uses outer width."),
            attr<Category>("Layout")});
    proto->addField(
        "headers"_key,
        field{
            bool{false},
            attr<DisplayName>("Show Headers"),
            attr<Description>("Render a header row from TableColumn labels."),
            attr<Category>("Appearance")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Table"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A multi-column table. Direct children of type TableColumn define "
                          "column setup; all other children (typically TableRow) provide rows. "
                          "Emits 'row_selected' / 'row_activated' with {index: int} on row clicks. "
                          "When flags includes ImGuiTableFlags_Sortable (8) and the user clicks a "
                          "column header, emits 'sorted' with {column_id: int, ascending: bool} -- "
                          "the owner is responsible for actually reordering rows in response."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Table"_key));
  }

  // TableColumn — maps to ImGui::TableSetupColumn.
  // Must be a direct child of Table and precede any TableRow children.
  {
    auto proto = dynamic_ptr{"TableColumn"_key, {}};
    proto->addField(
        "label"_key,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Column header text, shown when Table.headers is true."),
            attr<Category>("Content")});
    proto->addField(
        "flags"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiTableColumnFlags bitmask."),
            attr<Category>("Behavior")});
    proto->addField(
        "init_width"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Init Width"),
            attr<Description>("Initial column width in pixels (or weight for stretch columns)."),
            attr<Category>("Layout")});
    proto->addField(
        "column_id"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Column ID"),
            attr<Description>("Stable identifier passed through to ImGui as the column's user_data. "
                              "When the parent Table has ImGuiTableFlags_Sortable and the user clicks "
                              "this column's header, the Table's 'sorted' event payload's `column_id` "
                              "field echoes this value back, letting the owner map it to a semantic "
                              "field (e.g. \"sort by PID\") independent of column position."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("TableColumn"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Defines one column inside a Table. "
                          "Processed by the parent Table for setup; not rendered independently."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "TableColumn"_key));
  }

  // TableRow — maps to ImGui::TableNextRow.
  // Each direct child of a TableRow is rendered into the next table column.
  {
    auto proto = dynamic_ptr{"TableRow"_key, {}};
    proto->addField(
        "flags"_key,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiTableRowFlags bitmask (e.g. Headers=1)."),
            attr<Category>("Behavior")});
    proto->addField(
        "min_height"_key,
        field{
            float{0.0f},
            attr<DisplayName>("Min Height"),
            attr<Description>("Minimum row height in pixels; 0 uses default."),
            attr<Category>("Layout")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("TableRow"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A row inside a Table. Each child element occupies one column cell "
                          "in left-to-right order."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "TableRow"_key));
  }
}

} // namespace bdg::wish
