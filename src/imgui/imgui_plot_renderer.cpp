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

// Return the float vector stored under key k, or nullptr if absent/wrong type.
static const std::vector<float>* vec_field(const dynamic& obj, key_t k) {
  const auto* f = obj.findField(k);
  return (f && f->is<std::vector<float>>()) ? &f->as<std::vector<float>>() : nullptr;
}

// Append "##<wish_id>" so ImPlot/ImGui use a unique internal ID even when two
// elements share the same visible text.  Both ImGui and ImPlot hide everything
// after "##" from the display, so the rendered label / legend entry is unchanged.
static std::string with_id(const std::string& label, const ui_element& node) {
  return label + "##" + std::to_string(node.get_as<key_t>("__wish_id"_key, key_t{}).id);
}

// ── Plot container ─────────────────────────────────────────────────────────────

void render_plot(imgui_renderer& r, const ui_element& node, const context& s) {
  auto title = node.get_as<std::string>("title"_key, "##plot");
  float w = node.get_as<float>("width"_key, -1.0f);
  float h = node.get_as<float>("height"_key, 300.0f);
  int32_t flags = node.get_as<int32_t>("flags"_key, 0);
  auto x_label = node.get_as<std::string>("x_label"_key, "");
  auto y_label = node.get_as<std::string>("y_label"_key, "");
  int32_t xf = node.get_as<int32_t>("x_flags"_key, 0);
  int32_t yf = node.get_as<int32_t>("y_flags"_key, 0);
  float x_min = node.get_as<float>("x_min"_key, 0.0f);
  float x_max = node.get_as<float>("x_max"_key, 0.0f);
  float y_min = node.get_as<float>("y_min"_key, 0.0f);
  float y_max = node.get_as<float>("y_max"_key, 0.0f);

  auto iml = with_id(title, node);
  if (ImPlot::BeginPlot(iml.c_str(), ImVec2(w, h), ImPlotFlags(flags))) {
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
    render_children(r, node, s);
    ImPlot::EndPlot();
  }
}

// ── Line / scatter / stair / stem / shaded / digital ─────────────────────────

void render_plot_line(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  auto iml = with_id(label, node);
  if (count > 0)
    ImPlot::PlotLine(iml.c_str(), xs->data(), ys->data(), count);
}

void render_plot_scatter(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  auto iml = with_id(label, node);
  if (count > 0)
    ImPlot::PlotScatter(iml.c_str(), xs->data(), ys->data(), count);
}

void render_plot_stairs(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  auto iml = with_id(label, node);
  if (count > 0)
    ImPlot::PlotStairs(iml.c_str(), xs->data(), ys->data(), count);
}

void render_plot_stems(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  float ref = node.get_as<float>("ref"_key, 0.0f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  auto iml = with_id(label, node);
  if (count > 0)
    ImPlot::PlotStems(iml.c_str(), xs->data(), ys->data(), count, double(ref));
}

void render_plot_shaded(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  float ref = node.get_as<float>("ref"_key, 0.0f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  const auto* ys2 = vec_field(node, "ys2"_key);
  if (!xs || !ys)
    return;
  auto iml = with_id(label, node);

  if (ys2 && !ys2->empty()) {
    // Band between ys and ys2.
    int count = int(std::min({xs->size(), ys->size(), ys2->size()}));
    if (count > 0)
      ImPlot::PlotShaded(iml.c_str(), xs->data(), ys->data(), ys2->data(), count);
  } else {
    // Shade between ys and the ref baseline.
    int count = int(std::min(xs->size(), ys->size()));
    if (count > 0)
      ImPlot::PlotShaded(iml.c_str(), xs->data(), ys->data(), count, double(ref));
  }
}

void render_plot_digital(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  auto iml = with_id(label, node);
  if (count > 0)
    ImPlot::PlotDigital(iml.c_str(), xs->data(), ys->data(), count);
}

// ── Bar charts ────────────────────────────────────────────────────────────────

void render_plot_bars(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  float bar_size = node.get_as<float>("bar_size"_key, 0.67f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  auto iml = with_id(label, node);

  if (xs && !xs->empty() && ys && !ys->empty()) {
    int count = int(std::min(xs->size(), ys->size()));
    ImPlot::PlotBars(iml.c_str(), xs->data(), ys->data(), count, double(bar_size));
  } else if (ys && !ys->empty()) {
    ImPlot::PlotBars(iml.c_str(), ys->data(), int(ys->size()), double(bar_size));
  }
}

void render_plot_bars_h(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  float bar_size = node.get_as<float>("bar_size"_key, 0.67f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  auto iml = with_id(label, node);

  ImPlotSpec hspec;
  hspec.Flags = ImPlotBarsFlags_Horizontal;
  if (xs && !xs->empty() && ys && !ys->empty()) {
    int count = int(std::min(xs->size(), ys->size()));
    ImPlot::PlotBars(iml.c_str(), xs->data(), ys->data(), count, double(bar_size), hspec);
  } else if (ys && !ys->empty()) {
    ImPlot::PlotBars(iml.c_str(), ys->data(), int(ys->size()), double(bar_size), 0.0, hspec);
  }
}

// ── Histograms ────────────────────────────────────────────────────────────────

void render_plot_histogram(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t bins = node.get_as<int32_t>("bins"_key, -1); // -1 = Sturges
  bool cumul = node.get_as<bool>("cumulative"_key, false);
  bool density = node.get_as<bool>("density"_key, false);
  float rng_min = node.get_as<float>("range_min"_key, 0.0f);
  float rng_max = node.get_as<float>("range_max"_key, 0.0f);
  const auto* vals = vec_field(node, "values"_key);
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
  auto iml = with_id(label, node);
  ImPlot::PlotHistogram(iml.c_str(), vals->data(), int(vals->size()), bins, 1.0, range, hspec);
}

void render_plot_histogram2d(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t x_bins = node.get_as<int32_t>("x_bins"_key, -1);
  int32_t y_bins = node.get_as<int32_t>("y_bins"_key, -1);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys)
    return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count == 0)
    return;

  auto iml = with_id(label, node);
  ImPlot::PlotHistogram2D(iml.c_str(), xs->data(), ys->data(), count, x_bins, y_bins);
}

// ── Heatmap ───────────────────────────────────────────────────────────────────

void render_plot_heatmap(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  int32_t rows = node.get_as<int32_t>("rows"_key, 1);
  int32_t cols = node.get_as<int32_t>("cols"_key, 1);
  float scale_min = node.get_as<float>("scale_min"_key, 0.0f);
  float scale_max = node.get_as<float>("scale_max"_key, 1.0f);
  auto fmt = node.get_as<std::string>("format"_key, "%.1f");
  float x_min = node.get_as<float>("x_min"_key, 0.0f);
  float x_max = node.get_as<float>("x_max"_key, 1.0f);
  float y_min = node.get_as<float>("y_min"_key, 0.0f);
  float y_max = node.get_as<float>("y_max"_key, 1.0f);
  const auto* vals = vec_field(node, "values"_key);
  if (!vals || int(vals->size()) < rows * cols || rows <= 0 || cols <= 0)
    return;

  auto iml = with_id(label, node);
  ImPlot::PlotHeatmap(
      iml.c_str(),
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

void render_plot_pie_chart(imgui_renderer&, const ui_element& node, const context&) {
  auto labels_str = node.get_as<std::string>("labels"_key, "");
  float cx = node.get_as<float>("x"_key, 0.5f);
  float cy = node.get_as<float>("y"_key, 0.5f);
  float radius = node.get_as<float>("radius"_key, 0.4f);
  bool normalize = node.get_as<bool>("normalize"_key, false);
  auto fmt = node.get_as<std::string>("label_fmt"_key, "%.1f%%");
  float angle0 = node.get_as<float>("angle0"_key, 90.0f);
  const auto* vals = vec_field(node, "values"_key);
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

void render_plot_text(imgui_renderer&, const ui_element& node, const context&) {
  auto text = node.get_as<std::string>("text"_key, "");
  float x = node.get_as<float>("x"_key, 0.0f);
  float y = node.get_as<float>("y"_key, 0.0f);
  float offset_x = node.get_as<float>("offset_x"_key, 0.0f);
  float offset_y = node.get_as<float>("offset_y"_key, 0.0f);
  if (!text.empty())
    ImPlot::PlotText(text.c_str(), double(x), double(y), ImVec2(offset_x, offset_y));
}

void render_plot_inf_lines(imgui_renderer&, const ui_element& node, const context&) {
  auto label = node.get_as<std::string>("label"_key, "");
  bool horiz = node.get_as<bool>("horizontal"_key, false);
  const auto* vals = vec_field(node, "values"_key);
  if (!vals || vals->empty())
    return;

  ImPlotSpec ispec;
  ispec.Flags = horiz ? ImPlotInfLinesFlags_Horizontal : 0;
  auto iml = with_id(label, node);
  ImPlot::PlotInfLines(iml.c_str(), vals->data(), int(vals->size()), ispec);
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
