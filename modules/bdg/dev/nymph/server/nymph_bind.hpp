// MIT License © 2026 Binary Dice Games
/// @file nymph_bind.hpp
/// @brief Turns a nymph document into something drawable: validates the
///        format YAML against wish's registered plot classes and replaces
///        every `$column` reference with data from the CSV part.
///
/// `bind()` is the only place a source is rejected for its content, and
/// both the live preview and the PNG are built from its result. It touches
/// no file, no session and no ImGui state. See DESIGN.md "3. Source Format".
#pragma once

#include "nymph_csv.hpp"
#include "nymph_document.hpp"
#include "nymph_figure_renderer.hpp"

#include "src/bison/bison_object.hpp"

#include <string>
#include <vector>

namespace bdg::wish::nymph {

/// @brief A bound document: what to draw and how large.
struct figure {
  image_options options; ///< The format's `image:` block, defaults applied.
  /// The format's `root:` as a generic wish descriptor (the shape
  /// `build_ui_node()` takes), holding only literal values.
  bison::dynamic root;
  table data; ///< The parsed CSV part.
};

/// @brief Validate @p doc and resolve its column references.
///
/// Requires the "wish" class registry to be populated (`register_all()`).
///
/// @throws error, positioned in the whole source, for: YAML that does not
///         parse; a missing `root`; an unknown key under `image` or an
///         out-of-range image option; an element type that is not allowed
///         (or not allowed in that place); a field the class does not have;
///         a value of the wrong kind for its field; an unknown column; a
///         column reference on a field that cannot take one; a non-number
///         in a column used as numbers; any CSV error.
figure bind(const document& doc);

/// @brief Every element type a nymph `root` may contain, sorted: all
///        registered plot classes plus a few layout classes.
std::vector<std::string> allowed_types();

} // namespace bdg::wish::nymph
