// MIT License © 2026 Binary Dice Games
/// @file notifications.cpp
/// @brief notification_center implementation.
#include <server/notifications.hpp>

#include <algorithm>
#include <utility>

namespace bdg::wish {

uint64_t notification_center::post(notification n) {
  const auto now = std::chrono::steady_clock::now();
  const auto expires = n.duration.count() > 0 ? now + n.duration : std::chrono::steady_clock::time_point::max();
  uint64_t id = 0;
  {
    auto items = items_.wlock();
    if (!n.tag.empty()) {
      auto it = std::find_if(items->begin(), items->end(), [&](const auto& a) { return a.content.tag == n.tag; });
      if (it != items->end()) {
        id = it->id;
        *it = {id, std::move(n), now, expires};
      }
    }
    if (id == 0) {
      id = next_id_.fetch_add(1, std::memory_order_relaxed);
      if (items->size() >= kMaxActive)
        items->erase(items->begin());
      items->push_back({id, std::move(n), now, expires});
    }
  }
  changed_.store(true, std::memory_order_release);
  return id;
}

void notification_center::dismiss(uint64_t id) {
  bool removed = false;
  {
    auto items = items_.wlock();
    auto it = std::remove_if(items->begin(), items->end(), [&](const auto& a) { return a.id == id; });
    removed = it != items->end();
    items->erase(it, items->end());
  }
  if (removed)
    changed_.store(true, std::memory_order_release);
}

void notification_center::clear() {
  items_.wlock()->clear();
  changed_.store(true, std::memory_order_release);
}

std::vector<active_notification> notification_center::active(std::chrono::steady_clock::time_point now) {
  auto items = items_.wlock();
  auto it = std::remove_if(items->begin(), items->end(), [&](const auto& a) { return a.expires <= now; });
  if (it != items->end()) {
    items->erase(it, items->end());
    changed_.store(true, std::memory_order_release);
  }
  return *items;
}

} // namespace bdg::wish
