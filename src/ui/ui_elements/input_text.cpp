// MIT License © 2025 Binary Dice Games
/// @file input_text.cpp
/// @brief Registers the InputText prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_input_text() {
  auto proto = dynamic_ptr{"InputText"_rkey, {}};
  proto->addField(
      "label"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Label"),
          attr<Description>("Input field label."),
          attr<Category>("Content")});
  proto->addField(
      "value"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Value"),
          attr<Description>("Current text value."),
          attr<Category>("State")});
  proto->addField(
      "hint"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Hint"),
          attr<Description>("Placeholder text shown when the field is empty."),
          attr<Category>("Content")});
  proto->addField(
      "max_length"_rkey,
      field{
          int32_t{256},
          attr<DisplayName>("Max Length"),
          attr<Description>("Maximum number of characters the user may enter."),
          attr<Category>("Behavior"),
          attr<Range>(1, 65536),
          attr<Step>(1)});
  proto->addField(
      "width"_rkey,
      field{
          float{0.0f},
          attr<DisplayName>("Width"),
          attr<Description>("Width of the input field in pixels. 0 uses ImGui default. "
                            "-1 fills the remaining content width."),
          attr<Category>("Layout")});
  proto->addField(
      "flags"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Flags"),
          attr<Description>("ImGuiInputTextFlags bitmask (combine names with '|'). "
                            "Notable value: EnterReturnsTrue (fire changed only on Enter)."),
          attr<Category>("Behavior"),
          attr<EnumFlags>(EnumFlags::table{
              {"CharsDecimal", 1 << 0},
              {"CharsHexadecimal", 1 << 1},
              {"CharsScientific", 1 << 2},
              {"CharsUppercase", 1 << 3},
              {"CharsNoBlank", 1 << 4},
              {"AllowTabInput", 1 << 5},
              {"EnterReturnsTrue", 1 << 6},
              {"EscapeClearsAll", 1 << 7},
              {"CtrlEnterForNewLine", 1 << 8},
              {"ReadOnly", 1 << 9},
              {"Password", 1 << 10},
              {"AlwaysOverwrite", 1 << 11},
              {"AutoSelectAll", 1 << 12},
              {"ParseEmptyRefVal", 1 << 13},
              {"DisplayEmptyRefVal", 1 << 14},
              {"NoHorizontalScroll", 1 << 15},
              {"NoUndoRedo", 1 << 16},
              {"ElideLeft", 1 << 17},
              {"CallbackCompletion", 1 << 18},
              {"CallbackHistory", 1 << 19},
              {"CallbackAlways", 1 << 20},
              {"CallbackCharFilter", 1 << 21},
              {"CallbackResize", 1 << 22},
              {"CallbackEdit", 1 << 23},
              {"WordWrap", 1 << 24},
          })});
  proto->addField(
      "multiline"_rkey,
      field{
          bool{false},
          attr<DisplayName>("Multiline"),
          attr<Description>("When true, renders as a resizable multi-line text "
                            "box (ImGui::InputTextMultiline) instead of a "
                            "single-line field."),
          attr<Category>("Behavior")});
  proto->addField(
      "height"_rkey,
      field{
          float{0.0f},
          attr<DisplayName>("Height"),
          attr<Description>("Box height in pixels when multiline is true; 0 "
                            "uses ImGui's default (a few lines)."),
          attr<Category>("Layout")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Input Text"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>("A single-line text input field."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "InputText"_key));
}

} // namespace bdg::wish
