// MIT License © 2025 Binary Dice Games
/// @file imgui_plot_renderer.cpp
/// @brief ImPlot render functions for wish plot elements.
///
/// Each function maps one wish element class to the corresponding ImPlot call.
/// All functions share the signature:
///   void(imgui_renderer&, const ui_element&, session&)
/// matching the render_fn typedef in imgui_renderer.cpp.
#include "imgui_plot_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <wish/renderer.hpp>

#include <implot.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── Inline field helpers (mirrors imgui_renderer.cpp) ─────────────────────────

static std::string str_field(
    const dynamic& obj, key_t k, const char* dflt = "") {
  const auto* f = obj.findField(k);
  return (f && f->is<std::string>()) ? f->as<std::string>() : dflt;
}

static float float_field(const dynamic& obj, key_t k, float dflt = 0.0f) {
  const auto* f = obj.findField(k);
  return (f && f->is<float>()) ? f->as<float>() : dflt;
}

static int32_t int_field(const dynamic& obj, key_t k, int32_t dflt = 0) {
  const auto* f = obj.findField(k);
  return (f && f->is<int32_t>()) ? f->as<int32_t>() : dflt;
}

static bool bool_field(const dynamic& obj, key_t k, bool dflt = false) {
  const auto* f = obj.findField(k);
  return (f && f->is<bool>()) ? f->as<bool>() : dflt;
}

// Return the float vector stored under key k, or nullptr if absent/wrong type.
static const std::vector<float>* vec_field(const dynamic& obj, key_t k) {
  const auto* f = obj.findField(k);
  return (f && f->is<std::vector<float>>()) ? &f->as<std::vector<float>>() : nullptr;
}

// ── Plot container ─────────────────────────────────────────────────────────────

void render_plot(imgui_renderer& r, const ui_element& node, session& s) {
  auto    title   = str_field(node, "title"_key, "##plot");
  float   w       = float_field(node, "width"_key, -1.0f);
  float   h       = float_field(node, "height"_key, 300.0f);
  int32_t flags   = int_field(node, "flags"_key, 0);
  auto    x_label = str_field(node, "x_label"_key, "");
  auto    y_label = str_field(node, "y_label"_key, "");
  int32_t xf      = int_field(node, "x_flags"_key, 0);
  int32_t yf      = int_field(node, "y_flags"_key, 0);

  if (ImPlot::BeginPlot(title.c_str(), ImVec2(w, h), ImPlotFlags(flags))) {
    if (!x_label.empty() || !y_label.empty() || xf != 0 || yf != 0) {
      ImPlot::SetupAxes(
          x_label.empty() ? nullptr : x_label.c_str(),
          y_label.empty() ? nullptr : y_label.c_str(),
          ImPlotAxisFlags(xf),
          ImPlotAxisFlags(yf));
    }
    render_children(r, node, s);
    ImPlot::EndPlot();
  }
}

// ── Line / scatter / stair / stem / shaded / digital ─────────────────────────

void render_plot_line(imgui_renderer&, const ui_element& node, session&) {
  auto label = str_field(node, "label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys) return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotLine(label.c_str(), xs->data(), ys->data(), count);
}

void render_plot_scatter(imgui_renderer&, const ui_element& node, session&) {
  auto label = str_field(node, "label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys) return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotScatter(label.c_str(), xs->data(), ys->data(), count);
}

void render_plot_stairs(imgui_renderer&, const ui_element& node, session&) {
  auto label = str_field(node, "label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys) return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotStairs(label.c_str(), xs->data(), ys->data(), count);
}

void render_plot_stems(imgui_renderer&, const ui_element& node, session&) {
  auto  label = str_field(node, "label"_key, "");
  float ref   = float_field(node, "ref"_key, 0.0f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys) return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotStems(label.c_str(), xs->data(), ys->data(), count, double(ref));
}

void render_plot_shaded(imgui_renderer&, const ui_element& node, session&) {
  auto  label = str_field(node, "label"_key, "");
  float ref   = float_field(node, "ref"_key, 0.0f);
  const auto* xs  = vec_field(node, "xs"_key);
  const auto* ys  = vec_field(node, "ys"_key);
  const auto* ys2 = vec_field(node, "ys2"_key);
  if (!xs || !ys) return;

  if (ys2 && !ys2->empty()) {
    // Band between ys and ys2.
    int count = int(std::min({xs->size(), ys->size(), ys2->size()}));
    if (count > 0)
      ImPlot::PlotShaded(label.c_str(),
                         xs->data(), ys->data(), ys2->data(), count);
  } else {
    // Shade between ys and the ref baseline.
    int count = int(std::min(xs->size(), ys->size()));
    if (count > 0)
      ImPlot::PlotShaded(label.c_str(),
                         xs->data(), ys->data(), count, double(ref));
  }
}

void render_plot_digital(imgui_renderer&, const ui_element& node, session&) {
  auto label = str_field(node, "label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys) return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count > 0)
    ImPlot::PlotDigital(label.c_str(), xs->data(), ys->data(), count);
}

// ── Bar charts ────────────────────────────────────────────────────────────────

void render_plot_bars(imgui_renderer&, const ui_element& node, session&) {
  auto  label    = str_field(node, "label"_key, "");
  float bar_size = float_field(node, "bar_size"_key, 0.67f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);

  if (xs && !xs->empty() && ys && !ys->empty()) {
    int count = int(std::min(xs->size(), ys->size()));
    ImPlot::PlotBars(label.c_str(), xs->data(), ys->data(), count, double(bar_size));
  } else if (ys && !ys->empty()) {
    ImPlot::PlotBars(label.c_str(), ys->data(), int(ys->size()), double(bar_size));
  }
}

void render_plot_bars_h(imgui_renderer&, const ui_element& node, session&) {
  auto  label    = str_field(node, "label"_key, "");
  float bar_size = float_field(node, "bar_size"_key, 0.67f);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);

  ImPlotSpec hspec;
  hspec.Flags = ImPlotBarsFlags_Horizontal;
  if (xs && !xs->empty() && ys && !ys->empty()) {
    int count = int(std::min(xs->size(), ys->size()));
    ImPlot::PlotBars(label.c_str(), xs->data(), ys->data(), count,
                     double(bar_size), hspec);
  } else if (ys && !ys->empty()) {
    ImPlot::PlotBars(label.c_str(), ys->data(), int(ys->size()),
                     double(bar_size), 0.0, hspec);
  }
}

// ── Histograms ────────────────────────────────────────────────────────────────

void render_plot_histogram(imgui_renderer&, const ui_element& node, session&) {
  auto    label    = str_field(node, "label"_key, "");
  int32_t bins     = int_field(node, "bins"_key, -1);  // -1 = Sturges
  bool    cumul    = bool_field(node, "cumulative"_key, false);
  bool    density  = bool_field(node, "density"_key, false);
  float   rng_min  = float_field(node, "range_min"_key, 0.0f);
  float   rng_max  = float_field(node, "range_max"_key, 0.0f);
  const auto* vals = vec_field(node, "values"_key);
  if (!vals || vals->empty()) return;

  ImPlotHistogramFlags hflags = ImPlotHistogramFlags_None;
  if (cumul)   hflags |= ImPlotHistogramFlags_Cumulative;
  if (density) hflags |= ImPlotHistogramFlags_Density;

  // Pass an empty range when min==max (auto-range).
  ImPlotRange range;
  if (rng_min != rng_max)
    range = ImPlotRange(double(rng_min), double(rng_max));

  ImPlotSpec hspec;
  hspec.Flags = hflags;
  ImPlot::PlotHistogram(label.c_str(), vals->data(), int(vals->size()),
                        bins, 1.0, range, hspec);
}

void render_plot_histogram2d(imgui_renderer&, const ui_element& node, session&) {
  auto    label  = str_field(node, "label"_key, "");
  int32_t x_bins = int_field(node, "x_bins"_key, -1);
  int32_t y_bins = int_field(node, "y_bins"_key, -1);
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  if (!xs || !ys) return;
  int count = int(std::min(xs->size(), ys->size()));
  if (count == 0) return;

  ImPlot::PlotHistogram2D(label.c_str(), xs->data(), ys->data(), count,
                          x_bins, y_bins);
}

// ── Heatmap ───────────────────────────────────────────────────────────────────

void render_plot_heatmap(imgui_renderer&, const ui_element& node, session&) {
  auto    label     = str_field(node, "label"_key, "");
  int32_t rows      = int_field(node, "rows"_key, 1);
  int32_t cols      = int_field(node, "cols"_key, 1);
  float   scale_min = float_field(node, "scale_min"_key, 0.0f);
  float   scale_max = float_field(node, "scale_max"_key, 1.0f);
  auto    fmt       = str_field(node, "format"_key, "%.1f");
  float   x_min     = float_field(node, "x_min"_key, 0.0f);
  float   x_max     = float_field(node, "x_max"_key, 1.0f);
  float   y_min     = float_field(node, "y_min"_key, 0.0f);
  float   y_max     = float_field(node, "y_max"_key, 1.0f);
  const auto* vals  = vec_field(node, "values"_key);
  if (!vals || int(vals->size()) < rows * cols || rows <= 0 || cols <= 0) return;

  ImPlot::PlotHeatmap(
      label.c_str(), vals->data(), rows, cols,
      double(scale_min), double(scale_max),
      fmt.empty() ? nullptr : fmt.c_str(),
      ImPlotPoint(double(x_min), double(y_min)),
      ImPlotPoint(double(x_max), double(y_max)));
}

// ── Pie chart ─────────────────────────────────────────────────────────────────

void render_plot_pie_chart(imgui_renderer&, const ui_element& node, session&) {
  auto  labels_str = str_field(node, "labels"_key, "");
  float cx         = float_field(node, "x"_key, 0.5f);
  float cy         = float_field(node, "y"_key, 0.5f);
  float radius     = float_field(node, "radius"_key, 0.4f);
  bool  normalize  = bool_field(node, "normalize"_key, false);
  auto  fmt        = str_field(node, "label_fmt"_key, "%.1f%%");
  float angle0     = float_field(node, "angle0"_key, 90.0f);
  const auto* vals = vec_field(node, "values"_key);
  if (!vals || vals->empty()) return;

  // Parse newline-separated labels.
  std::vector<std::string> labels;
  {
    std::string::size_type pos = 0, end;
    while ((end = labels_str.find('\n', pos)) != std::string::npos) {
      labels.push_back(labels_str.substr(pos, end - pos));
      pos = end + 1;
    }
    if (!labels_str.empty()) labels.push_back(labels_str.substr(pos));
  }

  int count = int(std::min(vals->size(), labels.size()));
  if (count == 0) return;

  std::vector<const char*> label_ptrs;
  label_ptrs.reserve(size_t(count));
  for (int i = 0; i < count; ++i) label_ptrs.push_back(labels[size_t(i)].c_str());

  ImPlotSpec pspec;
  pspec.Flags = normalize ? ImPlotPieChartFlags_Normalize : 0;
  ImPlot::PlotPieChart(label_ptrs.data(), vals->data(), count,
                       double(cx), double(cy), double(radius),
                       fmt.c_str(), double(angle0), pspec);
}

// ── Annotations ───────────────────────────────────────────────────────────────

void render_plot_text(imgui_renderer&, const ui_element& node, session&) {
  auto  text     = str_field(node, "text"_key, "");
  float x        = float_field(node, "x"_key, 0.0f);
  float y        = float_field(node, "y"_key, 0.0f);
  float offset_x = float_field(node, "offset_x"_key, 0.0f);
  float offset_y = float_field(node, "offset_y"_key, 0.0f);
  if (!text.empty())
    ImPlot::PlotText(text.c_str(), double(x), double(y),
                     ImVec2(offset_x, offset_y));
}

void render_plot_inf_lines(imgui_renderer&, const ui_element& node, session&) {
  auto        label = str_field(node, "label"_key, "");
  bool        horiz = bool_field(node, "horizontal"_key, false);
  const auto* vals  = vec_field(node, "values"_key);
  if (!vals || vals->empty()) return;

  ImPlotSpec ispec;
  ispec.Flags = horiz ? ImPlotInfLinesFlags_Horizontal : 0;
  ImPlot::PlotInfLines(label.c_str(), vals->data(), int(vals->size()), ispec);
}

}  // namespace bdg::wish

#endif  // WISH_IMGUI_ENABLED
