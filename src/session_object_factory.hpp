// MIT License © 2025 Binary Dice Games
/// @file session_object_factory.hpp
/// @brief Shared on_create_object helpers used by both wish::server and
///        wish::standalone.
#pragma once

#include <session.hpp>

#include "src/bison/bison_common.hpp"
#include "src/rmi/server/context.hpp"

namespace bdg::wish::detail {

/// @brief Return the per-session singleton instance for a `__Wish*` protocol
///        class (`__WishFileSystem`, `__WishStyle`, `__WishLogger`).
///
/// These classes are session-scoped singletons rather than per-instantiate
/// objects, so `on_create_object` must return the existing service instance
/// instead of constructing a new one.
///
/// @param s     Session owning the singleton services.
/// @param klass Requested class key.
/// @return The singleton's `dynamic_ptr`, or an empty `dynamic_ptr` if
///         @p klass is not one of the singleton protocol classes (or the
///         session has no such service attached).
bison::dynamic_ptr find_singleton_service(const session& s, bison::key_t klass);

/// @brief Inject session context into a freshly created form/template_handler.
///
/// No-op if @p obj is null or not a `form`/`template_handler` instance.
///
/// @param obj       Object returned by the base class's `on_create_object`.
/// @param ctx       Per-session RMI context; must outlive @p obj.
/// @param sync_sess Synchronized wish session; held for @p obj's lifetime.
void init_session_object(const bison::dynamic_ptr& obj, bison::rmi::context& ctx, const sync_session_ptr& sync_sess);

} // namespace bdg::wish::detail
