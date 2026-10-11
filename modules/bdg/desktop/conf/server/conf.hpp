// MIT License © 2026 Binary Dice Games
/// @file conf.hpp
/// @brief Server-side form for conf, a System Settings-like editor of the
///        settings apps describe in the persistent stores.
#pragma once

#include "modules/bdg/common/server/tool_form.hpp"

#include <context/settings_store.hpp>
#include <ui/ui_element.hpp>

#include <map>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Settings editor modeled on macOS System Settings.
///
/// A sidebar lists the categories (built-in ones -- General, Appearance,
/// Desktop, Server -- then the ones apps declare); the panel shows the
/// selected category's settings grouped in cards, each with an editor
/// chosen by its type. Everything comes from the settings metadata
/// (`src/context/settings_schema.hpp`) in the session's user store and the
/// server store; see docs/settings.md.
///
/// - User-store settings are editable by their identity.
/// - Server-store settings are editable only when the session is a settings
///   admin (`context::settings_admin`); otherwise they are shown read-only.
///   The form edits the server store in-process; nothing exposes it over
///   RMI.
///
/// RMI methods:
///   - `refresh` -- re-reads the stores and rebuilds the UI if anything
///     changed (the client runner calls it every two seconds, so settings
///     other apps declare or change show up live).
///   - `select` {id} -- selects a category (as clicking it does).
///
/// Emitted events:
///   - `"closed"` — the user closed the window; internal UI is removed.
class conf : public common::tool_form {
 public:
  explicit conf(bison::dynamic&& base);

  /// @brief RMI `refresh`: see the class comment.
  bison::dynamic do_refresh(const bison::dynamic& args);
  /// @brief RMI `select`: see the class comment.
  bison::dynamic do_select(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// One described setting and the store it lives in.
  struct item {
    settings::described_setting setting;
    bool server{false};
  };
  /// One sidebar category.
  struct category {
    settings::category_info info;
    bool builtin{false};
  };
  /// What an editor widget edits.
  struct editor_binding {
    std::string name;
    bool server{false};
    std::string kind; ///< Widget class: Checkbox, Combo, SliderInt, ...
    ui_element_ptr error_label;
    ui_element_ptr reset_button;
  };

  /// Re-reads both stores into items_/categories_. @return true when what
  /// is shown changed (schema or values).
  bool collect();
  const item* find_item(const std::string& name, bool server) const;
  /// Categories shown in the sidebar, in order.
  std::vector<const category*> visible_categories() const;
  void rebuild();
  void rebuild_sidebar();
  void rebuild_panel();
  /// Imports @p json, registers its nodes, stamps unique paths under
  /// @p scope and puts it as the only child of @p container.
  ui_tree attach(const ui_element_ptr& container, const std::string& scope, const std::string& json);
  void forget(std::vector<bison::key_t>& ids);
  void select_category(const std::string& id);
  void on_edit(const editor_binding& binding, const bison::dynamic& payload);
  void on_reset(const std::string& name, bool server);
  persistent_store* store_for(bool server);

  bison::key_t window_id_{0u};
  bison::key_t search_id_{0u};
  ui_element_ptr sidebar_list_;
  ui_element_ptr panel_;
  ui_element_ptr account_name_;
  ui_element_ptr account_role_;

  std::vector<item> items_;
  std::vector<category> categories_;
  std::string signature_;
  std::string selected_{"general"};
  std::string query_;
  bool admin_{false};
  bool has_user_store_{false};

  std::vector<bison::key_t> sidebar_ids_;
  std::vector<bison::key_t> panel_ids_;
  std::map<bison::hash_t, std::string> category_rows_; ///< Selectable id -> category id
  std::map<bison::hash_t, editor_binding> editors_;
};

/// @brief Registers the `Conf` form class in the "wish" namespace.
void register_conf();

} // namespace bdg::wish
