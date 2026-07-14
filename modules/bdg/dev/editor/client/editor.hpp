// MIT License © 2025 Binary Dice Games
/// @file editor.hpp
/// @brief Client-side runner for the Editor embedded app.
#pragma once

#include "src/client/wish_app_host.hpp"

namespace bdg::wish {

/// @brief Run the Editor app: `wish client --run=editor -- path/to/ui.json`.
void run_editor(wish_app_host& s);

} // namespace bdg::wish
