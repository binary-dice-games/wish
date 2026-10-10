// MIT License © 2026 Binary Dice Games
/// @file tool_form.cpp
/// @brief Implementation of common::tool_form.
#include "modules/bdg/common/server/tool_form.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/forms/message_box.hpp>

namespace bdg::wish::common {

using namespace bison;

tool_form::tool_form(dynamic&& base) : form(std::move(base)) {}

tool_form::~tool_form() = default;

void tool_form::assign_ids(ui_tree& tree) {
  // put_object() files each element under the current request's group (see
  // rmi::context::current_group) so they're cleaned up together with the
  // rest of this form when relayed through rmi::bridge.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
}

std::string tool_form::tr(std::string_view text) {
  if (!is_translatable(text))
    return std::string{text};
  if (!translations_)
    translations_ = sess().translations_for(i18n_prefix_);
  return translate_text(text, *translations_);
}

void tool_form::build_window(
    const std::string& root_key,
    const char* layout_json,
    key_t& window_id_out,
    const std::function<void(ui_tree&)>& wire) {
  if (!translations_ && !i18n_prefix_.empty())
    translations_ = sess().translations_for(i18n_prefix_);
  auto tree = import_json(layout_json, i18n_prefix_.empty() ? nullptr : translations_.get());
  assign_ids(tree);
  window_id_out = wish_id_of(tree[""]);
  if (wire)
    wire(tree);

  ui_element_ptr root_ptr = tree[""];
  sess().ui_objects.merge(std::move(tree), root_key);
  // The main root's top-level registration and "__path__" are handled by
  // form::init() once on_init() returns; secondary windows register here.
  if (root_key != internal_root_key_) {
    sess().top_level_objects[key_t{root_key}] = root_ptr;
    sess().top_level_handlers[key_t{root_key}] = this;
    (*root_ptr)["__path__"_key] = root_key;
  }
}

void tool_form::assign_id(const ui_element_ptr& el) {
  key_t id = rmi::shared::generate_id();
  ctx().put_object(id, el);
  el["__wish_id"_key] = id;
}

ui_element_ptr tool_form::make_label(const std::string& text, const char* light, const char* dark) {
  ui_element_ptr l = ui_element_ptr::create("wish"_key, "Label"_key);
  l["text"_key] = text;
  if (light)
    l["text_color_light"_key] = std::string{light};
  if (dark)
    l["text_color_dark"_key] = std::string{dark};
  assign_id(l);
  return l;
}

void tool_form::erase_objects(const std::vector<key_t>& ids) {
  for (auto id : ids) {
    ctx().objects.erase(id.id);
    click_handlers_.erase(id);
  }
}

bool tool_form::dispatch_click(key_t id) {
  auto it = click_handlers_.find(id);
  if (it == click_handlers_.end())
    return false;
  const auto handler = it->second; // copy: the handler may rebuild the map
  handler();
  return true;
}

void tool_form::run_in_dispatch(const std::function<void()>& fn) {
  if (detail::current_context) {
    fn();
    return;
  }
  auto session = context_wlock{*sync_ctx_};
  detail::current_context = &*session;
  fn();
  detail::current_context = nullptr;
}

void tool_form::show_confirm(const std::string& message, std::function<void()> on_yes, confirm_options options) {
  dynamic params;
  params["title"_key] = options.title;
  params["message"_key] = message;
  params["icon"_key] = std::string{"warning"};
  params["buttons"_key] = std::string{"yes_no"};

  confirm_dialog_ = instantiate_child_form<message_box>(
      "MessageBox"_key,
      std::move(params),
      [on_yes = std::move(on_yes), on_no = std::move(options.on_no)](key_t /*event_name*/, const dynamic& payload) {
        if (str_of(payload, "button"_key) == "yes") {
          if (on_yes)
            on_yes();
        } else if (on_no) {
          on_no();
        }
      });
}

void tool_form::show_message(const std::string& title, const std::string& message, const std::string& icon) {
  dynamic params;
  params["title"_key] = title;
  params["message"_key] = message;
  params["icon"_key] = icon;
  params["buttons"_key] = std::string{"ok"};
  message_dialog_ =
      instantiate_child_form<message_box>("MessageBox"_key, std::move(params), [](key_t, const dynamic&) {});
}

} // namespace bdg::wish::common
