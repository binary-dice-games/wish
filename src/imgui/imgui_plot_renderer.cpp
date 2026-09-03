// MIT License © 2025 Binary Dice Games
/// @file imgui_plot_renderer.cpp
/// @brief ImPlot render functions for wish plot elements.
///
/// Each function maps one wish element class to the corresponding ImPlot call.
/// All functions share the signature:
///   void(imgui_renderer&, const ui_element&, const context&)
/// matching the render_fn typedef in imgui_renderer.cpp.
#include "imgui_plot_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <server/renderer.hpp>

#include <implot.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// No with_id()/label-suffix helper needed here: every render_plot_* function
// is invoked through imgui_renderer::render_node() (imgui_renderer.cpp),
// which already wraps the call in ImGui::PushID(stable_id(node)) -- unlike
// a top-level ImGui::Begin() window, ImPlot::BeginPlot() and the PlotXxx
// series calls all consult the current ID stack, so that push is enough to
// disambiguate same-labeled plots/series without touching their labels.

// ── Plot container ─────────────────────────────────────────────────────────────

void render_plot(imgui_renderer& r, const ui_element& node_base, const context& s) {
  const auto& node = static_cast<const ui_plot&>(node_base);
  const std::string& title = node.title_ref();
  float w = node.width();
  float h = node.height();
  int32_t flags = node.flags();
  const std::string& x_label = node.x_label_ref();
  const std::string& y_label = node.y_label_ref();
  int32_t xf = node.x_flags();
  int32_t yf = node.y_flags();
  float x_min = node.x_min();
  float x_max = node.x_max();
  float y_min = node.y_min();
  float y_max = node.y_max();

  if (ImPlot::BeginPlot(title.c_str(), ImVec2(w, h), ImPlotFlags(flags))) {
    if (!x_label.empty() || !y_label.empty() || xf != 0 || yf != 0) {
      ImPlot::SetupAxes(
          x_label.empty() ? nullptr : x_label.c_str(),
          y_label.empty() ? nullptr : y_label.c_str(),
          ImPlotAxisFlags(xf),
          ImPlotAxisFlags(yf));
    }
    // A fixed range (locked every frame via ImPlotCond_Always) is opt-in per
    // axis by setting x_min != x_max / y_min != y_max; otherwise ImPlot's
    // normal auto-fit behavior applies unchanged.
    if (x_min != x_max)
      ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Always);
    if (y_min != y_max)
      ImPlot::SetupAxisLimits(ImAxis_Y1, y_min, y_max, ImPlotCond_Always);
    // Legend placement is opt-in: only touch it when the element sets a
    // location (>= 0) or any legend flag. Otherwise ImPlot keeps its default
    // (top-left, inside the plot).
    int32_t legend_location = node.legend_location();
    int32_t legend_flags = node.legend_flags();
    if (legend_location >= 0 || legend_flags != 0) {
      ImPlot::SetupLegend(
          ImPlotLocation(legend_location < 0 ? ImPlotLocation_NorthWest : legend_location),
          ImPlotLegendFlags(legend_flags));
    }
    render_children(r, node, s);
    ImPlot::EndPlot();
  }
}

// ── Line / scatter / stair / stem / shaded / digital ─────────────────────────

void render_plot_line(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_xy_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotLine(label.c_str(), xs->data(), ys->data(), count);
}

void render_plot_scatter(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_xy_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotScatter(label.c_str(), xs->data(), ys->data(), count);
}

void render_plot_stairs(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_xy_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotStairs(label.c_str(), xs->data(), ys->data(), count);
}

void render_plot_stems(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_stems&>(node_base);
  const std::string& label = node.label_ref();
  float ref = node.ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotStems(label.c_str(), xs->data(), ys->data(), count, double(ref));
}

void render_plot_shaded(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_shaded&>(node_base);
  const std::string& label = node.label_ref();
  float ref = node.ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* ys2 = node.ys2();
  if (!xs || !ys)
    return;

  if (ys2 && !ys2->empty()) {
    // Band between ys and ys2.
    int count = int(std::min({xs->size(), ys->size(), ys2->size()}));
    if (count > 0)
      ImPlot::PlotShaded(label.c_str(), xs->data(), ys->data(), ys2->data(), count);
  } else {
    // Shade between ys and the ref baseline.
    int count = int(std::min(xs->size(), ys->size()));
    if (count > 0)
      ImPlot::PlotShaded(label.c_str(), xs->data(), ys->data(), count, double(ref));
  }
}

void render_plot_digital(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_xy_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotDigital(label.c_str(), xs->data(), ys->data(), count);
}

// ── Bar charts ────────────────────────────────────────────────────────────────

void render_plot_bars(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_bars&>(node_base);
  const std::string& label = node.label_ref();
  float bar_size = node.bar_size();
  const auto* xs = node.xs();
  const auto* ys = node.ys();

  if (xs && !xs->empty() && ys && !ys->empty()) {
    int count = int(std::min(xs->size(), ys->size()));
    ImPlot::PlotBars(label.c_str(), xs->data(), ys->data(), count, double(bar_size));
  } else if (ys && !ys->empty()) {
    ImPlot::PlotBars(label.c_str(), ys->data(), int(ys->size()), double(bar_size));
  }
}

void render_plot_bars_h(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_bars&>(node_base);
  const std::string& label = node.label_ref();
  float bar_size = node.bar_size();
  const auto* xs = node.xs();
  const auto* ys = node.ys();

  ImPlotSpec hspec;
  hspec.Flags = ImPlotBarsFlags_Horizontal;
  if (xs && !xs->empty() && ys && !ys->empty()) {
    int count = int(std::min(xs->size(), ys->size()));
    ImPlot::PlotBars(label.c_str(), xs->data(), ys->data(), count, double(bar_size), hspec);
  } else if (ys && !ys->empty()) {
    ImPlot::PlotBars(label.c_str(), ys->data(), int(ys->size()), double(bar_size), 0.0, hspec);
  }
}

// ── Histograms ────────────────────────────────────────────────────────────────

void render_plot_histogram(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_histogram&>(node_base);
  const std::string& label = node.label_ref();
  int32_t bins = node.bins(); // -1 = Sturges
  bool cumul = node.cumulative();
  bool density = node.density();
  float rng_min = node.range_min();
  float rng_max = node.range_max();
  const auto* vals = node.values();
  if (!vals || vals->empty())
    return;

  ImPlotHistogramFlags hflags = ImPlotHistogramFlags_None;
  if (cumul)
    hflags |= ImPlotHistogramFlags_Cumulative;
  if (density)
    hflags |= ImPlotHistogramFlags_Density;

  // Pass an empty range when min==max (auto-range).
  ImPlotRange range;
  if (rng_min != rng_max)
    range = ImPlotRange(double(rng_min), double(rng_max));

  ImPlotSpec hspec;
  hspec.Flags = hflags;
  ImPlot::PlotHistogram(label.c_str(), vals->data(), int(vals->size()), bins, 1.0, range, hspec);
}

void render_plot_histogram2d(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_histogram2d&>(node_base);
  const std::string& label = node.label_ref();
  int32_t x_bins = node.x_bins();
  int32_t y_bins = node.y_bins();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count == 0)
    return;

  ImPlot::PlotHistogram2D(label.c_str(), xs->data(), ys->data(), count, x_bins, y_bins);
}

// ── Heatmap ───────────────────────────────────────────────────────────────────

void render_plot_heatmap(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_heatmap&>(node_base);
  const std::string& label = node.label_ref();
  int32_t rows = node.rows();
  int32_t cols = node.cols();
  float scale_min = node.scale_min();
  float scale_max = node.scale_max();
  const std::string& fmt = node.format_ref();
  float x_min = node.x_min();
  float x_max = node.x_max();
  float y_min = node.y_min();
  float y_max = node.y_max();
  const auto* vals = node.values();
  if (!vals || int(vals->size()) < rows * cols || rows <= 0 || cols <= 0)
    return;

  ImPlot::PlotHeatmap(
      label.c_str(),
      vals->data(),
      rows,
      cols,
      double(scale_min),
      double(scale_max),
      fmt.empty() ? nullptr : fmt.c_str(),
      ImPlotPoint(double(x_min), double(y_min)),
      ImPlotPoint(double(x_max), double(y_max)));
}

// ── Pie chart ─────────────────────────────────────────────────────────────────

void render_plot_pie_chart(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_pie_chart&>(node_base);
  const std::string& labels_str = node.labels_ref();
  float cx = node.x();
  float cy = node.y();
  float radius = node.radius();
  bool normalize = node.normalize();
  const std::string& fmt = node.label_fmt_ref();
  float angle0 = node.angle0();
  const auto* vals = node.values();
  if (!vals || vals->empty())
    return;

  // Parse newline-separated labels.
  std::vector<std::string> labels;
  {
    std::string::size_type pos = 0, end;
    while ((end = labels_str.find('\n', pos)) != std::string::npos) {
      labels.push_back(labels_str.substr(pos, end - pos));
      pos = end + 1;
    }
    if (!labels_str.empty())
      labels.push_back(labels_str.substr(pos));
  }

  int count = int(std::min(vals->size(), labels.size()));
  if (count == 0)
    return;

  std::vector<const char*> label_ptrs;
  label_ptrs.reserve(size_t(count));
  for (int i = 0; i < count; ++i)
    label_ptrs.push_back(labels[size_t(i)].c_str());

  ImPlotSpec pspec;
  pspec.Flags = normalize ? ImPlotPieChartFlags_Normalize : 0;
  ImPlot::PlotPieChart(
      label_ptrs.data(),
      vals->data(),
      count,
      double(cx),
      double(cy),
      double(radius),
      fmt.c_str(),
      double(angle0),
      pspec);
}

// ── Annotations ───────────────────────────────────────────────────────────────

void render_plot_text(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_text&>(node_base);
  const std::string& text = node.text_ref();
  float x = node.x();
  float y = node.y();
  float offset_x = node.offset_x();
  float offset_y = node.offset_y();
  if (!text.empty())
    ImPlot::PlotText(text.c_str(), double(x), double(y), ImVec2(offset_x, offset_y));
}

void render_plot_inf_lines(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot_inf_lines&>(node_base);
  const std::string& label = node.label_ref();
  bool horiz = node.horizontal();
  const auto* vals = node.values();
  if (!vals || vals->empty())
    return;

  ImPlotSpec ispec;
  ispec.Flags = horiz ? ImPlotInfLinesFlags_Horizontal : 0;
  ImPlot::PlotInfLines(label.c_str(), vals->data(), int(vals->size()), ispec);
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
