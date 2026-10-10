// MIT License © 2026 Binary Dice Games
/// @file rolling_plot.cpp
/// @brief Implementation of common::rolling_plot.
#include "modules/bdg/common/server/rolling_plot.hpp"

#include "modules/bdg/common/server/tool_form.hpp"
#include "modules/bdg/common/server/ui_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace bdg::wish::common {

using namespace bison;

void rolling_plot::configure_axes(const ui_element_ptr& plot, y_axis y) {
  // X is a rolling sample index: hide its numeric labels
  // (ImPlotAxisFlags_NoTickLabels = 1 << 3) and keep it continuously
  // auto-fitted to the collected history (ImPlotAxisFlags_AutoFit = 1 << 11)
  // so the trace always fills the frame from the first sample.
  constexpr int32_t kNoTickLabels = 1 << 3;
  constexpr int32_t kAutoFit = 1 << 11;
  constexpr int32_t kLegendSouth = 1 << 1; // ImPlotLocation_South
  constexpr int32_t kLegendOutside = 1 << 4; // ImPlotLegendFlags_Outside
  plot["x_flags"_key] = kNoTickLabels | kAutoFit;
  if (y == y_axis::percent) {
    plot["y_min"_key] = 0.0f;
    plot["y_max"_key] = 100.0f;
  } else {
    plot["y_flags"_key] = kAutoFit;
  }
  // Keep the legend below the frame as a single vertical column: ImPlot
  // shrinks the trace area to fit the *whole* column (no clipping), so every
  // line stays labelled. A horizontal legend would be clamped to the plot
  // width and crop the tail entries.
  plot["legend_location"_key] = kLegendSouth;
  plot["legend_flags"_key] = kLegendOutside;
}

void rolling_plot::init(
    tool_form& owner,
    const ui_element_ptr& plot,
    const std::string& aggregate_label,
    bool average) {
  owner_ = &owner;
  plot_ = plot;
  average_ = average;
  auto* children_p = plot->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  ui_element_ptr line = ui_element_ptr::create("wish"_key, "PlotLine"_key);
  line["label"_key] = aggregate_label;
  line["order"_key] = static_cast<int32_t>(0);
  owner.assign_id(line);
  aggregate_.el = line;
  aggregate_.id = wish_id_of(line);
  aggregate_.child_key = 0;
  next_child_key_ = 0;
  (**children_p)[static_cast<size_t>(0)] = dynamic_ptr{line};
  plot->refresh_children_order();
}

void rolling_plot::push_history(std::vector<float>& history, float value) {
  history.push_back(value);
  if (history.size() > kMaxHistory)
    history.erase(history.begin());
}

void rolling_plot::push(const std::map<std::string, float>& values) {
  if (!owner_ || !plot_ || !aggregate_.el)
    return;
  auto* children_p = plot_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  float agg = 0.0f;
  for (const auto& [name, v] : values)
    agg += v;
  if (average_ && !values.empty())
    agg /= static_cast<float>(values.size());
  push_history(aggregate_.hist, agg);

  std::vector<std::pair<std::string, float>> ranked(values.begin(), values.end());
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::set<std::string> keep;
  for (size_t i = 0; i < ranked.size() && i < kMaxSeries; ++i)
    keep.insert(ranked[i].first);

  for (auto it = series_.begin(); it != series_.end();) {
    if (keep.count(it->first)) {
      ++it;
      continue;
    }
    children->erase(it->second.child_key);
    owner_->erase_objects({it->second.id});
    it = series_.erase(it);
  }

  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (const auto& name : keep) {
    if (series_.count(name))
      continue;
    ui_element_ptr line = ui_element_ptr::create("wish"_key, "PlotLine"_key);
    line["label"_key] = name;
    line["order"_key] = static_cast<int32_t>(1);
    owner_->assign_id(line);
    series s;
    s.el = line;
    s.id = wish_id_of(line);
    s.child_key = ++next_child_key_;
    s.hist.assign(aggregate_.hist.size() - 1, nan);
    (*children)[s.child_key] = dynamic_ptr{line};
    series_.emplace(name, std::move(s));
  }

  for (const auto& name : keep)
    push_history(series_.at(name).hist, values.at(name));

  const size_t count = aggregate_.hist.size();
  if (xs_.size() != count) {
    xs_.resize(count);
    for (size_t i = 0; i < count; ++i)
      xs_[i] = static_cast<float>(i);
  }

  aggregate_.el["xs"_key] = xs_;
  aggregate_.el["ys"_key] = aggregate_.hist;
  for (auto& [name, s] : series_) {
    s.el["xs"_key] = xs_;
    s.el["ys"_key] = s.hist;
  }
  plot_->refresh_children_order();
}

std::vector<std::string> rolling_plot::series_names() const {
  std::vector<std::string> names;
  for (const auto& [name, s] : series_)
    names.push_back(name);
  return names;
}

} // namespace bdg::wish::common
