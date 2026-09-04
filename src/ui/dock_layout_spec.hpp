// MIT License © 2026 Binary Dice Games
/// @file dock_layout_spec.hpp
/// @brief Tiny builder for a `DockLayout` element tree.
///
/// Builds the same `DockLayout` / `DockSplit` / `DockArea` `ui_element`
/// objects the template importer produces from a descriptor -- just directly,
/// with no JSON text or `build_ui_node()` round trip. Hand the result to
/// `form::set_default_dock_layout()`.
///
/// A client that registers a template instead declares the arrangement in
/// its descriptor JSON/YAML (`{"type":"DockLayout", ...}`) -- the registered
/// element classes are the shared representation, so nothing extra is needed
/// there.
///
/// Example (see `modules/bdg/dev/docker/server/docker.cpp`):
/// @code
///   using namespace bdg::wish::dock;
///   set_default_dock_layout(layout(
///       split(dir::left, 0.62f,
///           split(dir::down, 0.24f,
///               area({containers_key, images_key}, containers_key),
///               area({console_key})),
///           area({logs_key, inspect_key}, logs_key))));
/// @endcode
#pragma once

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <initializer_list>
#include <string>

namespace bdg::wish::dock {

/// @brief Which way a `DockSplit`'s first child is placed.
enum class dir { left, right, up, down };

namespace detail {

inline const char* dir_name(dir d) {
  switch (d) {
    case dir::left: return "left";
    case dir::right: return "right";
    case dir::up: return "up";
    case dir::down: return "down";
  }
  return "left";
}

inline void set_children(const ui_element_ptr& parent, std::initializer_list<ui_element_ptr> kids) {
  auto arr = bison::dynamic_ptr{bison::key_t{0U}, {}};
  std::size_t i = 0;
  for (const auto& kid : kids)
    (*arr)[i++] = bison::dynamic_ptr{kid};
  (*parent)[bison::key_t{"children"}] = arr;
  parent->refresh_children_order();
}

} // namespace detail

/// @brief A leaf dock node holding one or more tabbed windows.
/// @param windows  Window paths in tab order (each a `Window`'s `__path__`).
/// @param focused  Which of @p windows starts selected; empty ⇒ the first.
inline ui_element_ptr area(std::initializer_list<std::string> windows, const std::string& focused = {}) {
  auto e = ui_element_ptr::create(bison::key_t{"wish"}, bison::key_t{"DockArea"});
  std::string joined;
  bool first = true;
  for (const auto& w : windows) {
    if (!first)
      joined += '\n';
    joined += w;
    first = false;
  }
  (*e)[bison::key_t{"windows"}] = joined;
  if (!focused.empty())
    (*e)[bison::key_t{"focused"}] = focused;
  return e;
}

/// @brief A binary split. @p first is the pane on the @p d side of the
///        parent and gets @p ratio of the space; @p second fills the rest.
///        (`split(dir::left, 0.7, a, b)` ⇒ `a` is the left 70%, `b` the
///        right 30%; `split(dir::down, 0.3, a, b)` ⇒ `a` is the bottom 30%.)
/// @param ratio  Fraction (0..1) of the parent given to @p first.
inline ui_element_ptr split(dir d, float ratio, ui_element_ptr first, ui_element_ptr second) {
  auto e = ui_element_ptr::create(bison::key_t{"wish"}, bison::key_t{"DockSplit"});
  (*e)[bison::key_t{"dir"}] = std::string{detail::dir_name(d)};
  (*e)[bison::key_t{"ratio"}] = ratio;
  detail::set_children(e, {std::move(first), std::move(second)});
  return e;
}

/// @brief Wrap a `DockSplit`/`DockArea` tree into a `DockLayout` root.
/// @param root     The split/area tree.
/// @param version  Layout revision; bump to re-apply after changing the tree.
/// @param target   Dockspace id to seed; empty ⇒ the ambient dockspace.
inline ui_element_ptr layout(ui_element_ptr root, std::int32_t version = 1, const std::string& target = {}) {
  auto e = ui_element_ptr::create(bison::key_t{"wish"}, bison::key_t{"DockLayout"});
  (*e)[bison::key_t{"version"}] = version;
  if (!target.empty())
    (*e)[bison::key_t{"target"}] = target;
  detail::set_children(e, {std::move(root)});
  return e;
}

} // namespace bdg::wish::dock
