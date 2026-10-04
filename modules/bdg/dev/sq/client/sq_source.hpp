// MIT License © 2026 Binary Dice Games
/// @file sq_source.hpp
/// @brief Client-side `sq` command orchestration for the sq module.
///
/// Runs every `sq` command through dev::tool_source (see
/// common/tool_source.hpp), parses the output, and pushes structured snapshots to the server-side
/// SqFrontend form via its update_* RMI methods. Reacts to the form's
/// `*_requested` events (see server/sq.hpp) by running the matching `sq`
/// command and pushing the outcome.
///
/// The "active database" is this session's own state (every command passes
/// `--src <handle>`); it never runs `sq src`, so it does not change the
/// active source of the user's `sq` configuration.
#pragma once

#include "modules/bdg/dev/common/tool_source.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish::sq {

using dev::process_result;

class sq_source : public dev::tool_source {
 public:
  sq_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<dev::command_worker> worker);

  /// @brief Pushes the driver list (once), the connections, and the active
  /// database's schema. Called on startup and by "refresh_requested".
  void refresh_all();

  // ── *_requested event reactions ─────────────────────────────────────────
  void on_activate(const std::string& handle);
  void on_ping(const std::string& handle);
  void on_remove(const std::string& handle);
  /// @param password Fed to `sq add -p` on stdin (never on the argv).
  void on_add(
      const std::string& handle, const std::string& location, const std::string& driver, const std::string& password);
  /// @brief Runs @p sql (after the read-only check) against the active
  /// database and pushes update_result with at most @p max_rows rows.
  void on_query(const std::string& sql, int32_t max_rows);
  /// @brief Stores @p name (a path relative to the session's server-side
  /// sandbox) with content @p data; throws on failure.
  using upload_fn = std::function<void(const std::string& name, const std::string& data)>;
  /// @brief Re-runs the last successful query in full as CSV (into a local
  /// temp file) and hands it to @p upload as @p path, so the file ends up on
  /// the server. The form has already validated @p path and overwrite.
  void on_export(const std::string& path, const upload_fn& upload);

 private:
  void push_drivers();
  void push_connections();
  void push_schema();
  void push_result_error(const std::string& message);
  /// @brief SqFrontend's own command_result contract: `{scope, ok,
  /// message}` (replaces tool_source::report()).
  void report(const std::string& scope, bool ok, const std::string& message);

  /// @brief Runs `sq <args>` (feeding @p stdin_text, when non-empty) via
  /// tool_source::run_logged(), with connection locations masked in the
  /// caption and the output kept out of the progress dialog.
  process_result run_logged(const std::vector<std::string>& args, const std::string& stdin_text = {});

  std::string error_text(const process_result& r) const override;
  std::string log_output(const process_result& r) const override;

  bool drivers_pushed_{false};
  std::string active_;      ///< active handle ("" = none)
  std::string last_sql_;    ///< last successfully run query, for export
  std::string last_handle_; ///< the handle it ran against
};

} // namespace bdg::wish::sq
