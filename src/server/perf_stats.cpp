// MIT License © 2026 Binary Dice Games
/// @file perf_stats.cpp
/// @brief perf_recorder implementation.
#include <server/perf_stats.hpp>

#include <algorithm>

namespace bdg::wish {

void perf_recorder::record_frame(std::chrono::nanoseconds cpu_time) {
  const double ms = std::chrono::duration<double, std::milli>(cpu_time).count();
  ++frames_;
  frame_ms_total_ += ms;
  frame_ms_max_ = std::max(frame_ms_max_, static_cast<float>(ms));
}

bool perf_recorder::due(std::chrono::steady_clock::time_point now) const {
  return !started_ || now - last_close_ >= std::chrono::seconds{1};
}

void perf_recorder::close_sample(std::chrono::steady_clock::time_point now, size_t clients, double cpu_seconds,
                                 uint64_t memory_bytes) {
  perf_sample s;
  const double elapsed = started_ ? std::chrono::duration<double>(now - last_close_).count() : 0.0;
  if (frames_ > 0) {
    s.frame_ms_avg = static_cast<float>(frame_ms_total_ / frames_);
    s.frame_ms_max = frame_ms_max_;
  }
  // Per second, even when the loop was late closing this one.
  s.frames = elapsed > 0.0 ? static_cast<float>(frames_ / elapsed) : static_cast<float>(frames_);
  if (cpu_seconds >= 0.0 && last_cpu_seconds_ >= 0.0 && elapsed > 0.0)
    s.cpu_percent = static_cast<float>(std::max(0.0, (cpu_seconds - last_cpu_seconds_) / elapsed * 100.0));
  s.memory_mb = static_cast<float>(static_cast<double>(memory_bytes) / (1024.0 * 1024.0));
  s.clients = static_cast<float>(clients);

  last_cpu_seconds_ = cpu_seconds;
  last_close_ = now;
  frames_ = 0;
  frame_ms_total_ = 0.0;
  frame_ms_max_ = 0.0f;
  if (!started_) {
    started_ = true; // the first call only sets the baseline
    return;
  }
  history_.push_back(s);
  if (history_.size() > kHistory)
    history_.pop_front();
}

} // namespace bdg::wish
