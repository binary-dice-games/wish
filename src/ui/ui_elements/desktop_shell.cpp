// MIT License © 2025 Binary Dice Games
/// @file desktop_shell.cpp
/// @brief Registers the DesktopShell and DesktopApp prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

void register_desktop_shell() {
  // DesktopShell — registered by a session (the `wish desktop` bridge) as a
  // top-level object to turn the server's host chrome into a desktop:
  // wallpaper, desktop icons, and a bottom taskbar with a Start menu, pinned
  // apps, open windows and a tray. Like MenuBarExtension it is never
  // rendered as a standalone window: the host renderer finds it by class
  // among each session's top-level objects (see render_desktop_shell() in
  // src/imgui/imgui_desktop_shell.hpp) and has no render_node dispatch entry.
  {
    auto proto = dynamic_ptr{"DesktopShell"_rkey, {}};
    proto->addField(
        "wallpaper"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Wallpaper"),
            attr<Description>("Desktop background image, relative to the session's resource folder "
                              "(e.g. \"private/apps/wish.desktop/wallpaper.png\"); scaled to cover the "
                              "screen. Empty draws the built-in gradient. Same sandbox rules as Image.src."),
            attr<Category>("Appearance")});
    proto->addField(
        "user"_rkey,
        field{
            std::string{},
            attr<DisplayName>("User"),
            attr<Description>("User name shown at the bottom of the Start menu."),
            attr<Category>("Content")});
    proto->addField(
        "show_icons"_rkey,
        field{
            true,
            attr<DisplayName>("Show desktop icons"),
            attr<Description>("Draws the apps whose on_desktop is true as icons on the wallpaper."),
            attr<Category>("Appearance")});
    proto->addField(
        "clients"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Clients"),
            attr<Description>("Number of connected client processes, shown in the tray."),
            attr<Category>("State")});
    proto->addField(
        "time_format"_rkey,
        field{
            std::string{"%H:%M"},
            attr<DisplayName>("Time format"),
            attr<Description>("strftime() format of the tray clock's time line."),
            attr<Category>("Appearance")});
    proto->addField(
        "date_format"_rkey,
        field{
            std::string{"%d/%m/%Y"},
            attr<DisplayName>("Date format"),
            attr<Description>("strftime() format of the tray clock's date line."),
            attr<Category>("Appearance")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DesktopShell"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Turns the server's host window into a desktop: wallpaper, desktop icons, and a taskbar "
                          "with a Start menu, pinned apps, open windows, a theme toggle and a clock. Register as a "
                          "top-level object; children are DesktopApp entries. Emits \"launch\" {app}, \"pin\" "
                          "{app, pinned}, \"desktop_icon\" {app, on_desktop}, \"theme\" {preset} and \"quit\". "
                          "Removed automatically when the owning session disconnects."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DesktopShell"_key));
  }

  // DesktopApp — one launchable app listed by a DesktopShell.
  {
    auto proto = dynamic_ptr{"DesktopApp"_rkey, {}};
    proto->addField(
        "app"_rkey,
        field{
            std::string{},
            attr<DisplayName>("App"),
            attr<Description>("Identifier sent back in the shell's events (e.g. \"bdg/desktop/nano\")."),
            attr<Category>("Content")});
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Name shown in the Start menu, on the taskbar and under the desktop icon."),
            attr<Category>("Content")});
    proto->addField(
        "description"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Description"),
            attr<Description>("Tooltip text; also matched by the Start menu search."),
            attr<Category>("Content")});
    proto->addField(
        "collection"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Collection"),
            attr<Description>("Start menu group heading (e.g. \"desktop\", \"dev\")."),
            attr<Category>("Content")});
    proto->addField(
        "icon"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Icon"),
            attr<Description>("Image path relative to the session's resource folder, drawn tinted to the "
                              "text color (e.g. \"res/icons/edit.png\"). Empty uses a generic app icon."),
            attr<Category>("Appearance")});
    proto->addField(
        "pinned"_rkey,
        field{
            false,
            attr<DisplayName>("Pinned"),
            attr<Description>("Shown on the taskbar even while not running. The shell never changes it "
                              "itself: it emits \"pin\" and the owner sets the field."),
            attr<Category>("State")});
    proto->addField(
        "on_desktop"_rkey,
        field{
            false,
            attr<DisplayName>("On desktop"),
            attr<Description>("Shown as an icon on the desktop. Changed by the owner on \"desktop_icon\"."),
            attr<Category>("State")});
    proto->addField(
        "sessions"_rkey,
        field{
            std::vector<int32_t>{},
            attr<DisplayName>("Sessions"),
            attr<Description>("Object groups (the bridge's downstream session ids) of the running "
                              "instances of this app. The shell maps each open Window to its app "
                              "through these, for its taskbar icon and running indicator."),
            attr<Category>("State")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DesktopApp"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("An app listed by a DesktopShell: Start menu entry, optional pinned "
                          "taskbar button and optional desktop icon."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DesktopApp"_key));
  }
}

} // namespace bdg::wish
