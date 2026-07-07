// MIT License © 2025 Binary Dice Games
/// @file window.hpp
/// @brief Concrete ui_root for the "Window" bison class.
#pragma once

#include <ui_root.hpp>

namespace bdg::wish {

/// @brief Typed C++ class for wish Window elements.
///
/// Sits at the end of the primary inheritance chain:
///   `bison::dynamic ← ui_element ← ui_root ← window`
///
/// Because `window` inherits `ui_root`, only Window elements (not generic
/// ui_elements) are registered in `context::top_level_handlers` and receive
/// `on_event` callbacks from the render loop.
class window : public ui_root {
 public:
  explicit window(bison::dynamic&& base);
};

} // namespace bdg::wish
