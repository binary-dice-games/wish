// MIT License © 2026 Binary Dice Games
/// @file notification_rpc.hpp
/// @brief Client-side calls into the server's `__WishNotifications`
///        service, shared by `wish::client` and `wish::standalone`.
#pragma once

#include <server/notifications.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bdg::wish::detail {

/// @brief Throws `std::logic_error` when the server offers no notification
///        service (e.g. an older server); called inside the returned
///        future's task, so it surfaces through `.get()`.
inline void require_notifications(const std::optional<bison::rmi::proxy::dynamic>& proxy) {
  if (!proxy)
    throw std::logic_error("wish: this server shows no notifications (see has_notifications())");
}

/// @brief Synchronous `post`; returns the notification's id.
inline uint64_t notify_sync(bison::rmi::proxy::dynamic& proxy, const notification& n) {
  using namespace bison;
  dynamic args;
  args["title"_key] = n.title;
  args["body"_key] = n.body;
  args["keys"_key] = n.keys;
  args["tag"_key] = n.tag;
  args["kind"_key] = static_cast<int32_t>(n.kind);
  args["duration_ms"_key] = static_cast<int32_t>(n.duration.count());
  auto result = proxy.call("post"_key, std::move(args)).get();
  return static_cast<uint64_t>(result.get_as<int32_t>("id"_key, 0));
}

/// @brief Synchronous `dismiss`; true when it was still on screen.
inline bool dismiss_notification_sync(bison::rmi::proxy::dynamic& proxy, uint64_t id) {
  using namespace bison;
  dynamic args;
  args["id"_key] = static_cast<int32_t>(id);
  auto result = proxy.call("dismiss"_key, std::move(args)).get();
  return result.get_as<bool>("dismissed"_key, false);
}

} // namespace bdg::wish::detail
