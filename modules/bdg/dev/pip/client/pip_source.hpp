// MIT License © 2026 Binary Dice Games
/// @file pip_source.hpp
/// @brief Client-side `pip` command orchestration for the pip module.
///
/// Runs every `pip` command through dev::tool_source (see
/// common/tool_source.hpp), parses its output (see pip_parsers.hpp), and pushes structured snapshots
/// to the server-side PipFrontend form via its update_* RMI methods. Also
/// reacts to the form's `*_requested` events (see server/pip.hpp) by running
/// the corresponding `pip` command and refreshing.
///
/// Mirrors helm_source: the Python environment a user wants to manage lives
/// on their own machine, reachable only from the client -- the server never
/// touches `pip` directly.
///
/// No command runs on the caller's thread: a `pip install` can take minutes,
/// and an event handler that blocks freezes the whole UI. Every method that
/// runs `pip` is called from a job of the shared common::command_worker, which
/// also shows the modal progress dialog (modules/bdg/common/command_worker.hpp).
#pragma once

#include "modules/bdg/dev/common/tool_source.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

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

using dev::process_result;

class pip_source : public dev::tool_source {
 public:
  /// @param interpreter  Python interpreter to run pip with (see
  ///                     resolve_interpreter()).
  /// @param worker       Runs the jobs; may be null for a source that is only
  ///                     used for probe_version().
  pip_source(
      std::shared_ptr<bison::rmi::proxy::dynamic> proxy, const std::string& interpreter,
      std::shared_ptr<common::command_worker> worker);

  /// @brief Runs `pip --version`. @return its output (`pip X from <path>
  /// (python Y)`), or `""` -- with @p error set -- when pip cannot be run.
  /// Does not touch the form, so it can gate the app before one exists.
  std::string probe_version(std::string& error) const;

  /// @brief Pushes @p version_text (plus the interpreter) to the form's
  /// environment line via set_environment.
  void push_environment(const std::string& version_text);

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

  /// @brief pip's `ERROR:` message from stderr (see error_summary()), or
  /// stdout when stderr is empty.
  std::string error_text(const process_result& r) const override;

  /// @brief Runs a mutating `pip` command, reports its result and calls
  /// refresh_all(). @return whether the command succeeded.
  bool run_and_refresh(const std::string& label, const std::vector<std::string>& args);

  /// @brief `pip install` + @p opts' flags.
  static std::vector<std::string> install_argv(const install_options& opts);

  std::string interpreter_;

  // Latest versions from the last on_outdated_requested(), by package name.
  std::map<std::string, std::string> latest_;
  bool outdated_checked_{false};
};

} // namespace bdg::wish::pip
