// MIT License © 2026 Binary Dice Games
/// @file nymph.hpp
/// @brief Client-side runner for the nymph embedded app.
#pragma once

#include "src/client/wish_app_host.hpp"

namespace bdg::wish {

/// @brief Run nymph. The arguments after `--` select what it does:
///
/// - `render <in> [-o <out.png>]` -- no UI: turn a source text (or a nymph
///   PNG) into a PNG. `<in>` may be `-` to read the source from the console.
/// - `edit <in> [-o <out.png>]` -- open the source in the editing UI.
/// - `view <in>` -- show the chart and its data, read-only.
/// - `extract <in.png> [-o <out>]` -- write out the source a PNG carries.
///
/// A failure prints `nymph: ...` to stderr and sets the host's exit code to 1.
void run_nymph(wish_app_host& s);

} // namespace bdg::wish
