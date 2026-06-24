// MIT License © 2025 Binary Dice Games
/// @file form.cpp
/// @brief Implementation of the wish::form base class.
#include <wish/form.hpp>

#include <mutex>

namespace bdg::wish {

// ── form ─────────────────────────────────────────────────────────────────────

form::form(bison::dynamic&& base)
    : bison::dynamic(std::move(base)) {}

form::~form() {
  remove_internal_objects();
}

void form::remove_internal_objects() {
  if (internal_root_key_.empty() || !sess_) return;
  auto& objs = sess_->objects;
  const std::string dot = internal_root_key_ + ".";

  // Remove the root window from the top-level map before erasing objects.
  {
    std::lock_guard<std::mutex> lg(sess_->top_level_mutex);
    sess_->top_level_objects.erase(internal_root_key_);
  }

  for (auto it = objs.begin(); it != objs.end(); ) {
    if (it->first == internal_root_key_ || it->first.rfind(dot, 0) == 0)
      it = objs.erase(it);
    else
      ++it;
  }
}

void form::init(bison::rmi::context& ctx, std::shared_ptr<session> sess) {
  ctx_  = &ctx;
  sess_ = std::move(sess);
  on_init();
  // After on_init() the subclass has populated internal_root_key_. Register
  // the root window as an overlay so the render loop draws it each frame.
  if (!internal_root_key_.empty()) {
    auto it = sess_->objects.find(internal_root_key_);
    if (it != sess_->objects.end()) {
      std::lock_guard<std::mutex> lg(sess_->top_level_mutex);
      sess_->top_level_objects[internal_root_key_] = it->second;
    }
  }
}

void form::emit(bison::key_t event_name, bison::dynamic payload) {
  if (!sess_ || !sess_->emit_event) return;
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
  if (own_id_.id)
    sess_->emit_event(own_id_, event_name, std::move(payload));
}

}  // namespace bdg::wish
