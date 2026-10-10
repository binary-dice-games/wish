// MIT License © 2026 Binary Dice Games
/// @file rolling_plot.hpp
/// @brief A live `Plot` with one rolling line per named series plus an
///        aggregate line -- docker's Stats and kubectl's Top graphs.
///
/// Fed one sample per push() (a `{series name -> value}` map, e.g. CPU % per
/// container). The aggregate line ("Total" -- the sum, or the mean with
/// `average`) is always present; the kMaxSeries largest series of the
/// current sample get a line of their own, added and removed as series come
/// and go. Every line keeps the last kMaxHistory samples against a shared
/// rolling-index X axis; a line added late starts with NaN gaps so it lines
/// up with the aggregate.
#pragma once

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace bdg::wish::common {

class tool_form;

class rolling_plot {
 public:
  static constexpr size_t kMaxHistory = 120; ///< samples kept per line
  static constexpr size_t kMaxSeries = 15; ///< per-series lines

  /// @brief Y-axis behaviour for configure_axes().
  enum class y_axis { auto_fit, percent };

  /// @brief Sets up the X axis as an unlabelled, auto-fitted sample index,
  /// the Y axis per @p y, and the legend as one column below the frame (so
  /// every line stays labelled however many there are).
  static void configure_axes(const ui_element_ptr& plot, y_axis y);

  /// @brief Takes over @p plot (a `Plot` with an empty children map) and
  /// creates its aggregate line labelled @p aggregate_label.
  /// @param average  The aggregate is the mean of the sample's values
  ///                 rather than their sum.
  void init(tool_form& owner, const ui_element_ptr& plot, const std::string& aggregate_label, bool average = false);

  /// @brief Appends one sample: @p values maps series name -> value.
  void push(const std::map<std::string, float>& values);

  /// @brief Names of the series that currently have their own line.
  std::vector<std::string> series_names() const;

  /// @brief The aggregate line's history, oldest first.
  const std::vector<float>& aggregate_history() const {
    return aggregate_.hist;
  }

 private:
  struct series {
    ui_element_ptr el;
    std::vector<float> hist;
    bison::key_t id;
    size_t child_key{0};
  };

  static void push_history(std::vector<float>& history, float value);

  tool_form* owner_{nullptr};
  ui_element_ptr plot_;
  bool average_{false};
  series aggregate_; // child 0, never removed
  std::map<std::string, series> series_;
  size_t next_child_key_{0};
  std::vector<float> xs_; // 0, 1, ... history size - 1
};

} // namespace bdg::wish::common
