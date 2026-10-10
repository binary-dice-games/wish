// MIT License © 2026 Binary Dice Games
/// @file console_panel.hpp
/// @brief The Console window shared by the bdg/dev tool forms: a FIFO-capped
///        trace of every command the module's client ran.
///
/// Fed by the form's `append_command_log {command, exit_code, ok, output}`
/// RMI method, which the client's `common::tool_source::run_logged()` calls
/// after each command. One row per command (`# / Command / Exit / Output`),
/// green on success and red on failure, newest last with the table
/// following it. Right-clicking a row offers "Copy Entry" and
/// "Clear <title>". At most kMaxRows rows are kept; the oldest is evicted,
/// with every object it registered.
#pragma once

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish::common {

class tool_form;

/// @brief Settings for console_panel::build().
struct console_options {
  /// Window title; the context menu's clear item reads "Clear <title>".
  std::string title{"Console"};
  /// ImGui id of the table, e.g. `"##helm_console_table"`.
  std::string table_id{"##console_table"};
  int32_t width{940};
  /// Initial width of the Command column.
  int32_t command_width{360};
  bool closable{true};
  /// Called after the user cleared the console from the context menu.
  std::function<void()> on_cleared;
  /// Called after the user copied a row with "Copy Entry" (the copy itself
  /// happens client-side, via the item's `copy_text`).
  std::function<void()> on_copied;
};

class console_panel {
 public:
  static constexpr size_t kMaxRows = 500;

  /// @brief Builds the window and registers it as @p owner's top-level root
  /// @p root_key (conventionally `internal_root_key_ + "_console"`). The
  /// table is at `<root_key>.vbox.table`. Call from the owner's on_init().
  void build(tool_form& owner, const std::string& root_key, console_options options = {});

  /// @brief Appends one row; evicts the oldest past kMaxRows.
  void append(const std::string& command, int32_t exit_code, bool ok, const std::string& output);

  /// @brief The `append_command_log` RMI method body: append() from
  /// `{command, exit_code, ok, output}`.
  void append_from(const bison::dynamic& args);

  /// @brief Removes every row and restarts the `#` sequence at 1.
  void clear();

  /// @brief The window's id (match it against `closed` events).
  bison::key_t window_id() const {
    return window_id_;
  }
  const std::string& root_key() const {
    return root_key_;
  }
  size_t size() const {
    return rows_.size();
  }

 private:
  struct row_entry {
    size_t child_key;
    std::vector<bison::key_t> object_ids;
  };

  tool_form* owner_{nullptr};
  console_options options_;
  std::string root_key_;
  bison::key_t window_id_;
  ui_element_ptr table_;
  size_t seq_{0};
  size_t next_child_key_{0};
  std::deque<row_entry> rows_; // oldest first
};

} // namespace bdg::wish::common
