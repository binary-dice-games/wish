// MIT License © 2026 Binary Dice Games
/// @file notification_service.cpp
/// @brief Implementation of wish::notification_service.
#include <server/notification_service.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <utility>

namespace bdg::wish {

using namespace bison;

namespace {

// Cuts @p s to at most @p max bytes without splitting a UTF-8 sequence.
std::string clip_utf8(std::string s, size_t max) {
  if (s.size() <= max)
    return s;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u)
    --n; // s[n] continues a sequence: cut before its lead byte
  s.resize(n);
  return s;
}

notification_kind kind_of(int32_t v) {
  switch (v) {
    case 1:
      return notification_kind::success;
    case 2:
      return notification_kind::warning;
    case 3:
      return notification_kind::error;
    default:
      return notification_kind::info;
  }
}

std::string text_arg(const dynamic& p, const char* name) {
  const auto* f = p.findField(key_t{name});
  return f && f->is<std::string>() ? f->as<std::string>() : std::string{};
}

} // namespace

notification_service_ptr notification_service::instantiate(notification_center_ptr center, std::string source) {
  return std::make_shared<notification_service>(
      dynamic::instantiate(key_t{"wish"}, key_t{"__WishNotifications"}), std::move(center), std::move(source));
}

notification_service::notification_service(dynamic&& base, notification_center_ptr center, std::string source)
    : dynamic(std::move(base)), center_(std::move(center)) {
  *source_.wlock() = std::move(source);
  if (!center_)
    throw std::invalid_argument("wish::notification_service: null notification center");
  static std::atomic<uint64_t> next_session{1};
  tag_prefix_ = "client/" + std::to_string(next_session.fetch_add(1)) + "/";

  addMethod("post"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              notification n;
              n.title = text_arg(p, "title");
              n.body = text_arg(p, "body");
              n.keys = text_arg(p, "keys");
              n.tag = text_arg(p, "tag");
              n.kind = kind_of(p.get_as<int32_t>("kind"_key, 0));
              n.duration = std::chrono::milliseconds{p.get_as<int32_t>("duration_ms"_key, 6000)};
              dynamic result;
              result["id"_key] = static_cast<int32_t>(post(std::move(n)));
              return result;
            }});
  addMethod("dismiss"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              dynamic result;
              const int32_t id = p.get_as<int32_t>("id"_key, 0);
              result["dismissed"_key] = id > 0 && dismiss(static_cast<uint64_t>(id));
              return result;
            }});
}

notification_service::~notification_service() {
  for (uint64_t id : *ids_.rlock())
    center_->dismiss(id);
}

uint64_t notification_service::post(notification n) {
  if (n.title.empty())
    throw std::invalid_argument("wish: a notification needs a title");
  n.title = clip_utf8(std::move(n.title), kMaxTitle);
  n.body = clip_utf8(std::move(n.body), kMaxBody);
  n.keys = clip_utf8(std::move(n.keys), kMaxKeys);
  const auto max_duration = std::chrono::milliseconds{kMaxDurationMs};
  if (n.duration.count() < 0)
    n.duration = std::chrono::milliseconds{0};
  if (n.duration > max_duration)
    n.duration = max_duration;
  if (!n.tag.empty())
    n.tag = tag_prefix_ + clip_utf8(std::move(n.tag), 64);
  {
    const auto source = source_.rlock();
    n.source = source->empty() ? std::string{"an anonymous app"} : *source;
  }

  auto ids = ids_.wlock();
  // Forget ids that expired or were clicked away.
  std::set<uint64_t> live;
  for (const auto& a : center_->active())
    live.insert(a.id);
  ids->erase(std::remove_if(ids->begin(), ids->end(), [&](uint64_t id) { return !live.count(id); }), ids->end());

  const uint64_t id = center_->post(std::move(n));
  if (std::find(ids->begin(), ids->end(), id) != ids->end())
    return id; // a tag replaced one of ours: same id, same slot
  ids->push_back(id);
  while (ids->size() > kMaxPerSession) {
    center_->dismiss(ids->front());
    ids->erase(ids->begin());
  }
  return id;
}

bool notification_service::dismiss(uint64_t id) {
  auto ids = ids_.wlock();
  auto it = std::find(ids->begin(), ids->end(), id);
  if (it == ids->end())
    return false;
  ids->erase(it);
  const auto active = center_->active();
  const bool live = std::any_of(active.begin(), active.end(), [&](const auto& a) { return a.id == id; });
  center_->dismiss(id);
  return live;
}

void register_notification_service() {
  auto proto = dynamic_ptr{"__WishNotifications"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

} // namespace bdg::wish
