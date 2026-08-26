// MIT License © 2025 Binary Dice Games
/// @file tabs.cpp
/// @brief Registers TabBar and TabItem prototypes.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_tabs() {
  // TabBar — container; children should be TabItem elements.
  {
    auto proto = dynamic_ptr{"TabBar"_rkey, {}};
    proto->addField(
        "id"_rkey,
        field{
            std::string{"##tabbar"},
            attr<DisplayName>("ID"),
            attr<Description>("ImGui identifier for this tab bar. Must be unique within its window."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("TabBar"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A horizontal tab bar. Children should be TabItem elements."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_tab_bar>("wish"_key, "TabBar"_key));
  }

  // TabItem — one tab page inside a TabBar; its children are the page content.
  {
    auto proto = dynamic_ptr{"TabItem"_rkey, {}};
    proto->addField(
        "label"_rkey,
        field{
            std::string{},
            attr<DisplayName>("Label"),
            attr<Description>("Text shown on the tab button."),
            attr<Category>("Content")});
    proto->addField(
        "closable"_rkey,
        field{
            false,
            attr<DisplayName>("Closable"),
            attr<Description>("When true, a close button appears on the tab."),
            attr<Category>("Behavior")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("TabItem"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("A single tab page inside a TabBar. "
                          "Emits 'selected' when it becomes the active tab. "
                          "Emits 'closed' when the close button is clicked (requires closable: true)."));
    dynamic::addClass(
        "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_tab_item>("wish"_key, "TabItem"_key));
  }
}

} // namespace bdg::wish
