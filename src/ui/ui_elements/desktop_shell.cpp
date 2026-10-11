// MIT License © 2025 Binary Dice Games
/// @file desktop_shell.cpp
/// @brief Registers the DesktopShell, DesktopMenu and DesktopApp prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

namespace {

void add_text_field(
    dynamic& proto,
    key_t key,
    const char* name,
    const char* description,
    const char* category,
    std::string def = {}) {
  proto.addField(
      key, field{std::move(def), attr<DisplayName>(name), attr<Description>(description), attr<Category>(category)});
}

void add_bool_field(
    dynamic& proto,
    key_t key,
    const char* name,
    const char* description,
    const char* category,
    bool def) {
  proto.addField(key, field{def, attr<DisplayName>(name), attr<Description>(description), attr<Category>(category)});
}

} // namespace

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
    add_text_field(
        *proto,
        "wallpaper"_rkey,
        "Wallpaper",
        "Desktop background image, relative to the session's resource folder "
        "(e.g. \"private/apps/wish.desktop/wallpaper.png\"); scaled to cover the "
        "screen. Empty draws the built-in gradient. Same sandbox rules as Image.src.",
        "Appearance");
    add_text_field(*proto, "user"_rkey, "User", "User name shown at the bottom of the Start menu.", "Content");
    add_bool_field(
        *proto,
        "show_icons"_rkey,
        "Show desktop icons",
        "Draws the entries whose on_desktop is true as icons on the wallpaper.",
        "Appearance",
        true);
    proto->addField(
        "clients"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Clients"),
            attr<Description>("Number of connected client processes, shown in the tray."),
            attr<Category>("State")});
    add_text_field(
        *proto,
        "menu_error"_rkey,
        "Menu error",
        "Shown at the top of the Start menu when the menu configuration has an error.",
        "State");
    add_bool_field(
        *proto,
        "shutdown"_rkey,
        "Shut down",
        "Set to true to ask the server hosting the shell to shut down (the desktop's "
        "\"shutdown_server\" action).",
        "State",
        false);
    add_text_field(
        *proto,
        "time_format"_rkey,
        "Time format",
        "strftime() format of the tray clock's time line.",
        "Appearance",
        "%H:%M");
    add_text_field(
        *proto,
        "date_format"_rkey,
        "Date format",
        "strftime() format of the tray clock's date line.",
        "Appearance",
        "%d/%m/%Y");
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DesktopShell"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Turns the server's host window into a desktop: wallpaper, desktop icons, and a taskbar "
                          "with a Start menu, pinned apps, open windows, a theme toggle and a clock. Register as a "
                          "top-level object; children are the Start menu tree: DesktopApp entries, DesktopMenu "
                          "folders and Separators (a DesktopMenu with role \"power\" fills the power button). "
                          "Emits \"activate\" {id}, \"pin\" {id, pinned}, \"desktop_icon\" {id, on_desktop} and "
                          "\"theme\" {preset}. Removed automatically when the owning session disconnects."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DesktopShell"_key));
  }

  // DesktopMenu — a Start menu folder.
  {
    auto proto = dynamic_ptr{"DesktopMenu"_rkey, {}};
    add_text_field(
        *proto, "id"_rkey, "Id", "Unique id; keeps the folder's expanded state across menu reloads.", "Content");
    add_text_field(*proto, "label"_rkey, "Label", "Folder name.", "Content");
    add_text_field(*proto, "icon"_rkey, "Icon", "Image path relative to the session's resource folder.", "Appearance");
    add_text_field(*proto, "tooltip"_rkey, "Tooltip", "Tooltip text.", "Content");
    add_text_field(
        *proto,
        "role"_rkey,
        "Role",
        "\"power\": its entries fill the Start menu's power button instead of being a folder "
        "in the list.",
        "Behavior");
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DesktopMenu"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A folder of a DesktopShell's Start menu. Children are DesktopApp entries, nested "
                          "DesktopMenu folders and Separators."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DesktopMenu"_key));
  }

  // DesktopApp — one Start menu entry: an app to start, or an action.
  {
    auto proto = dynamic_ptr{"DesktopApp"_rkey, {}};
    add_text_field(
        *proto,
        "id"_rkey,
        "Id",
        "Unique entry id, sent back in the shell's events (e.g. \"bdg/desktop/nano\").",
        "Content");
    add_text_field(
        *proto,
        "app"_rkey,
        "App",
        "The \"app\" its clients connect as (see sessions): ties their windows to this entry. "
        "Empty for an entry that starts no app, e.g. an action.",
        "Content");
    add_text_field(
        *proto,
        "label"_rkey,
        "Label",
        "Name shown in the Start menu, on the taskbar and under the desktop icon.",
        "Content");
    add_text_field(
        *proto, "tooltip"_rkey, "Tooltip", "Tooltip text; also matched by the Start menu search.", "Content");
    add_text_field(
        *proto,
        "icon"_rkey,
        "Icon",
        "Image path relative to the session's resource folder, drawn tinted to the "
        "text color (e.g. \"res/icons/edit.png\"). Empty uses a generic app icon.",
        "Appearance");
    add_bool_field(
        *proto,
        "pinned"_rkey,
        "Pinned",
        "Shown on the taskbar. The shell never changes it itself: it emits \"pin\" and the "
        "owner sets the field.",
        "State",
        false);
    add_bool_field(
        *proto,
        "on_desktop"_rkey,
        "On desktop",
        "Shown as an icon on the desktop. Changed by the owner on \"desktop_icon\".",
        "State",
        false);
    add_bool_field(
        *proto,
        "listed"_rkey,
        "Listed",
        "Shown in the Start menu and available as a pinned button or desktop icon. "
        "False for an app the owner can't start itself (e.g. a client-ABI app that "
        "connected on its own), which only appears on the taskbar while running.",
        "State",
        true);
    proto->addField(
        "sessions"_rkey,
        field{
            std::vector<int32_t>{},
            attr<DisplayName>("Sessions"),
            attr<Description>("Object groups (the bridge's downstream session ids) of the running "
                              "instances of this entry's app. The shell maps each open Window to its "
                              "entry through these, for its taskbar icon and running indicator."),
            attr<Category>("State")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DesktopApp"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A DesktopShell Start menu entry -- an app to start or an action -- with an "
                          "optional pinned taskbar button and desktop icon."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DesktopApp"_key));
  }
}

} // namespace bdg::wish
