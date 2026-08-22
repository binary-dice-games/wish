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
    auto proto = dynamic_ptr{"Table"_rkey, {}};
    proto->addField(
        "id"_rkey,
        field{
            std::string{"##table"},
            attr<DisplayName>("ID"),
            attr<Description>("ImGui string identifier for this table."),
            attr<Category>("Behavior")});
    proto->addField(
        "columns"_rkey,
        field{
            int32_t{1},
            attr<DisplayName>("Columns"),
            attr<Description>("Number of columns. Must be >= 1."),
            attr<Category>("Layout"),
            attr<Range>(1, 64),
            attr<Step>(1)});
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiTableFlags bitmask (combine names with '|')."),
            attr<Category>("Behavior"),
            attr<EnumFlags>(EnumFlags::table{
                {"Resizable", 1 << 0},
                {"Reorderable", 1 << 1},
                {"Hideable", 1 << 2},
                {"Sortable", 1 << 3},
                {"NoSavedSettings", 1 << 4},
                {"ContextMenuInBody", 1 << 5},
                {"RowBg", 1 << 6},
                {"BordersInnerH", 1 << 7},
                {"BordersOuterH", 1 << 8},
                {"BordersInnerV", 1 << 9},
                {"BordersOuterV", 1 << 10},
                {"NoBordersInBody", 1 << 11},
                {"NoBordersInBodyUntilResize", 1 << 12},
                {"SizingFixedFit", 1 << 13},
                {"SizingFixedSame", 2 << 13},
                {"SizingStretchProp", 3 << 13},
                {"SizingStretchSame", 4 << 13},
                {"NoHostExtendX", 1 << 16},
                {"NoHostExtendY", 1 << 17},
                {"NoKeepColumnsVisible", 1 << 18},
                {"PreciseWidths", 1 << 19},
                {"NoClip", 1 << 20},
                {"PadOuterX", 1 << 21},
                {"NoPadOuterX", 1 << 22},
                {"NoPadInnerX", 1 << 23},
                {"ScrollX", 1 << 24},
                {"ScrollY", 1 << 25},
                {"SortMulti", 1 << 26},
                {"SortTristate", 1 << 27},
                {"HighlightHoveredColumn", 1 << 28},
                // Convenience composites — listed after single-bit flags so format()
                // prefers the fine-grained names when decomposing a value.
                {"BordersH", (1 << 7) | (1 << 8)},
                {"BordersV", (1 << 9) | (1 << 10)},
                {"BordersInner", (1 << 7) | (1 << 9)},
                {"BordersOuter", (1 << 8) | (1 << 10)},
                {"Borders", (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10)},
            })});
    proto->addField(
        "outer_width"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Outer Width"),
            attr<Description>("Outer container width; 0 fills available width."),
            attr<Category>("Layout")});
    proto->addField(
        "outer_height"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Outer Height"),
            attr<Description>("Outer container height; 0 means no clipping."),
            attr<Category>("Layout")});
    proto->addField(
        "inner_width"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Inner Width"),
            attr<Description>("Width allocated to contents; 0 uses outer width."),
            attr<Category>("Layout")});
    proto->addField(
        "headers"_rkey,
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
    auto proto = dynamic_ptr{"TableColumn"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Column header text, shown when Table.headers is true."),
            attr<Category>("Content")});
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiTableColumnFlags bitmask (combine names with '|')."),
            attr<Category>("Behavior"),
            attr<EnumFlags>(EnumFlags::table{
                {"Disabled", 1 << 0},
                {"DefaultHide", 1 << 1},
                {"DefaultSort", 1 << 2},
                {"WidthStretch", 1 << 3},
                {"WidthFixed", 1 << 4},
                {"NoResize", 1 << 5},
                {"NoReorder", 1 << 6},
                {"NoHide", 1 << 7},
                {"NoClip", 1 << 8},
                {"NoSort", 1 << 9},
                {"NoSortAscending", 1 << 10},
                {"NoSortDescending", 1 << 11},
                {"NoHeaderLabel", 1 << 12},
                {"NoHeaderWidth", 1 << 13},
                {"PreferSortAscending", 1 << 14},
                {"PreferSortDescending", 1 << 15},
                {"IndentEnable", 1 << 16},
                {"IndentDisable", 1 << 17},
                {"AngledHeader", 1 << 18},
            })});
    proto->addField(
        "init_width"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Init Width"),
            attr<Description>("Initial column width in pixels (or weight for stretch columns)."),
            attr<Category>("Layout")});
    proto->addField(
        "column_id"_rkey,
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
    auto proto = dynamic_ptr{"TableRow"_rkey, {}};
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiTableRowFlags bitmask (combine names with '|')."),
            attr<Category>("Behavior"),
            attr<EnumFlags>(EnumFlags::table{
                {"Headers", 1 << 0},
            })});
    proto->addField(
        "min_height"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Min Height"),
            attr<Description>("Minimum row height in pixels; 0 uses default."),
            attr<Category>("Layout")});
    proto->addField(
        "selected"_rkey,
        field{
            false,
            attr<DisplayName>("Selected"),
            attr<Description>("Whether this row is rendered highlighted, e.g. to show the "
                              "current selection in a file list."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("TableRow"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A row inside a Table. Each child element occupies one column cell "
                          "in left-to-right order, except a ContextMenu child (if present), which "
                          "is excluded from column layout and instead opens as a right-click menu "
                          "for the whole row."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "TableRow"_key));
  }
}

} // namespace bdg::wish
