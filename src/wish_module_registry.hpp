// MIT License © 2025 Binary Dice Games
/// @file wish_module_registry.hpp
/// @brief Declares the generated optional-module registration entry point.
#pragma once

namespace bdg::wish {

/// @brief Registers every optional module enabled at CMake configure time.
///
/// Defined in a generated translation unit (see wish_module_registry.cpp.in
/// and cmake/WishModules.cmake); called once from register_all(). Adding a
/// new module never requires editing this declaration or its call site —
/// see src/forms/DESIGN.md's "Module System" section.
void register_optional_modules();

} // namespace bdg::wish
