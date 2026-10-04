// MIT License © 2026 Binary Dice Games
/// @file command_worker.hpp
/// @brief Background job execution with a modal progress dialog, shared by
///        the module clients (the bdg/dev tools, mc, ...).
///
/// A module's client reacts to its form's `*_requested` events by running a
/// command-line tool or moving files. Doing that inside the event handler
/// blocks the whole UI until it finishes (see the zip module's client for
/// the same rule), so every handler instead post()s a job to one
/// command_worker:
///
///   - one worker thread runs the jobs in order, one at a time (the tools
///     must not overlap, and the module's own state stays single-threaded);
///   - a step that has been running for more than kProgressDelay opens a
///     modal `ProgressBox` (src/ui/forms/progress_box.hpp) showing its
///     caption, a progress bar, its output as it arrives and a Cancel
///     button -- quick steps (a refresh, a small file) never flash a dialog.
///     run() drives a child process (indeterminate bar, the tool's output);
///     run_task() drives any other work that reports its own progress
///     through a task_progress (a determinate bar, e.g. bytes transferred);
///   - once open, the dialog stays for the rest of that busy period and
///     closes when the queue is empty -- unless fail() recorded an error,
///     which it then shows until the user closes it.
///
/// Header-only (module client sources are compiled into several targets; see
/// cmake/WishModules.cmake), and not a module itself: this directory sits
/// outside every `modules/<org>/<collection>/` tree, so no collection picks
/// it up.
#pragma once

#include "modules/common/process_hooks.hpp"

#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/client/proxy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish::common {

/// @brief `"<tool> <arg> <arg> ..."` -- the caption of one command.
inline std::string command_text(const std::string& tool, const std::vector<std::string>& args) {
  std::string text = tool;
  for (auto& a : args)
    text += ' ' + a;
  return text;
}

class command_worker;

/// @brief Progress handle a run_task() step reports through. Worker thread
/// only; valid for the duration of the run_task() call.
class task_progress {
 public:
  /// @brief Recaptions the dialog (a new caption also starts a new section
  /// in its log), e.g. the file a batch has moved on to.
  void set_caption(const std::string& caption) {
    caption_ = caption;
    flush(true);
  }
  /// @brief Reports @p done of @p total units: a determinate bar with
  /// @p detail (e.g. `"1.2 MB / 3.4 MB"`) overlaid; an empty @p detail shows
  /// the percentage. Throttled -- cheap to call per chunk.
  void report(std::uint64_t done, std::uint64_t total, const std::string& detail = {}) {
    fraction_ = total == 0 ? 1.0f : static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
    detail_ = detail;
    flush(false);
  }
  /// @brief Appends @p text to the dialog's log.
  void line(std::string text) {
    lines_.push_back(std::move(text));
    flush(true);
  }
  /// @brief True once the user pressed Cancel (or cancel() was called):
  /// the step should stop at its next opportunity.
  bool cancelled() const;

 private:
  friend class command_worker;
  task_progress(command_worker& worker, std::string caption)
      : worker_(worker), caption_(std::move(caption)), started_(std::chrono::steady_clock::now()) {}
  /// @brief Pushes the current state to the dialog -- at most every
  /// kMinPushInterval unless @p force -- opening it once kProgressDelay has
  /// passed.
  void flush(bool force);

  static constexpr float kMinPushInterval = 0.1f; // seconds

  command_worker& worker_;
  std::string caption_;
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point last_push_{};
  float fraction_{-1.0f}; // < 0: indeterminate
  std::string detail_;
  std::vector<std::string> lines_; // not yet pushed
};

class command_worker : public std::enable_shared_from_this<command_worker> {
 public:
  /// @param host   The app host the progress dialog is instantiated through.
  ///               Must outlive the worker's last job (shutdown() is called
  ///               from the form's "closed" handler, before the app ends).
  /// @param title  The progress dialog's window title, e.g. `"Running helm"`.
  command_worker(wish_app_host& host, std::string title) : host_(host), title_(std::move(title)) {}

  /// @brief Starts the worker thread. It keeps this object alive until
  /// shutdown(), so the instance must be owned by a `std::shared_ptr`.
  void start() {
    // Detached, holding a reference to this object: shutdown() is called
    // from an event handler, which must not block joining a thread that may
    // itself be waiting on an RMI reply.
    std::thread([self = shared_from_this()] { self->work(); }).detach();
  }

  /// @brief Queues @p job; jobs run one at a time, in order. Safe from any
  /// thread.
  void post(std::function<void()> job) {
    queue_.wlock()->jobs.push_back(std::move(job));
    queue_.notify_one();
  }

  /// @brief Subscribes @p handler to @p proxy's @p event so that it runs as
  /// a job on the worker thread instead of inside the event dispatch.
  template <typename Proxy, typename Handler>
  void on(Proxy& proxy, bison::key_t event, Handler handler) {
    proxy.onEvent(event, [weak = weak_from_this(), handler = std::move(handler)](bison::dynamic payload) {
      if (auto self = weak.lock())
        self->post([handler, payload = std::move(payload)] { handler(payload); });
    });
  }

  /// @brief Stops the command that is running (if any) and drops the queued
  /// jobs. Safe from any thread; returns immediately.
  void cancel() {
    cancel_ = true;
    queue_.wlock()->jobs.clear();
  }

  /// @brief cancel() + ends the worker thread. Call when the form closes.
  void shutdown() {
    cancel_ = true;
    {
      auto q = queue_.wlock();
      q->jobs.clear();
      q->stop = true;
    }
    queue_.notify_one();
  }

  /// @brief Records that the current job failed, with the message to show.
  /// If the progress dialog is open when the worker goes idle it stays open
  /// displaying @p message. Safe from any thread.
  ///
  /// @param always_show  Open the dialog for this failure even if the command
  ///                     was too quick to have opened it. For the outcome of
  ///                     something the user explicitly asked for (an install),
  ///                     whose error is often longer than a status line; not
  ///                     for a background refresh, which would keep popping
  ///                     a modal while a service is unreachable.
  void fail(const std::string& message, bool always_show = false) {
    auto f = failure_.wlock();
    f->message = message;
    f->always_show = f->always_show || always_show;
  }

  /// @brief Runs one command with progress reporting. Worker thread only.
  ///
  /// @param command  Caption, e.g. command_text("helm", args).
  /// @param run_fn   `process_result(const run_hooks*)` -- must pass the
  ///                 hooks on to run_process() (see process.hpp;
  ///                 tool_source::run_logged() does this).
  /// @param show_output  False when the command's output is data rather
  ///                 than progress (an HTTP response body, say): the dialog
  ///                 then shows only the command and the bar.
  /// @return @p run_fn's result; when the user cancelled the command, its
  ///         `stderr_text` is replaced by `"cancelled"`.
  template <typename RunFn>
  auto run(const std::string& command, RunFn&& run_fn, bool show_output = true) {
    const auto started = std::chrono::steady_clock::now();
    std::string pending;            // output not yet terminated by a newline
    std::vector<std::string> lines; // complete lines not yet pushed
    auto flush = [&] {
      const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - started;
      if (!shown_ && elapsed.count() < kProgressDelay)
        return;
      shown_ = true;
      push(command, elapsed.count(), lines);
      lines.clear();
    };
    flush();

    run_hooks hooks;
    hooks.on_output = [&](const std::string& chunk) {
      if (!show_output)
        return;
      pending += chunk;
      size_t eol;
      while ((eol = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, eol);
        pending.erase(0, eol + 1);
        // A tool's progress is often redrawn in place with '\r'.
        if (const size_t cr = line.find_last_of('\r'); cr != std::string::npos)
          line = cr + 1 < line.size() ? line.substr(cr + 1) : line.substr(0, cr);
        const size_t text = line.find_first_not_of(" \t");
        // JSON snapshots (list / inspect output) are data, not progress.
        if (text == std::string::npos || line[text] == '[' || line[text] == '{')
          continue;
        if (line.size() > kMaxLineLength)
          line = line.substr(0, kMaxLineLength) + "...";
        lines.push_back(std::move(line));
        // Before the dialog opens, only the tail is worth keeping.
        if (lines.size() > kMaxPendingLines)
          lines.erase(lines.begin());
      }
    };
    hooks.on_tick = [&] {
      flush();
      return !cancel_.load();
    };

    auto result = run_fn(static_cast<const run_hooks*>(&hooks));
    if (shown_)
      flush(); // the lines since the last tick.
    // Consume the request: the rest of the job (the refresh after a
    // cancelled action, say) must still run.
    if (cancel_.exchange(false) && !result.ok())
      result.stderr_text = "cancelled";
    return result;
  }

  /// @brief Runs one step that is not a child process (a file transfer, a
  /// batch of them) with progress reporting. Worker thread only.
  ///
  /// @param caption  The dialog's caption until the step changes it.
  /// @param fn       Called as `fn(task_progress&)`; reports progress
  ///                 through it and polls `cancelled()` to stop early.
  /// @return @p fn's result. A pending cancel request is consumed when @p fn
  ///         returns, so the rest of the job (a refresh) still runs.
  template <typename Fn>
  auto run_task(const std::string& caption, Fn&& fn) {
    task_progress progress(*this, caption);
    progress.flush(true);
    struct consume_cancel {
      command_worker& w;
      ~consume_cancel() { w.cancel_ = false; }
    } guard{*this};
    return fn(progress);
  }

 private:
  friend class task_progress;

  // Long enough that a refresh never flashes a dialog, short enough that a
  // real wait gets one almost at once.
  static constexpr float kProgressDelay = 0.4f; // seconds
  static constexpr size_t kMaxLineLength = 400;
  static constexpr size_t kMaxPendingLines = 200;

  void work() {
    while (true) {
      std::function<void()> job;
      queue_.wait([&](work_queue& q) {
        if (q.stop)
          return true;
        if (q.jobs.empty())
          return false;
        job = std::move(q.jobs.front());
        q.jobs.pop_front();
        return true;
      });
      if (!job)
        return; // stopped.
      cancel_ = false;
      try {
        job();
      } catch (const std::exception&) {
        // A job that throws (the form is gone mid-call) must not end the app.
      }
      if (queue_.rlock()->jobs.empty()) {
        const failure failed = *failure_.rlock();
        if (!shown_ && failed.always_show && !failed.message.empty()) {
          push({}, 0.0f, {}); // open it just to show the error.
          shown_ = true;
        }
        if (shown_)
          finish(failed.message);
        shown_ = false;
        *failure_.wlock() = failure{};
      }
    }
  }

  /// @brief `update` on the progress dialog, instantiating it on first use.
  /// @p fraction >= 0 draws a determinate bar with @p detail overlaid; a
  /// negative one the indeterminate animation driven by @p phase.
  void push(
      const std::string& command, float phase, const std::vector<std::string>& lines, float fraction = -1.0f,
      const std::string& detail = {}) {
    bison::dynamic arr;
    size_t i = 0;
    for (auto& line : lines)
      arr[i++] = line;

    bison::dynamic args;
    args[bison::key_t{"command"}] = command;
    args[bison::key_t{"phase"}] = phase;
    args[bison::key_t{"lines"}] = bison::dynamic_ptr{std::make_shared<bison::dynamic>(std::move(arr))};
    if (fraction >= 0.0f) {
      args[bison::key_t{"fraction"}] = std::min(fraction, 1.0f);
      args[bison::key_t{"detail"}] = detail;
    }
    try {
      if (!box_) {
        bison::dynamic params;
        params[bison::key_t{"title"}] = title_;
        box_ = std::make_shared<bison::rmi::proxy::dynamic>(
            host_.instantiate(bison::key_t{"wish"}, bison::key_t{"ProgressBox"}, std::move(params)).get());
        // Weak: the proxy (owned by this object) must not keep it alive.
        box_->onEvent(bison::key_t{"cancel_requested"}, [weak = weak_from_this()](bison::dynamic) {
          if (auto self = weak.lock())
            self->cancel();
        });
      }
      box_->call(bison::key_t{"update"}, std::move(args)).get();
    } catch (const std::exception&) {
      // Best-effort: without a dialog the command still runs.
    }
  }

  /// @brief `finish` on the progress dialog (an empty @p error closes it).
  void finish(const std::string& error) {
    if (!box_)
      return;
    bison::dynamic args;
    args[bison::key_t{"error"}] = error;
    try {
      box_->call(bison::key_t{"finish"}, std::move(args)).get();
    } catch (const std::exception&) {
    }
  }

  struct work_queue {
    std::deque<std::function<void()>> jobs;
    bool stop{false};
  };

  wish_app_host& host_;
  std::string title_;
  bison::synchronized<work_queue> queue_;
  std::atomic<bool> cancel_{false}; // the running command should be stopped
  struct failure {
    std::string message;
    bool always_show{false};
  };
  bison::synchronized<failure> failure_;

  // Worker thread only.
  std::shared_ptr<bison::rmi::proxy::dynamic> box_; // the ProgressBox, once needed
  bool shown_{false};                               // the dialog is up for this busy period
};

inline bool task_progress::cancelled() const {
  return worker_.cancel_.load();
}

inline void task_progress::flush(bool force) {
  const auto now = std::chrono::steady_clock::now();
  const std::chrono::duration<float> elapsed = now - started_;
  if (!worker_.shown_ && elapsed.count() < command_worker::kProgressDelay) {
    // Before the dialog opens, only the tail of the log is worth keeping.
    if (lines_.size() > command_worker::kMaxPendingLines)
      lines_.erase(lines_.begin(), lines_.end() - command_worker::kMaxPendingLines);
    return;
  }
  const std::chrono::duration<float> since_push = now - last_push_;
  if (worker_.shown_ && !force && since_push.count() < kMinPushInterval)
    return;
  worker_.shown_ = true;
  last_push_ = now;
  worker_.push(caption_, elapsed.count(), lines_, fraction_, detail_);
  lines_.clear();
}

} // namespace bdg::wish::common
