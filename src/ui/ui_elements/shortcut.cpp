// MIT License © 2026 Binary Dice Games
/// @file shortcut.cpp
/// @brief Registers the Shortcut prototype: an invisible keyboard binding.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_shortcut() {
  auto proto = dynamic_ptr{"Shortcut"_rkey, {}};
  proto->addField(
      "keys"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Keys"),
          attr<Description>("Key combination, e.g. \"F5\", \"Ctrl+S\", \"Ctrl+Shift+N\", \"Alt+F4\" "
                            "(modifiers Ctrl/Shift/Alt/Super; keys A-Z, 0-9, F1-F24, Enter, Esc, Tab, "
                            "Space, Del, Ins, Home, End, PgUp, PgDn, arrows, punctuation). An invalid "
                            "string binds nothing."),
          attr<Category>("Behavior")});
  proto->addField(
      "enabled"_rkey,
      field{
          true,
          attr<DisplayName>("Enabled"),
          attr<Description>("When false, the binding is ignored."),
          attr<Category>("Behavior")});
  proto->addField(
      "global"_rkey,
      field{
          false,
          attr<DisplayName>("Global"),
          attr<Description>("When false (the default), the keys only fire while the window containing "
                            "this element has keyboard focus, like a Windows accelerator. When true, they "
                            "fire whichever window is focused, unless the focused window binds the same "
                            "keys itself."),
          attr<Category>("Behavior")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Shortcut"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("An invisible keyboard shortcut (e.g. F1 for help, F5 to refresh). Draws nothing "
                        "and takes no layout space. Emits 'pressed' when its keys are pressed."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_shortcut>("wish"_key, "Shortcut"_key));
}

} // namespace bdg::wish
