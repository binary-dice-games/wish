// MIT License © 2026 Binary Dice Games
/// @file tool_form.hpp
/// @brief Base class of the bdg modules' server-side forms: window building,
///        click dispatch and the confirm / message dialogs every tool uses.
///
/// Server-side counterpart of the client's `common::tool_source`. Each bdg
/// form used to re-implement the same plumbing; deriving from tool_form
/// instead of `wish::form` gives it:
///
///   - build_window() -- import a JSON layout, give every node an RMI id and
///     register it as the form's main root or as an extra dockable root;
///   - assign_id() / make_label() / erase_objects() -- C++-built rows;
///   - click_handlers_ / on_click() / dispatch_click() -- id -> handler map;
///   - show_confirm() / show_message() -- the built-in MessageBox form as a
///     private child dialog (form::instantiate_child_form()).
///
/// The shared panels (console_panel, list_panel, text_viewer_panel,
/// rolling_plot) are members of a tool_form and build on these.
#pragma once

#include "modules/bdg/common/server/ui_helpers.hpp"

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bdg::wish {
class message_box;
} // namespace bdg::wish

namespace bdg::wish::common {

class console_panel;
class text_viewer_panel;
class rolling_plot;
template <typename Meta>
class table_rows;
template <typename Meta>
class list_panel;

/// @brief Optional settings for tool_form::show_confirm().
struct confirm_options {
  std::string title{"Confirm"};
  /// Called when the user answers anything but "Yes" (No, or closes it).
  std::function<void()> on_no;
};

class tool_form : public form {
 public:
  explicit tool_form(bison::dynamic&& base);
  ~tool_form() override;

 protected:
  using click_handler = std::function<void()>;

  /// @brief Gives every node of @p tree a fresh RMI id (registered in
  /// `ctx().objects` and stamped into its `__wish_id`).
  void assign_ids(ui_tree& tree);

  /// @brief Translate @p text (`"$$KEY"`) with this tool's translations
  /// (`res/<i18n_prefix_>/i18n/<lang>.lang`, see context::translations_for());
  /// any other string is returned unchanged. The map is fetched on first use
  /// (which must be during dispatch, e.g. on_init()) and kept, so later calls
  /// also work from on_event().
  std::string tr(std::string_view text);

  /// @brief `"<org>/<collection>/<name>"` of the module whose translation
  /// files tr() and build_window() use. Set it in the constructor; empty
  /// (the default) leaves layouts untranslated.
  std::string i18n_prefix_;

  /// @brief Imports @p layout_json, assigns ids, calls @p wire with the tree
  /// (to cache widgets and bind handlers) and merges it under @p root_key.
  /// `"$$KEY"` strings in the layout are translated (see tr()).
  /// A root other than `internal_root_key_` (which form::init() registers
  /// itself) is registered as a top-level object of its own, so it docks
  /// independently.
  /// @param[out] window_id_out  The root Window's id (its `closed` events).
  void build_window(
      const std::string& root_key,
      const char* layout_json,
      bison::key_t& window_id_out,
      const std::function<void(ui_tree&)>& wire = {});

  /// @brief Registers a C++-created element under a fresh RMI id.
  void assign_id(const ui_element_ptr& el);

  /// @brief A registered `Label` showing @p text, optionally coloured.
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);
  ui_element_ptr make_label(const std::string& text, const theme_color& color) {
    return make_label(text, color.light, color.dark);
  }

  /// @brief Unregisters elements created with assign_id(): erases each id
  /// from `ctx().objects` and from click_handlers_.
  void erase_objects(const std::vector<bison::key_t>& ids);

  /// @brief Runs @p handler when the widget @p id is clicked (see
  /// dispatch_click()).
  void on_click(bison::key_t id, click_handler handler) {
    click_handlers_[id] = std::move(handler);
  }

  /// @brief Runs the click handler registered for @p id, if any.
  /// @return Whether one was registered. The handler is copied before it
  /// runs, so it may replace or erase its own entry.
  bool dispatch_click(bison::key_t id);

  /// @brief Opens a modal yes/no MessageBox (warning icon) and calls
  /// @p on_yes when the user answers "Yes". Replacing a still-open confirm
  /// dialog with a new one is safe (see form::instantiate_child_form()).
  void show_confirm(const std::string& message, std::function<void()> on_yes, confirm_options options = {});

  /// @brief Opens a modal MessageBox with a single OK button.
  /// @param icon  `"error"`, `"warning"` or `"info"`.
  void show_message(const std::string& title, const std::string& message, const std::string& icon = "error");

  /// @brief Runs @p fn with the session installed as the dispatch context
  /// when called outside RMI dispatch (from on_event()), so code that needs
  /// sess() -- building a window on demand -- works from either place. The
  /// idiom form::instantiate_child_form() uses.
  void run_in_dispatch(const std::function<void()>& fn);

  /// id -> handler for buttons, menu items and other clickable widgets.
  std::unordered_map<bison::key_t, click_handler, bison::key_t, bison::key_t> click_handlers_;

 private:
  std::shared_ptr<const translation_map> translations_;

  friend class console_panel;
  friend class text_viewer_panel;
  friend class rolling_plot;
  template <typename Meta>
  friend class table_rows;
  template <typename Meta>
  friend class list_panel;

  std::shared_ptr<message_box> confirm_dialog_;
  std::shared_ptr<message_box> message_dialog_;
};

} // namespace bdg::wish::common
