// MIT License © 2026 Binary Dice Games
/// @file perf_stats.hpp
/// @brief Rolling per-second performance history of a wish server (frame
///        CPU time, frames drawn, process CPU and memory, clients), shown by
///        the server overlay's Performance page.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace bdg::wish {

/// @brief One second of the server's life.
struct perf_sample {
  float frame_ms_avg{0.0f}; ///< Average CPU time per drawn frame, ms (0: no frame).
  float frame_ms_max{0.0f}; ///< Slowest drawn frame, ms.
  float frames{0.0f}; ///< Frames drawn in that second.
  float cpu_percent{0.0f}; ///< Process CPU use; 100 = one core busy.
  float memory_mb{0.0f}; ///< Resident memory of the process, MiB.
  float clients{0.0f}; ///< Connected sessions at the end of the second.
};

/**
 * @brief Collects frame times as they happen and closes one sample per
 *        second, keeping the last `kHistory` of them.
 *
 * Render-thread only (no locking): the host renderer records frames and
 * ticks it, and the overlay reads it, all on the render thread. The clock
 * and the process figures are passed in, so tests drive it directly.
 */
class perf_recorder {
 public:
  /// @brief Samples kept: two minutes.
  static constexpr size_t kHistory = 120;

  /// @brief Adds one drawn frame that took @p cpu_time to build and submit.
  void record_frame(std::chrono::nanoseconds cpu_time);

  /// @brief Whether a sample is due at @p now (a second passed since the
  ///        last one); query the process figures only then.
  bool due(std::chrono::steady_clock::time_point now) const;

  /**
   * @brief Closes the current second into a sample.
   * @param now          The current time.
   * @param clients      Connected sessions.
   * @param cpu_seconds  Total CPU time the process used so far (user +
   *                     system); the sample holds the delta since the last
   *                     one. Negative: unknown (sample reads 0).
   * @param memory_bytes Resident memory now.
   */
  void close_sample(std::chrono::steady_clock::time_point now, size_t clients, double cpu_seconds,
                    uint64_t memory_bytes);

  /// @brief The samples, oldest first.
  const std::deque<perf_sample>& history() const {
    return history_;
  }

  /// @brief The newest sample, if any.
  std::optional<perf_sample> latest() const {
    if (history_.empty())
      return std::nullopt;
    return history_.back();
  }

 private:
  std::deque<perf_sample> history_;
  std::chrono::steady_clock::time_point last_close_{};
  bool started_{false};
  double last_cpu_seconds_{-1.0};
  // Current second.
  uint32_t frames_{0};
  double frame_ms_total_{0.0};
  float frame_ms_max_{0.0f};
};

} // namespace bdg::wish
