// MIT License © 2026 Binary Dice Games
/// @file progress_box.hpp
/// @brief Server-side ProgressBox form class.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Modal progress dialog for a long-running operation the client is
/// performing (typically a command-line tool it shells out to).
///
/// Built-in reusable counterpart to `MessageBox` for "this will take a
/// while": a true input-blocking modal (Window.modal = true) showing the
/// operation in progress, an indeterminate progress bar, a scrolling log of
/// the operation's output, and a Cancel button. Being modal, it also keeps
/// the user from starting another action until the operation is over.
///
/// The dialog is open as soon as the form is instantiated. The client then
/// drives it with two RMI methods:
///   - `update` -- `{ command (string), phase (float), lines (array of
///     strings) }`: @c command captions the dialog (a change of caption also
///     starts a new `$ command` section in the log), @c phase is the number
///     of seconds the operation has been running (it animates the bar) and
///     @c lines are appended to the log. Reopens the dialog if it was closed
///     by an earlier `finish`.
///   - `finish` -- `{ error (string) }`: the operation is over. An empty
///     @c error closes the dialog. A non-empty one keeps it open, showing the
///     error and the log, with the button relabelled Close -- unless the user
///     had pressed Cancel, in which case it closes regardless.
///
/// Emitted events:
///   - `"cancel_requested"` -- no payload; the Cancel button. The dialog
///     stays open (captioned "Cancelling ...") until `finish` is called.
///   - `"closed"` -- no payload; the dialog is gone from the screen (after a
///     successful `finish`, or the user's Close after a failed one). The form
///     object itself stays valid: the next `update` reopens it, so one
///     instance can serve an application's whole lifetime.
///
/// Field: `title` (string, default `"Working"`), settable at
/// `instantiate(..., params)` time.
class progress_box : public cloneable_ui_element<progress_box, form> {
 public:
  explicit progress_box(bison::dynamic&& base);

  /// @brief Called from the `__construct` prototype method -- applies an
  /// `instantiate(..., params)`-time `title` (see
  /// message_box::on_construct()).
  void on_construct(const bison::dynamic& params);

  /// @brief RMI method `update` -- see the class doc comment.
  bison::dynamic do_update(const bison::dynamic& args);
  /// @brief RMI method `finish` -- see the class doc comment.
  bison::dynamic do_finish(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// @brief Build the Window tree and reset the per-run state. Needs the
  /// dispatch context; does not register the root (see register_root()).
  void build();
  /// @brief Register the tree built by build() as this form's top-level root
  /// (see message_box::register_root()).
  void register_root();
  /// @brief Ask the renderer to close the modal; the tree is torn down once
  /// the Window's own "closed" event confirms it (see
  /// message_box::request_close() for why this cannot happen immediately).
  void request_close();
  /// @brief Erase the dialog's elements from the object table and the UI tree.
  void destroy();
  void append_line(const std::string& text, bool is_command);
  /// @brief Run @p fn with the session installed as the dispatch context (it
  /// already is inside an RMI method; on_event() runs outside one).
  void with_context(const std::function<void()>& fn);

  enum class phase { closed, open, closing };
  phase state_{phase::closed};
  bool finished_{false};   // open, showing a failure and the Close button
  bool cancelling_{false}; // the user pressed Cancel
  bool reopen_{false};     // `update` arrived while closing
  std::string command_;    // caption whose output is being appended

  bison::key_t window_id_;
  bison::key_t button_id_;
  ui_element_ptr window_;
  ui_element_ptr command_label_;
  ui_element_ptr bar_;
  ui_element_ptr result_label_;
  ui_element_ptr table_;
  ui_element_ptr button_;

  struct log_row {
    size_t child_key;
    std::vector<bison::key_t> object_ids;
  };
  std::vector<bison::key_t> object_ids_; // layout elements
  std::deque<log_row> rows_;             // log rows, oldest first
  size_t next_child_key_{0};

  // The log is FIFO-capped so a chatty operation stays bounded.
  static constexpr size_t kMaxRows = 300;
};

/// @brief Register ProgressBox in the "wish" bison namespace.
void register_progress_box();

} // namespace bdg::wish
