// MIT License © 2025 Binary Dice Games
/// @file bison_attributes.hpp
/// @brief Additional bison attribute types used by the wish UI element registry.
///
/// These extend the built-in attribute set from bison_object.hpp with
/// wish-specific metadata that has not yet been merged into the upstream
/// bison submodule.
#pragma once

#include "src/bison/bison_object.hpp"

namespace bdg::bison {

/**
 * @brief Inclusive [min, max] range hint for a numeric field.
 *
 * Advisory only — the runtime does not enforce the range.  Consumers such
 * as property editors or validation layers may use it to clamp input or
 * render an appropriate slider widget.
 */
class Range : public attribute {
 public:
  Range(double min, double max) : min_(min), max_(max) {}
  double min() const { return min_; }
  double max() const { return max_; }

 private:
  double min_;
  double max_;
};

/**
 * @brief Increment step hint for a numeric field.
 *
 * Advisory increment used by property editors and slider widgets.  Does not
 * affect serialisation or runtime value assignment.
 */
class Step : public attribute {
 public:
  explicit Step(double step) : step_(step) {}
  double step() const { return step_; }

 private:
  double step_;
};

}  // namespace bdg::bison
