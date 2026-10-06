// MIT License © 2026 Binary Dice Games
/// @file plot_style_fields.hpp
/// @brief Styling fields shared by the 2D and 3D plot element registrations:
///        per-series colour / line / marker fields (PlotItem, Plot3DItem) and
///        the colormap choice (Plot, Plot3D). Internal to the registration
///        sources; not part of the public wish API.
///
/// The values map onto ImPlotSpec / ImPlot3DSpec and ImPlotColormap /
/// ImPlot3DColormap, whose members and enum values are the same in both
/// libraries -- see src/imgui/imgui_plot_style.hpp for the render side.
#pragma once

#include "src/bison/bison_object.hpp"

#include <string>

namespace bdg::wish {

/// @brief `marker` value meaning "leave the series type's own default".
inline constexpr int32_t kPlotMarkerDefault = -3;
/// @brief `colormap` value meaning "leave the current colormap".
inline constexpr int32_t kPlotColormapDefault = -1;

/// @brief Names for `marker` (ImPlotMarker / ImPlot3DMarker values).
inline const bison::Enum::table& plot_marker_table() {
  static const bison::Enum::table t{
      {"Default", kPlotMarkerDefault},
      {"None", -2},
      {"Auto", -1},
      {"Circle", 0},
      {"Square", 1},
      {"Diamond", 2},
      {"Up", 3},
      {"Down", 4},
      {"Left", 5},
      {"Right", 6},
      {"Cross", 7},
      {"Plus", 8},
      {"Asterisk", 9},
  };
  return t;
}

/// @brief Names for `colormap` (ImPlotColormap / ImPlot3DColormap values).
inline const bison::Enum::table& plot_colormap_table() {
  static const bison::Enum::table t{
      {"Default", kPlotColormapDefault},
      {"Deep", 0},
      {"Dark", 1},
      {"Pastel", 2},
      {"Paired", 3},
      {"Viridis", 4},
      {"Plasma", 5},
      {"Hot", 6},
      {"Cool", 7},
      {"Pink", 8},
      {"Jet", 9},
      {"Twilight", 10},
      {"RdBu", 11},
      {"BrBG", 12},
      {"PiYG", 13},
      {"Spectral", 14},
      {"Greys", 15},
  };
  return t;
}

/// @brief Add the per-series styling fields to a series base prototype
///        (`PlotItem` or `Plot3DItem`); every concrete series inherits them.
///
/// Every default means "automatic", so a series that sets none of them
/// renders exactly as before these fields existed.
inline void add_plot_item_style_fields(bison::dynamic_ptr& proto) {
  using namespace bison;
  proto->addField(
      "color"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Color"),
          attr<Description>("Series colour as \"#RRGGBB\" or \"#RRGGBBAA\", used for its lines, fills "
                            "and markers. Empty (default) takes the next colour of the plot's colormap."),
          attr<Category>("Style")});
  proto->addField(
      "fill_color"_rkey,
      field{
          std::string{},
          attr<DisplayName>("Fill Color"),
          attr<Description>("Colour of filled areas (bar faces, shaded regions, surfaces) as \"#RRGGBB\" "
                            "or \"#RRGGBBAA\". Empty (default) uses color."),
          attr<Category>("Style")});
  proto->addField(
      "line_weight"_rkey,
      field{
          float{1.0f},
          attr<DisplayName>("Line Weight"),
          attr<Description>("Line thickness in pixels (lines, bar edges, marker edges)."),
          attr<Category>("Style"),
          attr<Range>(0.0, 32.0)});
  proto->addField(
      "fill_alpha"_rkey,
      field{
          float{-1.0f},
          attr<DisplayName>("Fill Alpha"),
          attr<Description>("Opacity of fills from 0 (transparent) to 1 (opaque). "
                            "-1 (default) keeps the series type's own default."),
          attr<Category>("Style"),
          attr<Range>(-1.0, 1.0)});
  proto->addField(
      "marker"_rkey,
      field{
          int32_t{kPlotMarkerDefault},
          attr<DisplayName>("Marker"),
          attr<Description>("Point marker: None, Auto, Circle, Square, Diamond, Up, Down, Left, Right, "
                            "Cross, Plus or Asterisk. Default keeps the series type's own choice "
                            "(markers on a scatter, none on a line)."),
          attr<Category>("Style"),
          attr<Enum>(plot_marker_table())});
  proto->addField(
      "marker_size"_rkey,
      field{
          float{0.0f},
          attr<DisplayName>("Marker Size"),
          attr<Description>("Marker radius in pixels. 0 (default) keeps the automatic size."),
          attr<Category>("Style"),
          attr<Range>(0.0, 64.0)});
}

/// @brief The `colormap` field for a plot container (`Plot` or `Plot3D`).
inline bison::field plot_colormap_field() {
  using namespace bison;
  return field{
      int32_t{kPlotColormapDefault},
      attr<DisplayName>("Colormap"),
      attr<Description>("Colormap for this plot: the colours series take in turn when they set none, "
                        "and the scale of heatmaps and surfaces. Deep (the usual default), Dark, "
                        "Pastel, Paired, Viridis, Plasma, Hot, Cool, Pink, Jet, Twilight, RdBu, BrBG, "
                        "PiYG, Spectral or Greys. Default leaves the current colormap."),
      attr<Category>("Style"),
      attr<Enum>(plot_colormap_table())};
}

} // namespace bdg::wish
