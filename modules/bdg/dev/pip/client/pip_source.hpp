// MIT License © 2026 Binary Dice Games
/// @file pip_source.hpp
/// @brief Client-side `pip` command orchestration for the pip module.
///
/// Owns the proxy, runs every `pip` command via pip_process::run_pip_cli(),
/// parses its output (see pip_parsers.hpp), and pushes structured snapshots
/// to the server-side PipFrontend form via its update_* RMI methods. Also
/// reacts to the form's `*_requested` events (see server/pip.hpp) by running
/// the corresponding `pip` command and refreshing.
///
/// Mirrors helm_source: the Python environment a user wants to manage lives
/// on their own machine, reachable only from the client -- the server never
/// touches `pip` directly.
///
/// Unlike helm_source, no command runs on the caller's thread: a `pip
/// install` can take minutes, and an event handler that blocks freezes the
/// whole UI. Event handlers post() a job; one worker thread runs the jobs in
/// order (pip commands must not overlap) and reports live progress to the
/// form's set_progress RMI method.
#pragma once

#include "pip_process.hpp"
#include "src/bison/bison.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/client/proxy.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::pip {

/// @brief Resolves the app's optional positional argument to the interpreter
/// pip is run with (`<interpreter> -m pip`).
///
/// @param arg  `""` -> `python3`, or `python` when `python3` cannot be run
///             (so an activated virtualenv, first on `PATH`, is picked up); a
///             directory -> that virtualenv's `bin/python` (or
///             `Scripts/python.exe`); anything else -> used as the interpreter
///             path / name itself.
/// @return The interpreter, or `""` when @p arg is a directory holding no
///         interpreter.
std::string resolve_interpreter(const std::string& arg);

class pip_source : public std::enable_shared_from_this<pip_source> {
 public:
  /// @param interpreter  Python interpreter to run pip with (see
  ///                     resolve_interpreter()).
  pip_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, const std::string& interpreter);

  /// @brief Runs `pip --version`. @return its output (`pip X from <path>
  /// (python Y)`), or `""` -- with @p error set -- when pip cannot be run.
  /// Does not touch the form, so it can gate the app before one exists.
  std::string probe_version(std::string& error) const;

  /// @brief Pushes @p version_text (plus the interpreter) to the form's
  /// environment line via set_environment.
  void push_environment(const std::string& version_text);

  // ── worker thread ───────────────────────────────────────────────────────

  /// @brief Starts the worker thread. It keeps this object alive until
  /// shutdown(), so the instance must be owned by a `std::shared_ptr`.
  void start();

  /// @brief Queues @p job for the worker thread; jobs run one at a time, in
  /// order. Safe from any thread. Every method below that runs `pip` must be
  /// called from a job, never directly from an event handler.
  void post(std::function<void()> job);

  /// @brief Stops the command that is running (if any) and drops the queued
  /// jobs. Safe from any thread; returns immediately.
  void cancel();

  /// @brief cancel() + ends the worker thread. Call when the form closes.
  void shutdown();

  /// @brief Pushes the installed-packages snapshot. Called once on startup,
  /// on "refresh_requested", and after every mutating action below.
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────

  /// @brief `pip list --outdated`: remembers each outdated package's latest
  /// version (shown in the Latest column from then on) and re-pushes the
  /// packages snapshot. Queries the package index, so it is only run on
  /// request.
  void on_outdated_requested();

  /// @brief Options shared by the install commands.
  struct install_options {
    bool upgrade{false}; // --upgrade
    bool user{false};    // --user
    bool pre{false};     // --pre
  };

  /// @brief `pip install [options] <requirement>...` -- @p spec is split on
  /// whitespace into one requirement per word.
  void on_install_requested(const std::string& spec, const install_options& opts);

  /// @brief `pip install [options] -r <path>` (a requirements file on the
  /// client machine).
  void on_requirements_requested(const std::string& path, const install_options& opts);

  /// @brief `upgrade` -> `pip install --upgrade <name>`; `reinstall` ->
  /// `pip install --force-reinstall --no-deps <name>`; `uninstall` ->
  /// `pip uninstall -y <name>`.
  void on_package_action(const std::string& name, const std::string& action);

  /// @brief Pushes one `pip index versions <name> [--pre]` snapshot to
  /// update_versions.
  void on_versions_requested(const std::string& name, bool pre);

  /// @brief Pushes the verbatim output of one read-only command to
  /// update_details. @p kind selects it: `show` / `files` run `pip show
  /// [-f] <name>`; `freeze` / `check` run `pip freeze` / `pip check`
  /// (@p name unused).
  void on_details_requested(const std::string& kind, const std::string& name);

 private:
  void push_packages();

  /// @brief Runs `pip <args>` via run_pip_cli() and pushes a trace row to the
  /// Console window (PipFrontend's append_command_log RMI method). Every
  /// `pip` invocation after startup goes through here so the Console window
  /// is a complete trace. Mirrors helm_source::run_logged().
  process_result run_logged(const std::vector<std::string>& args);

  /// @brief Reports a command outcome via PipFrontend's command_result RMI
  /// method (tagged with @p scope so the right window's status label is
  /// written). @return false when the form is gone.
  bool report(const std::string& label, const std::string& scope, bool ok, const std::string& output);

  /// @brief Runs a mutating `pip` command, reports its result and calls
  /// refresh_all(). @return whether the command succeeded.
  bool run_and_refresh(const std::string& label, const std::vector<std::string>& args);

  /// @brief `pip install` + @p opts' flags.
  static std::vector<std::string> install_argv(const install_options& opts);

  /// @brief Drives the form's modal progress dialog (set_progress RMI
  /// method): @p active opens / closes it, @p command is the command running
  /// now, @p phase animates the bar and @p lines are the output lines since
  /// the previous call.
  void push_progress(bool active, const std::string& command, float phase, const std::vector<std::string>& lines);

  void work(); // worker thread body

  struct work_queue {
    std::deque<std::function<void()>> jobs;
    bool stop{false};
  };
  bison::synchronized<work_queue> queue_;
  std::atomic<bool> cancel_{false}; // the running command should be stopped

  // Whether the progress dialog is up. It only opens once a command has run
  // for kProgressDelay, so quick ones (a refresh) do not flash a modal; from
  // then on it stays until the queue is empty. Worker thread only.
  bool progress_shown_{false};

  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  std::string interpreter_;
  std::vector<std::string> launcher_; // interpreter + `-m pip` + global options

  // Latest versions from the last on_outdated_requested(), by package name.
  // Worker thread only.
  std::map<std::string, std::string> latest_;
  bool outdated_checked_{false};
};

} // namespace bdg::wish::pip
