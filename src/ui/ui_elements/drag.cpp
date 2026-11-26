// MIT License © 2025 Binary Dice Games
/// @file drag.cpp
/// @brief Registers DragFloat and DragInt prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_drag() {
  // DragFloat — click-and-drag (or double-click to type) a float value.
  {
    auto proto = dynamic_ptr{"DragFloat"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Caption shown to the left of the drag widget."),
            attr<Category>("Content")});
    proto->addField(
        "value"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Value"),
            attr<Description>("Current float value."),
            attr<Category>("State")});
    proto->addField(
        "speed"_rkey,
        field{
            float{1.0f},
            attr<DisplayName>("Speed"),
            attr<Description>("Change per pixel dragged."),
            attr<Category>("Behavior")});
    proto->addField(
        "min"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Min"),
            attr<Description>("Lower clamp. When min == max the value is unclamped."),
            attr<Category>("Behavior")});
    proto->addField(
        "max"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Max"),
            attr<Description>("Upper clamp. When min == max the value is unclamped."),
            attr<Category>("Behavior")});
    proto->addField(
        "format"_rkey,
        field{
            std::string{"%.3f"},
            attr<DisplayName>("Format"),
            attr<Description>("printf-style display format."),
            attr<Category>("Display")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DragFloat"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A float value editor: click-and-drag to change, double-click to type. "
                          "Emits 'changed' with {value: float} when edited."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_drag_float>("wish"_key, "DragFloat"_key));
  }

  // DragFloatN — one drag field per vector component, with colored axis
  // tags (X/Y/Z/W), e.g. an inspector's position/scale.
  {
    auto proto = dynamic_ptr{"DragFloatN"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Optional caption to the right of the fields (ImGui convention); "
                              "usually empty inside a property table."),
            attr<Category>("Content")});
    proto->addField(
        "value"_rkey,
        field{
            std::vector<float>{0.0f, 0.0f},
            attr<DisplayName>("Value"),
            attr<Description>("Current components (2 to 4); the count decides how many fields are shown."),
            attr<Category>("State")});
    proto->addField(
        "speed"_rkey,
        field{
            float{0.1f},
            attr<DisplayName>("Speed"),
            attr<Description>("Change per pixel dragged."),
            attr<Category>("Behavior")});
    proto->addField(
        "min"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Min"),
            attr<Description>("Lower clamp. When min == max the values are unclamped."),
            attr<Category>("Behavior")});
    proto->addField(
        "max"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Max"),
            attr<Description>("Upper clamp. When min == max the values are unclamped."),
            attr<Category>("Behavior")});
    proto->addField(
        "format"_rkey,
        field{
            std::string{"%.2f"},
            attr<DisplayName>("Format"),
            attr<Description>("printf-style display format of every component."),
            attr<Category>("Display")});
    proto->addField(
        "axis_labels"_rkey,
        field{
            std::string{"X\nY\nZ\nW"},
            attr<DisplayName>("Axis Labels"),
            attr<Description>("Newline-separated tag drawn in front of each component, tinted "
                              "red/green/blue/gray like Godot and Unity; an empty line draws no tag."),
            attr<Category>("Display")});
    proto->addField(
        "live"_rkey,
        field{
            false,
            attr<DisplayName>("Live"),
            attr<Description>("When true, 'changed' fires on every edit while dragging; when false "
                              "(default) only once the edit is committed (mouse release, Enter, focus "
                              "loss) -- one undo step per drag."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DragFloatN"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A 2-4 component float vector editor: one drag field per component with an "
                          "axis tag. Emits 'changed' with {value: float[]}."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DragFloatN"_key));
  }

  // DragInt — click-and-drag (or double-click to type) an integer value.
  {
    auto proto = dynamic_ptr{"DragInt"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Caption shown to the left of the drag widget."),
            attr<Category>("Content")});
    proto->addField(
        "value"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Value"),
            attr<Description>("Current integer value."),
            attr<Category>("State")});
    proto->addField(
        "speed"_rkey,
        field{
            float{1.0f},
            attr<DisplayName>("Speed"),
            attr<Description>("Change per pixel dragged."),
            attr<Category>("Behavior")});
    proto->addField(
        "min"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Min"),
            attr<Description>("Lower clamp. When min == max the value is unclamped."),
            attr<Category>("Behavior")});
    proto->addField(
        "max"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Max"),
            attr<Description>("Upper clamp. When min == max the value is unclamped."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DragInt"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("An integer value editor: click-and-drag to change, double-click to type. "
                          "Emits 'changed' with {value: int} when edited."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_drag_int>("wish"_key, "DragInt"_key));
  }
}

} // namespace bdg::wish
