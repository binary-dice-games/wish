// MIT License © 2026 Binary Dice Games
/// @file tool_source.hpp
/// @brief Base class of the bdg/dev modules' client-side `<tool>_source`
///        classes: runs the tool, traces every run to the form's Console
///        window and reports outcomes back to the form.
///
/// Every dev module's client owns one `<tool>_source` that runs the tool's
/// commands and pushes the parsed results to its server-side form through
/// RMI calls. The plumbing around that is the same in every module and lives
/// here:
///
///   - run() -- `<launcher> <args>` via run_process(), unlogged;
///   - run_logged() -- run() on the command_worker (progress dialog, Cancel)
///     plus a trace row to the form's `append_command_log` method;
///   - report() -- a command's outcome to the form's `command_result`
///     method, recording a failure with the worker so the progress dialog
///     shows it;
///   - call() -- a best-effort RMI call that tolerates a torn-down form;
///   - push_rows() -- one tab-separated listing pushed as an array.
///
/// The form contract this relies on: `append_command_log {command,
/// exit_code, ok, output}` and `command_result {command, [scope,] ok,
/// output}`. Header-only.
#pragma once

#include "modules/common/command_worker.hpp"
#include "modules/bdg/dev/common/process.hpp"
#include "modules/bdg/dev/common/text.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish::dev {

/// @brief Optional settings for tool_source::run_logged().
struct run_options {
  /// Progress-dialog caption and Console-window command text; empty uses
  /// tool_source::caption(). Set it to mask secrets an argument carries.
  std::string caption;
  /// Written to the child's stdin (see process_options::stdin_text).
  std::string stdin_text;
  /// False when the command's output is data rather than progress (query
  /// rows, a response body): the dialog then shows only the caption.
  bool show_output{true};
};

class tool_source {
 public:
  virtual ~tool_source() = default;

  /// @brief Runs `<launcher> <args>`, blocking, without logging or progress
  /// reporting. Safe from any thread (a background poll uses it directly so
  /// that it never floods the Console window).
  virtual process_result run(const std::vector<std::string>& args, process_options options = {}) const {
    return run_process(concat_args(launcher_, args), options);
  }

 protected:
  /// @param proxy     The module's server-side form. May be null for a
  ///                  source used only to run() a probe before the form
  ///                  exists.
  /// @param worker    The command_worker every logged command runs on. May
  ///                  be null under the same condition.
  /// @param tool      Name shown in captions and the Console window, e.g.
  ///                  `"helm"`. Empty when every argv names its own program.
  /// @param launcher  The program and leading arguments every command starts
  ///                  with, e.g. `{"python3", "-m", "pip"}`. Empty when every
  ///                  argv names its own program.
  tool_source(
      std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker, std::string tool,
      std::vector<std::string> launcher)
      : proxy_(std::move(proxy)), worker_(std::move(worker)), tool_(std::move(tool)), launcher_(std::move(launcher)) {}

  /// @brief Same, with @p tool itself as the launcher.
  tool_source(
      std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker, std::string tool)
      : tool_source(std::move(proxy), std::move(worker), tool, std::vector<std::string>{tool}) {}

  /// @brief Runs `<launcher> <args>` on the worker (behind the progress
  /// dialog when it takes long; cancellable) and pushes a trace row to the
  /// Console window, so that window is a complete trace of every command.
  /// Worker thread only.
  process_result run_logged(const std::vector<std::string>& args, const run_options& options = {}) {
    const std::string command = options.caption.empty() ? caption(args) : options.caption;
    auto r = worker_->run(
        command,
        [&](const common::run_hooks* hooks) {
          process_options po;
          po.stdin_text = options.stdin_text;
          po.hooks = hooks;
          return run(args, std::move(po));
        },
        options.show_output);
    log_command(command, r);
    return r;
  }

  /// @brief `"<tool> <args>"` on one line (a listing's format argument holds
  /// TABs and newlines), at most kMaxCaption characters.
  std::string caption(const std::vector<std::string>& args) const {
    std::string text = tool_;
    for (auto& a : args)
      text += (text.empty() ? "" : " ") + a;
    return one_line(text, kMaxCaption);
  }

  /// @brief Pushes one Console-window row for the finished command @p r:
  /// @p command plus a one-line preview of log_output(). Best-effort.
  void log_command(const std::string& command, const process_result& r) const {
    bison::dynamic log;
    log[bison::key_t{"command"}] = command;
    log[bison::key_t{"exit_code"}] = r.exit_code;
    log[bison::key_t{"ok"}] = r.ok();
    log[bison::key_t{"output"}] = one_line(log_output(r), kMaxOutputPreview);
    call(bison::key_t{"append_command_log"}, std::move(log));
  }

  /// @brief What a failed command had to say, as shown to the user. Defaults
  /// to error_output() without its trailing line break; a module whose tool
  /// prints chatter before its error line narrows it down.
  virtual std::string error_text(const process_result& r) const {
    return trim_eol(error_output(r));
  }

  /// @brief The output a Console-window row previews: stdout on success,
  /// error_text() on failure.
  virtual std::string log_output(const process_result& r) const {
    return r.ok() ? r.stdout_text : error_text(r);
  }

  /// @brief Reports a command's outcome via the form's `command_result`
  /// method. @p scope (omitted when empty) tells the form which window's
  /// status label to write. A failure is also recorded with the worker, so
  /// an open progress dialog shows it (see command_worker::fail()).
  ///
  /// @param always_show  Show a failure in the dialog even when the command
  ///                     was too quick to open it -- for something the user
  ///                     explicitly asked for.
  /// @return false when the form is gone.
  bool report(
      const std::string& label, const std::string& scope, bool ok, const std::string& output,
      bool always_show = false) {
    bison::dynamic args;
    args[bison::key_t{"command"}] = label;
    if (!scope.empty())
      args[bison::key_t{"scope"}] = scope;
    args[bison::key_t{"ok"}] = ok;
    args[bison::key_t{"output"}] = ok ? std::string{} : output;
    if (!ok)
      worker_->fail(label + " failed: " + output, always_show);
    return call(bison::key_t{"command_result"}, std::move(args));
  }

  /// @brief Calls @p method on the form, waiting for it to finish.
  /// @return false when the call failed -- the form is torn down (it was
  ///         closed mid-command), which callers treat as "stop here".
  bool call(bison::key_t method, bison::dynamic args) const {
    if (!proxy_)
      return false;
    try {
      proxy_->call(method, std::move(args)).get();
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  /// @brief Builds one array entry from a row's columns.
  using row_filler = std::function<void(bison::dynamic& entry, const std::vector<std::string>& columns)>;

  /// @brief Runs one listing command whose output has a row per line and
  /// TAB-separated columns (a Go template / jsonpath format), calls @p fill
  /// once per row with its columns (padded to @p ncols), then calls
  /// @p method with the collected array under @p array_key. A failed command
  /// pushes an empty array. Worker thread only.
  void push_rows(
      const std::vector<std::string>& args, size_t ncols, bison::key_t array_key, bison::key_t method,
      const row_filler& fill) {
    auto r = run_logged(args);

    bison::dynamic arr;
    size_t i = 0;
    if (r.ok()) {
      std::istringstream iss(r.stdout_text);
      std::string line;
      while (std::getline(iss, line)) {
        line = trim_eol(line);
        if (line.empty())
          continue;
        auto cols = split(line, '\t');
        cols.resize(ncols);
        auto e = std::make_shared<bison::dynamic>();
        fill(*e, cols);
        arr[i++] = bison::dynamic_ptr{e};
      }
    }

    bison::dynamic args_out;
    args_out[array_key] = bison::dynamic_ptr{std::make_shared<bison::dynamic>(std::move(arr))};
    call(method, std::move(args_out));
  }

  static constexpr size_t kMaxCaption = 300;
  static constexpr size_t kMaxOutputPreview = 200;

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  // Runs every logged command off the UI thread, behind a modal progress
  // dialog when it takes long (see command_worker.hpp). Every method that
  // calls run_logged() must be called from one of its jobs.
  std::shared_ptr<common::command_worker> worker_;

 private:
  std::string tool_;
  std::vector<std::string> launcher_;
};

} // namespace bdg::wish::dev
