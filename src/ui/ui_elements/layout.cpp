// MIT License © 2025 Binary Dice Games
/// @file layout.cpp
/// @brief Registers Layout, VerticalLayout, and HorizontalLayout prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_layout() {
  // Layout — intermediate base that adds a spacing field.
  {
    auto proto = dynamic_ptr{"Layout"_key, {}};
    proto->addField(
        "spacing"_key,
        field{
            0.0f,
            attr<DisplayName>("Spacing"),
            attr<Description>("Space between child elements in pixels."),
            attr<Category>("Layout"),
            attr<Range>(0, 256),
            attr<Step>(0.5)});
    proto->addField(
        "width"_key,
        field{
            0.0f,
            attr<DisplayName>("Width"),
            attr<Description>("Column width hint used when this element is a direct child of a "
                              "HorizontalLayout: 0 (default) sizes to content, exactly like a plain "
                              "child with no width opinion. A positive value reserves that many fixed "
                              "pixels. A negative value makes it a stretch column, sharing whatever "
                              "width remains after fixed columns and spacing are subtracted, weighted "
                              "by its magnitude relative to other stretch columns (mirrors "
                              "ImGuiTableColumnFlags_WidthStretch's weight convention). Ignored outside "
                              "a HorizontalLayout."),
            attr<Category>("Layout")});
    proto->addField(
        "height"_key,
        field{
            0.0f,
            attr<DisplayName>("Height"),
            attr<Description>("Row height hint used when this element is a direct child of a "
                              "VerticalLayout: 0 (default) sizes to content, exactly like a plain "
                              "child with no height opinion. A positive value reserves that many fixed "
                              "pixels. A negative value makes it a stretch row, sharing whatever "
                              "height remains after fixed/auto rows and spacing are subtracted, "
                              "weighted by its magnitude relative to other stretch rows (mirrors "
                              "Layout.width's convention). Auto rows (0) are still accounted for: "
                              "their height from the previous rendered frame is reserved for this "
                              "frame's stretch-row sizing, so a fixed header/footer plus one "
                              "stretch-filling body works regardless of child order. Ignored outside "
                              "a VerticalLayout."),
            attr<Category>("Layout")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Layout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Base container that arranges child elements."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Layout"_key));
  }

  // VerticalLayout — stacks children top-to-bottom; no extra fields.
  {
    auto proto = dynamic_ptr{"VerticalLayout"_key, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Vertical Layout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Stacks children top-to-bottom."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Layout"_key,
        dynamic::make_factory<ui_element>("wish"_key, "VerticalLayout"_key));
  }

  // HorizontalLayout — places children side by side.
  {
    auto proto = dynamic_ptr{"HorizontalLayout"_key, {}};
    proto->addField(
        "align"_key,
        field{
            std::string{"left"},
            attr<DisplayName>("Align"),
            attr<Description>("Horizontal alignment of children: \"left\" (default) or \"right\". "
                              "Right alignment offsets children so they flush to the content edge; "
                              "requires each child to have an explicit width field."),
            attr<Category>("Layout")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Horizontal Layout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Arranges children left-to-right."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Layout"_key,
        dynamic::make_factory<ui_element>("wish"_key, "HorizontalLayout"_key));
  }
}

} // namespace bdg::wish
