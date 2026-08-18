// MIT License © 2025 Binary Dice Games
/// @file themes.hpp
/// @brief Declares each built-in theme's registration entry point.
///
/// Each `theme_<name>.cpp` file in this directory defines one
/// `register_theme_<name>()` function that calls
/// `imgui_renderer::register_theme()` (imgui_renderer.hpp) for its theme.
/// These are declared here -- rather than left as file-local static
/// initializers -- and explicitly called from
/// `imgui_renderer.cpp`'s `register_built_in_themes()` so linkage doesn't
/// depend on static-initialization order or on whether a particular archive
/// member happens to get pulled into the final binary (a static library
/// only links in an object file when something references a symbol from
/// it -- a file with nothing but a side-effecting static initializer and no
/// referenced symbol can silently be dropped).
#pragma once

namespace bdg::wish {

void register_theme_dark();
void register_theme_light();
void register_theme_classic();
void register_theme_wish();

} // namespace bdg::wish
