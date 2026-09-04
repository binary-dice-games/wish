// MIT License © 2025 Binary Dice Games
/// @file docking.cpp
/// @brief Registers the DockSpaceViewport and DockSpace prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

namespace {

// Shared between DockSpaceViewport.flags and DockSpace.flags -- both map to
// the same ImGui::DockSpace() flags parameter.
EnumFlags::table dock_node_flags_table() {
  return {
      {"KeepAliveOnly", 1 << 0},
      {"NoDockingOverCentralNode", 1 << 2},
      {"PassthruCentralNode", 1 << 3},
      {"NoDockingSplit", 1 << 4},
      {"NoResize", 1 << 5},
      {"AutoHideTabBar", 1 << 6},
      {"NoUndocking", 1 << 7},
  };
}

} // namespace

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
            attr<Description>("ImGuiDockNodeFlags bitmask (combine names with '|')."),
            attr<Category>("Behavior"),
            attr<EnumFlags>(dock_node_flags_table())});
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
        dynamic::make_factory<ui_dockspace_viewport>("wish"_key, "DockSpaceViewport"_key));
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
            attr<Description>("ImGuiDockNodeFlags bitmask (combine names with '|')."),
            attr<Category>("Behavior"),
            attr<EnumFlags>(dock_node_flags_table())});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockSpace"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>("Inline dockspace inside an existing window."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_dockspace>("wish"_key, "DockSpace"_key));
  }

  // ── DockLayout / DockSplit / DockArea ─────────────────────────────────────
  // A declarative default docking arrangement. The renderer walks the
  // DockSplit/DockArea child tree once and realizes it via ImGui's
  // DockBuilder API (render_dock_layout, imgui_ui_renderer.cpp), then leaves
  // the result to imgui.ini like any user drag. Flows through every path
  // that carries a UI tree: a server-side form (form::set_default_dock_layout),
  // a client-registered template descriptor, or a hand-authored import_json
  // tree.
  {
    auto proto = dynamic_ptr{"DockLayout"_rkey, {}};
    proto->addField(
        "target"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Target"),
            attr<Description>("Dockspace id to seed. Empty (default) targets the ambient host/viewport "
                              "dockspace; a non-empty value is hashed like DockSpace.id to seed a named "
                              "inline dockspace instead."),
            attr<Category>("Behavior")});
    proto->addField(
        "version"_rkey,
        field{
            int32_t{1},
            attr<DisplayName>("Version"),
            attr<Description>("Layout revision. The arrangement is applied once per distinct value "
                              "(on the first run whose imgui.ini has no node for the target, or after "
                              "this number increases); bump it to re-apply the default even if the "
                              "user has rearranged the windows."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockLayout"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "Declarative default docking arrangement. Its single child is a DockSplit or DockArea tree "
        "naming Window paths. Realized once via DockBuilder, then owned by imgui.ini. No-op if there "
        "is no ambient dockspace and no target. Draws nothing."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_dock_layout>("wish"_key, "DockLayout"_key));
  }

  {
    auto proto = dynamic_ptr{"DockSplit"_rkey, {}};
    proto->addField(
        "dir"_rkey,
        field{
            std::string{"left"},
            attr<DisplayName>("Direction"),
            attr<Description>("\"left\", \"right\", \"up\", or \"down\". The first child is placed in "
                              "this direction, the second in the opposite one."),
            attr<Category>("Layout")});
    proto->addField(
        "ratio"_rkey,
        field{
            0.5f,
            attr<DisplayName>("Ratio"),
            attr<Description>("Fraction (0..1) of the parent node given to the first child (the one "
                              "placed in \"dir\")."),
            attr<Category>("Layout"),
            attr<Range>(0.05, 0.95),
            attr<Step>(0.05)});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockSplit"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "One binary split in a DockLayout tree. Exactly two ordered children, each a DockSplit or "
        "DockArea. Only meaningful inside a DockLayout."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_dock_split>("wish"_key, "DockSplit"_key));
  }

  {
    auto proto = dynamic_ptr{"DockArea"_rkey, {}};
    proto->addField(
        "windows"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Windows"),
            attr<Description>("Newline-separated list of Window paths to dock into this node as tabs. "
                              "A path is the Window's \"__path__\": its form root key server-side, or "
                              "its descriptor dot-path in a template."),
            attr<Category>("Behavior")});
    proto->addField(
        "focused"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Focused"),
            attr<Description>("Which of \"windows\" is the initially-selected tab. Empty means the "
                              "first entry."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("DockArea"));
    (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
        "A leaf of a DockLayout tree: one dock node holding one or more tabbed Windows. Only "
        "meaningful inside a DockLayout."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Element"_key,
        dynamic::make_factory<ui_dock_area>("wish"_key, "DockArea"_key));
  }
}

} // namespace bdg::wish
