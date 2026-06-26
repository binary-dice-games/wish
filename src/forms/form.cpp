// MIT License © 2025 Binary Dice Games
/// @file form.cpp
/// @brief Implementation of the wish::form base class.
#include <wish/form.hpp>

namespace bdg::wish {

// ── form ─────────────────────────────────────────────────────────────────────

form::form(bison::dynamic&& base)
    : ui_root(std::move(base)) {}

form::~form() {
  remove_internal_objects();
}

void form::remove_internal_objects() {
  if (internal_root_key_.empty() || !sync_sess_) return;
  const std::string dot = internal_root_key_ + ".";

  auto do_remove = [&](session& s) {
    s.top_level_objects.erase(bison::key_t{internal_root_key_});
    s.top_level_handlers.erase(bison::key_t{internal_root_key_});
    for (auto it = s.objects.begin(); it != s.objects.end(); ) {
      if (it->first == internal_root_key_ || it->first.rfind(dot, 0) == 0)
        it = s.objects.erase(it);
      else
        ++it;
    }
  };

  if (detail::current_session) {
    // Called within dispatch: wlock already held by the dispatch hook.
    do_remove(*detail::current_session);
  } else {
    // Called outside dispatch (event handler or destructor): acquire wlock.
    auto lock = sync_sess_->wlock();
    do_remove(*lock);
  }
}

void form::init(bison::rmi::context& ctx, sync_session_ptr sync_sess) {
  ctx_       = &ctx;
  sync_sess_ = std::move(sync_sess);
  on_init();
  // After on_init() the subclass has populated internal_root_key_. Register
  // the root window as renderable and this form as its event handler.
  // Called within dispatch: detail::current_session (wlock held) is valid.
  if (!internal_root_key_.empty() && detail::current_session) {
    auto& s = *detail::current_session;
    auto it = s.objects.find(internal_root_key_);
    if (it != s.objects.end())
      s.top_level_objects[bison::key_t{internal_root_key_}] = it->second;
    s.top_level_handlers[bison::key_t{internal_root_key_}] = this;
  }
}

void form::emit(bison::key_t event_name, bison::dynamic payload) {
  if (!sync_sess_) return;
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
  if (!own_id_.id) return;

  if (detail::current_session) {
    // Within dispatch: wlock already held.
    auto& s = *detail::current_session;
    if (s.emit_event) s.emit_event(own_id_, event_name, std::move(payload));
  } else {
    auto lock = sync_sess_->rlock();
    if (lock->emit_event)
      lock->emit_event(own_id_, event_name, std::move(payload));
  }
}

}  // namespace bdg::wish
