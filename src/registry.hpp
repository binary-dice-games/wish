// MIT License © 2025 Binary Dice Games
/// @file registry.hpp
/// @brief Declares register_all(), the entry point that populates the "wish"
///        bison namespace with all built-in UI element class prototypes.
#pragma once

namespace bdg::wish {

/// @brief Register every built-in UI element class in the "wish" bison
///        namespace. Safe to call more than once; duplicate registrations are
///        silently ignored.
void register_all();

} // namespace bdg::wish
