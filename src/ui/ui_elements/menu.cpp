// MIT License © 2025 Binary Dice Games
/// @file menu.cpp
/// @brief Registers MenuBar, Menu, and MenuItem prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_menu() {
  // MenuBar — rendered via BeginMenuBar/EndMenuBar inside a Window.
  // The Window renderer detects a direct MenuBar child and automatically
  // sets ImGuiWindowFlags_MenuBar on the window.
  {
    auto proto = dynamic_ptr{"MenuBar"_key, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MenuBar"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Menu bar container. Must be a direct child of a Window. "
                          "Children should be Menu elements."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "MenuBar"_key));
  }

  // Menu — drop-down submenu. Children are MenuItem, Menu, or Separator.
  {
    auto proto = dynamic_ptr{"Menu"_key, {}};
    proto->addField(
        "label"_key,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text shown in the parent menu bar or parent menu."),
            attr<Category>("Content")});
    proto->addField(
        "enabled"_key,
        field{
            true,
            attr<DisplayName>("Enabled"),
            attr<Description>("When false, the menu header is grayed out and cannot be opened."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Menu"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A drop-down menu. "
                          "Children may be MenuItem, Menu (submenu), or Separator elements."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Menu"_key));
  }

  // MenuItem — a single selectable item inside a Menu.
  {
    auto proto = dynamic_ptr{"MenuItem"_key, {}};
    proto->addField(
        "label"_key,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text of the menu item."),
            attr<Category>("Content")});
    proto->addField(
        "shortcut"_key,
        field{
            std::string{},
            attr<DisplayName>("Shortcut"),
            attr<Description>("Optional shortcut hint displayed on the right (decorative only)."),
            attr<Category>("Content")});
    proto->addField(
        "checked"_key,
        field{
            false,
            attr<DisplayName>("Checked"),
            attr<Description>("Shows a check mark. Updated by the renderer on click."),
            attr<Category>("State")});
    proto->addField(
        "enabled"_key,
        field{
            true,
            attr<DisplayName>("Enabled"),
            attr<Description>("When false, the item is grayed out and cannot be clicked."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MenuItem"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A selectable item inside a Menu. "
                          "Emits 'clicked' with {checked: bool} when activated."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "MenuItem"_key));
  }

  // MenuBarExtension — registered by a session (e.g. the desktop bridge) as
  // a top-level object to splice extra content into the *server's* own
  // chrome menu bar, instead of creating a competing menu bar/dockspace.
  // Never rendered standalone: the server's render_server_frame() finds it
  // by class among each session's top-level objects and draws its children
  // (Menu/Label) directly inside its own already-open BeginMenuBar/EndMenuBar
  // block via render_children(). It has no render_node dispatch entry of its
  // own.
  {
    auto proto = dynamic_ptr{"MenuBarExtension"_key, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MenuBarExtension"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "Extends the server's chrome menu bar with additional content. Register as a top-level "
        "object; children should be Menu or a trailing Label (e.g. a clock), same as MenuBar. "
        "Removed automatically when the owning session disconnects."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "MenuBarExtension"_key));
  }
}

} // namespace bdg::wish
