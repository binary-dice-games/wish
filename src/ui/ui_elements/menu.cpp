// MIT License © 2025 Binary Dice Games
/// @file menu.cpp
/// @brief Registers MenuBar, Menu, MenuItem, and MenuButton prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_menu() {
  // MenuBar — rendered via BeginMenuBar/EndMenuBar inside a Window.
  // The Window renderer detects a direct MenuBar child and automatically
  // sets ImGuiWindowFlags_MenuBar on the window.
  {
    auto proto = dynamic_ptr{"MenuBar"_rkey, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MenuBar"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Menu bar container. Must be a direct child of a Window. "
                          "Children should be Menu elements."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "MenuBar"_key));
  }

  // Menu — drop-down submenu. Children are MenuItem, Menu, or Separator.
  {
    auto proto = dynamic_ptr{"Menu"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text shown in the parent menu bar or parent menu."),
            attr<Category>("Content")});
    proto->addField(
        "enabled"_rkey,
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
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_menu>("wish"_key, "Menu"_key));
  }

  // MenuItem — a single selectable item inside a Menu.
  {
    auto proto = dynamic_ptr{"MenuItem"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text of the menu item."),
            attr<Category>("Content")});
    proto->addField(
        "shortcut"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Shortcut"),
            attr<Description>("Optional shortcut hint displayed on the right (decorative only)."),
            attr<Category>("Content")});
    proto->addField(
        "checked"_rkey,
        field{
            false,
            attr<DisplayName>("Checked"),
            attr<Description>("Shows a check mark. Display-only: the renderer never modifies "
                              "this itself (clicking does not toggle it) -- set it explicitly, "
                              "e.g. for a radio-style submenu recomputed from server state."),
            attr<Category>("State")});
    proto->addField(
        "enabled"_rkey,
        field{
            true,
            attr<DisplayName>("Enabled"),
            attr<Description>("When false, the item is grayed out and cannot be clicked."),
            attr<Category>("Behavior")});
    proto->addField(
        "copy_text"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Copy Text"),
            attr<Description>("When non-empty, clicking this item copies this text to the OS "
                              "clipboard (via ImGui::SetClipboardText, on the render thread, "
                              "before 'clicked' is enqueued) -- a quick way to offer a "
                              "\"Copy ...\" context-menu action without a client round trip."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MenuItem"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A selectable item inside a Menu. "
                          "Emits 'clicked' with {checked: bool} when activated. If 'copy_text' "
                          "is non-empty, that text is also copied to the OS clipboard on click."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_menu_item>("wish"_key, "MenuItem"_key));
  }

  // MenuButton — an ordinary Button that opens a popup containing its own
  // children (Menu/MenuItem/Separator) when clicked, rendered exactly as
  // they would be inside a MenuBar -- but unlike MenuBar/Menu, it needs no
  // surrounding menu context of its own (no Window menu-bar strip, no
  // already-open parent Menu/popup): it opens that context itself via
  // ImGui::OpenPopup()/BeginPopup() on click, so it can appear anywhere an
  // ordinary Button can (e.g. a toolbar).
  {
    auto proto = dynamic_ptr{"MenuButton"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Button caption text."),
            attr<Category>("Content")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MenuButton"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A button that opens a popup menu when clicked. Children should be "
                          "Menu, MenuItem, or Separator elements, rendered inside the popup "
                          "exactly as they would inside a MenuBar."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_menu_button>("wish"_key, "MenuButton"_key));
  }

  // ContextMenu — a right-click popup. Its own render dispatch entry
  // (render_context_menu, imgui_ui_renderer.cpp) attaches it to "the last
  // ImGui item drawn" via ImGui::BeginPopupContextItem(), exactly like
  // MenuButton attaches its popup to its own trigger Button -- the
  // difference is ContextMenu draws nothing itself, so whichever sibling
  // renders immediately before it in the same parent becomes the item the
  // right-click is detected on. That works for any normal widget (Button,
  // Selectable, ...) reached through the generic render_children() dispatch.
  // A `TableRow` is a special case: its children are cells consumed
  // positionally as table columns, and cell content (e.g. Label) does not
  // register a stable ImGui item id for BeginPopupContextItem to attach to.
  // render_table() therefore excludes a ContextMenu child from column
  // layout and renders it itself, right after the row's own hit-test
  // Selectable, so the popup attaches to the row as a whole (see
  // render_table() in imgui_ui_renderer.cpp and TableRow's docs).
  {
    auto proto = dynamic_ptr{"ContextMenu"_rkey, {}};
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("ContextMenu"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "A right-click popup menu. Opens when the previous sibling in the same "
        "parent is right-clicked (or, as a TableRow child, when that row is "
        "right-clicked). Children should be MenuItem, Menu (submenu), or "
        "Separator elements, rendered inside the popup exactly as they would "
        "inside a MenuBar."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "ContextMenu"_key));
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
    auto proto = dynamic_ptr{"MenuBarExtension"_rkey, {}};
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
