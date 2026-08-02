// MIT License © 2025 Binary Dice Games
/// @file docking.cpp
/// @brief Registers the DockSpaceViewport and DockSpace prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_docking() {
  // ── DockSpaceViewport ─────────────────────────────────────────────────────
  // Creates a fullscreen host window that acts as the docking target for the
  // entire viewport.  Children that are Windows become independent dockable
  // windows; other children (e.g. MenuBar) are rendered inside the host window.
  {
    auto proto = dynamic_ptr{"DockSpaceViewport"_rkey, {}};
    proto->addField(
        "id"_rkey,
        field{
            std::string{"demo_dockspace"},
            attr<DisplayName>("ID"),
            attr<Description>("Identifier used for both the host window and the DockSpace."),
            attr<Category>("Behavior")});
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiDockNodeFlags bitmask."),
            attr<Category>("Behavior")});
    proto->addField(
        "passthru"_rkey,
        field{
            bool{false},
            attr<DisplayName>("Passthru Central Node"),
            attr<Description>("Let the central node be transparent to mouse/keyboard."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockSpaceViewport"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Full-viewport dockspace host. Windows nested as children become dockable."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_element>("wish"_key, "DockSpaceViewport"_key));
  }

  // ── DockSpace ─────────────────────────────────────────────────────────────
  // Embeds a named DockSpace at the current cursor position inside any window.
  {
    auto proto = dynamic_ptr{"DockSpace"_rkey, {}};
    proto->addField(
        "id"_rkey,
        field{
            std::string{"dockspace"},
            attr<DisplayName>("ID"),
            attr<Description>("String identifier hashed to produce the ImGui DockSpace ID."),
            attr<Category>("Behavior")});
    proto->addField(
        "width"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Width"),
            attr<Description>("DockSpace width; 0 fills available width."),
            attr<Category>("Layout")});
    proto->addField(
        "height"_rkey,
        field{
            float{0.0f},
            attr<DisplayName>("Height"),
            attr<Description>("DockSpace height; 0 fills available height."),
            attr<Category>("Layout")});
    proto->addField(
        "flags"_rkey,
        field{
            int32_t{0},
            attr<DisplayName>("Flags"),
            attr<Description>("ImGuiDockNodeFlags bitmask."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockSpace"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Inline dockspace inside an existing window."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "DockSpace"_key));
  }
}

} // namespace bdg::wish
