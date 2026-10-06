// MIT License © 2026 Binary Dice Games
/// @file imgui_plot_style.hpp
/// @brief Reads a plot series element's styling fields into an ImPlotSpec or
///        ImPlot3DSpec. Shared by imgui_plot_renderer.cpp and
///        imgui_plot3d_renderer.cpp; the two spec structs have the same
///        member names, so one template serves both.
#pragma once

#include <imgui/imgui_renderer.hpp>
#include <ui/plot_elements/plot_style_fields.hpp>
#include <ui/ui_element.hpp>

#include <string>

namespace bdg::wish {

/// @brief Copy the styling fields of series element @p node (see
///        `add_plot_item_style_fields()`) into @p spec.
///
/// Only fields that were given a non-automatic value are written, so a
/// series that sets none keeps the library's own defaults (which differ
/// between series types and between ImPlot and ImPlot3D). `spec.Flags` is
/// not touched.
///
/// The fields are looked up by key on each call rather than through cached
/// `field*` accessors: a plot has a handful of series, not one per node of a
/// layout traversal, and caching would put six more pointers on each of the
/// many series classes.
///
/// @tparam Spec  `ImPlotSpec` or `ImPlot3DSpec`.
template <typename Spec>
void apply_plot_item_style(Spec& spec, const ui_element& node) {
  if (const auto* color = node.findField<std::string>(bison::key_t{"color"}); color && !color->empty()) {
    spec.LineColor = parse_hex_color(*color);
    spec.FillColor = spec.LineColor;
  }
  if (const auto* fill = node.findField<std::string>(bison::key_t{"fill_color"}); fill && !fill->empty())
    spec.FillColor = parse_hex_color(*fill);
  if (float weight = node.get_as<float>(bison::key_t{"line_weight"}, 1.0f); weight > 0.0f)
    spec.LineWeight = weight;
  if (float alpha = node.get_as<float>(bison::key_t{"fill_alpha"}, -1.0f); alpha >= 0.0f)
    spec.FillAlpha = alpha;
  if (int32_t marker = node.get_as<int32_t>(bison::key_t{"marker"}, kPlotMarkerDefault);
      marker != kPlotMarkerDefault)
    spec.Marker = marker;
  if (float size = node.get_as<float>(bison::key_t{"marker_size"}, 0.0f); size > 0.0f)
    spec.MarkerSize = size;
}

/// @brief The `colormap` field of plot container @p node, or
///        `kPlotColormapDefault` when it does not name one.
inline int32_t plot_colormap_of(const ui_element& node) {
  int32_t colormap = node.get_as<int32_t>(bison::key_t{"colormap"}, kPlotColormapDefault);
  return colormap >= 0 && colormap <= 15 ? colormap : kPlotColormapDefault;
}

} // namespace bdg::wish
