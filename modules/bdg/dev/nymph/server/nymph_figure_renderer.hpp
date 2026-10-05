// MIT License © 2026 Binary Dice Games
/// @file nymph_figure_renderer.hpp
/// @brief Offscreen rasterizer: draws a wish element tree into an RGBA
///        buffer with no window, for nymph's PNG output.
///
/// See DESIGN.md "4. Key Abstractions" (`nymph::figure_renderer`). Silent
/// mode and edit mode's Save both go through `render_figure()`, which is
/// what makes them produce the same pixels.
#pragma once

#include <context/context.hpp>
#include <ui/ui_element.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bdg::wish::nymph {

/// @brief Size and look of a rendered image (the source's `image:` block).
struct image_options {
  int width{800};          ///< Logical width in pixels, 16-4096.
  int height{500};         ///< Logical height in pixels, 16-4096.
  int scale{1};            ///< Supersampling factor, 1-4.
  std::string theme{"light"}; ///< A registered wish theme name.
  int padding{8};          ///< Pixels between the image edge and the root.
};

/// @brief A rendered image: tightly packed, opaque, 8-bit RGBA rows.
struct image {
  int width{0};  ///< `options.width * options.scale`.
  int height{0}; ///< `options.height * options.scale`.
  std::vector<uint8_t> rgba; ///< `width * height * 4` bytes, top row first.
};

/// @brief Draw @p root (and its children) into a new image.
///
/// Uses a private Dear ImGui / ImPlot / ImPlot3D context trio and SDL's
/// software renderer on an in-memory surface. Creates no window and reads
/// no input. The calling thread's current ImGui, ImPlot and ImPlot3D
/// contexts are restored before returning, also on exception, so this may
/// be called while another ImGui frame is open on the same thread.
///
/// Must be called on the thread that owns ImGui (the server's render
/// thread): ImGui's current-context pointer is a process global.
///
/// @param root     Element to draw, normally a `Plot`, `Plot3D` or a layout.
/// @param s        Session context handed to the element render functions.
/// @param options  Image size, scale, theme and padding.
/// @return The rendered image.
/// @throws std::runtime_error if the options are out of range, the theme is
///         unknown, SDL fails, or wish was built without the SDL3 renderer.
image render_figure(const ui_element& root, const context& s, const image_options& options);

} // namespace bdg::wish::nymph
