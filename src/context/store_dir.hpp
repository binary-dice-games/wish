// MIT License © 2026 Binary Dice Games
/// @file store_dir.hpp
/// @brief Default location of wish's per-user files (`~/.wish`).
#pragma once

#include <filesystem>

namespace bdg::wish {

/**
 * @brief Default directory for persistent stores and per-user config:
 *        `~/.wish`.
 *
 * `~` is `$HOME` (`%USERPROFILE%` on native Windows); falls back to `.wish`
 * relative to the working directory if neither is set.
 */
std::filesystem::path default_store_dir();

} // namespace bdg::wish
