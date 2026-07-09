// MIT License © 2025 Binary Dice Games
/// @file form.cpp
/// @brief Implementation of the wish::form base class.
#include "ui/forms/form.hpp"

namespace bdg::wish {

// ── form ─────────────────────────────────────────────────────────────────────

form::form(bison::dynamic&& base) : ui_root(std::move(base)) {}

form::~form() {
  remove_internal_objects();
}

void form::remove_internal_objects() {
  if (internal_root_key_.empty() || !sync_ctx_)
    return;
  const std::string dot = internal_root_key_ + ".";

  auto do_remove = [&](context& s) {
    s.top_level_objects.erase(bison::key_t{internal_root_key_});
    s.top_level_handlers.erase(bison::key_t{internal_root_key_});
    for (auto it = s.ui_objects.begin(); it != s.ui_objects.end();) {
      if (it->first == internal_root_key_ || it->first.rfind(dot, 0) == 0)
        it = s.ui_objects.erase(it);
      else
        ++it;
    }
  };

  if (detail::current_context) {
    // Called within dispatch: wlock already held by the dispatch hook.
    do_remove(*detail::current_context);
  } else {
    // Called outside dispatch (event handler or destructor): acquire wlock.
    auto lock = context_wlock{*sync_ctx_};
    do_remove(*lock);
  }
}

void form::init(bison::rmi::context& ctx, sync_context_ptr sync_ctx) {
  ctx_ = &ctx;
  sync_ctx_ = std::move(sync_ctx);
  on_init();
  // After on_init() the subclass has populated internal_root_key_. Register
  // the root window as renderable and this form as its event handler.
  // Called within dispatch: detail::current_context (wlock held) is valid.
  if (!internal_root_key_.empty() && detail::current_context) {
    auto& s = *detail::current_context;
    auto it = s.ui_objects.find(internal_root_key_);
    if (it != s.ui_objects.end())
      s.top_level_objects[bison::key_t{internal_root_key_}] = it->second;
    s.top_level_handlers[bison::key_t{internal_root_key_}] = this;
  }
}

void form::emit(bison::key_t event_name, bison::dynamic payload) {
  if (!sync_ctx_)
    return;
  // Resolve our own RMI ID lazily on the first call by scanning the object
  // table. Forms emit at interactive speed (button clicks), so the one-time
  // O(n) scan is negligible. The ID is stable once assigned by handle_instantiate.
  if (!own_id_.id && ctx_) {
    for (auto& [hash, obj] : ctx_->objects) {
      if (obj.get() == this) {
        own_id_ = bison::key_t{hash};
        break;
      }
    }
  }
  if (!own_id_.id)
    return;

  if (detail::current_context) {
    // Within dispatch: wlock already held.
    enqueue_event(*detail::current_context, own_id_, event_name, std::move(payload));
  } else {
    // Outside dispatch: acquire the wlock ourselves.
    auto lock = context_wlock{*sync_ctx_};
    enqueue_event(*lock, own_id_, event_name, std::move(payload));
  }
}

} // namespace bdg::wish
