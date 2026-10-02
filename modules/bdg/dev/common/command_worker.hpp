// MIT License © 2026 Binary Dice Games
/// @file command_worker.hpp
/// @brief Background command execution with a modal progress dialog, shared
///        by the bdg/dev modules' clients.
///
/// A dev module's client reacts to its form's `*_requested` events by running
/// a command-line tool. Doing that inside the event handler blocks the whole
/// UI until the tool exits (see the zip module's client for the same rule),
/// so every handler instead post()s a job to one command_worker:
///
///   - one worker thread runs the jobs in order, one at a time (the tools
///     must not overlap, and the module's own state stays single-threaded);
///   - a command that has been running for more than kProgressDelay opens a
///     modal `ProgressBox` (src/ui/forms/progress_box.hpp) showing the
///     command, an indeterminate progress bar, the tool's output as it
///     arrives and a Cancel button -- quick commands (a refresh) never flash
///     a dialog;
///   - once open, the dialog stays for the rest of that busy period and
///     closes when the queue is empty -- unless fail() recorded an error,
///     which it then shows until the user closes it.
///
/// Header-only (module client sources are compiled into several targets; see
/// cmake/WishModules.cmake), and not a module itself: this directory has no
/// server/ or client/ subdirectory.
#pragma once

#include "modules/bdg/dev/common/process_hooks.hpp"

#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish::dev {

/// @brief `"<tool> <arg> <arg> ..."` -- the caption of one command.
inline std::string command_text(const std::string& tool, const std::vector<std::string>& args) {
  std::string text = tool;
  for (auto& a : args)
    text += ' ' + a;
  return text;
}

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
  ///                 hooks on to the module's `run_<tool>_cli()`.
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

 private:
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
  void push(const std::string& command, float phase, const std::vector<std::string>& lines) {
    bison::dynamic arr;
    size_t i = 0;
    for (auto& line : lines)
      arr[i++] = line;

    bison::dynamic args;
    args[bison::key_t{"command"}] = command;
    args[bison::key_t{"phase"}] = phase;
    args[bison::key_t{"lines"}] = bison::dynamic_ptr{std::make_shared<bison::dynamic>(std::move(arr))};
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

} // namespace bdg::wish::dev
