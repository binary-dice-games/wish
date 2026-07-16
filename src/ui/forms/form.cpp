// MIT License © 2025 Binary Dice Games
/// @file form.cpp
/// @brief Implementation of the wish::form base class.
#include "ui/forms/form.hpp"

namespace bdg::wish {

using namespace bison;

// ── form ─────────────────────────────────────────────────────────────────────

form::form(bison::dynamic&& base) : ui_root(std::move(base)) {}

form::~form() {
  remove_internal_objects();
}

void form::remove_internal_objects() {
  remove_objects_at(internal_root_key_);
}

void form::remove_objects_at(const std::string& root_key) {
  if (root_key.empty() || !sync_ctx_)
    return;
  const std::string dot = root_key + ".";

  auto do_remove = [&](context& s) {
    s.top_level_objects.erase(bison::key_t{root_key});
    s.top_level_handlers.erase(bison::key_t{root_key});
    for (auto it = s.ui_objects.begin(); it != s.ui_objects.end();) {
      if (it->first == root_key || it->first.rfind(dot, 0) == 0)
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

void form::request_close_at(const std::string& root_key) {
  auto set_flag = [&](context& s) {
    auto it = s.ui_objects.find(root_key);
    if (it != s.ui_objects.end() && it->second)
      (*it->second)["__request_close__"_key] = true;
  };
  // Mirrors remove_objects_at()'s own dispatch/non-dispatch branching:
  // on_event() is documented to run outside the session lock, so sess()
  // (which requires an active dispatch) cannot be used here.
  if (detail::current_context) {
    set_flag(*detail::current_context);
  } else {
    auto lock = context_wlock{*sync_ctx_};
    set_flag(*lock);
  }
}

std::string form::next_available_key(const std::string& prefix) {
  auto& s = sess();
  for (int i = 0;; ++i) {
    std::string candidate = prefix + std::to_string(i);
    if (s.top_level_objects.find(bison::key_t{candidate}) == s.top_level_objects.end())
      return candidate;
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
    if (it != s.ui_objects.end()) {
      s.top_level_objects[bison::key_t{internal_root_key_}] = it->second;
      // ui_tree::merge() (ui_importer.hpp) only prefixes ui_objects' keys —
      // it never rewrites each element's own "__path__" field, so the root
      // Window still carries the empty "" stamped by build_ui_node(). Stamp
      // it here with internal_root_key_ (fixed per form class, see that
      // field's doc comment) so stable_id() in imgui_ui_renderer.hpp picks
      // it up instead of falling back to the per-run __wish_id.
      (*it->second)["__path__"_key] = internal_root_key_;
    }
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
