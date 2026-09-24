// MIT License © 2026 Binary Dice Games
/// @file sq_chart_render.hpp
/// @brief Software rasterizer that draws a chart of the sq Chart window to a
///        PNG image, independent of any renderer or window system.
///
/// The Chart window shows the chart with live `Plot` widgets, which only exist
/// inside a renderer; this renders the same data (see chart_spec) into an
/// RGB buffer so it can be saved as a file. It is intentionally simple --
/// axis ticks, grid, legend, and the series shapes -- not a pixel-exact copy
/// of ImPlot.
#pragma once

#include <string>
#include <vector>

namespace bdg::wish::sq_chart {

/// @brief Chart types, in the order of the Chart window's type combo.
enum class kind { line, scatter, stairs, stems, area, bars, bars_h, histogram, pie };

/// @brief One data series. Meaning of the arrays depends on the chart kind:
/// point kinds use (xs, ys); bars_h uses xs = lengths, ys = positions;
/// histogram uses ys as the samples; pie uses ys as the slice sizes.
struct series {
  std::string label;
  std::vector<float> xs;
  std::vector<float> ys;
};

/// @brief Everything needed to draw one chart.
struct chart_spec {
  kind type{kind::line};
  std::string x_label;
  std::string y_label;
  std::vector<series> data;
  std::vector<std::string> pie_labels; ///< one per pie slice
  int bins{-1};       ///< histogram: >0 explicit, -1 Sturges, -2 Scott, -3 Rice, -4 sqrt
  float bar_size{0.67f}; ///< bars/bars_h: bar thickness in data units
  std::string font_ttf;  ///< TrueType file bytes for all text; empty/invalid = small built-in font
  float font_size{20.0f}; ///< text height in output pixels (used with font_ttf)
};

/// @brief Draws @p spec into a @p width x @p height PNG.
/// @return The PNG file bytes, or "" if the spec has no drawable data or the
///         size is not positive.
std::string render_png(const chart_spec& spec, int width, int height);

/// @brief Number of histogram bins for @p n samples spanning @p range with
/// standard deviation @p sd, following @p rule (see chart_spec::bins). >= 1.
int histogram_bin_count(int rule, size_t n, float range, float sd);

} // namespace bdg::wish::sq_chart
