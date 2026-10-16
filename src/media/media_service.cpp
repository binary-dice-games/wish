// MIT License © 2026 Binary Dice Games
/// @file media_service.cpp
/// @brief Implementation of wish::media::media_service.
#include <media/media_service.hpp>

#include <context/file_service.hpp>

#include "src/rmi/shared/ids.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bdg::wish::media {

using namespace bison;

namespace {

std::string text_arg(const dynamic& p, const char* name) {
  const auto* f = p.findField(key_t{name});
  return f && f->is<std::string>() ? f->as<std::string>() : std::string{};
}

void check_name(const std::string& name) {
  if (name.empty() || name.size() > media_service::kMaxName)
    throw std::invalid_argument("wish: a media player needs a name of 1-128 bytes");
  if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
    throw std::invalid_argument("wish: a media player name can't hold control characters");
}

} // namespace

media_service_ptr media_service::instantiate(
    std::filesystem::path resource_dir, bool allow_absolute_paths, playback_options defaults) {
  return std::make_shared<media_service>(dynamic::instantiate(key_t{"wish"}, key_t{"__WishMedia"}),
      std::move(resource_dir), allow_absolute_paths, std::move(defaults));
}

media_service::media_service(
    dynamic&& base, std::filesystem::path resource_dir, bool allow_absolute_paths, playback_options defaults)
    : dynamic(std::move(base)), allow_absolute_paths_(allow_absolute_paths), defaults_(std::move(defaults)) {
  *resource_dir_.wlock() = std::move(resource_dir);

  addMethod("open"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              auto player = open(text_arg(p, "name"), text_arg(p, "src"), p);
              // Make the player an RMI object of this session, so the client
              // can call it and receive its events.
              bison::rmi::context* ctx = rmi_ctx_.load(std::memory_order_acquire);
              if (!ctx)
                ctx = detail::current_context;
              if (ctx) {
                const key_t id = rmi::shared::generate_id();
                ctx->put_object(id, dynamic_ptr{std::static_pointer_cast<dynamic>(player)});
                player->set_rmi_id(id);
              }
              const auto& info = player->engine()->info();
              dynamic result;
              result["id"_key] = player->rmi_id();
              result["has_video"_key] = info.has_video;
              result["has_audio"_key] = info.has_audio;
              result["width"_key] = static_cast<int32_t>(info.width);
              result["height"_key] = static_cast<int32_t>(info.height);
              result["duration"_key] = static_cast<float>(info.duration);
              return result;
            }});
  addMethod("close"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              dynamic result;
              result["closed"_key] = close(text_arg(p, "name"));
              return result;
            }});
  addMethod("list"_key, method{[this](dynamic& /*self*/, const dynamic&) -> dynamic {
              dynamic list;
              size_t i = 0;
              for (const auto& n : names())
                list[i++] = n;
              dynamic result;
              result["players"_key] = dynamic_ptr{std::move(list)};
              return result;
            }});
}

media_service::~media_service() {
  auto players = std::move(*players_.wlock());
  for (auto& [name, player] : players)
    player->close();
}

void media_service::set_resource_dir(std::filesystem::path resource_dir) {
  *resource_dir_.wlock() = std::move(resource_dir);
}

media_player_ptr media_service::open(const std::string& name, const std::string& src, const dynamic& params) {
  check_name(name);
  if (src.empty())
    throw std::invalid_argument("wish: media src is empty");
  if (src.find("://") != std::string::npos)
    throw std::invalid_argument("wish: media must be a file in the session sandbox, not a URL");
  const auto path = file_service::resolve_path(src, *resource_dir_.rlock(), allow_absolute_paths_);
  if (path.empty())
    throw std::invalid_argument("wish: media path is outside the session sandbox: " + src);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    throw std::invalid_argument("wish: no such media file in the session sandbox: " + src);
  {
    auto players = players_.rlock();
    if (!players->count(name) && players->size() >= kMaxPlayers)
      throw std::invalid_argument("wish: too many media players open in this session");
  }

  playback_options options = defaults_;
  options.loop = params.get_as<bool>("loop"_key, options.loop);
  options.volume = params.get_as<float>("volume"_key, options.volume);
  auto player = media_player::instantiate(name, path, std::move(options));
  if (params.get_as<bool>("autoplay"_key, false))
    player->engine()->play();

  media_player_ptr replaced;
  {
    auto players = players_.wlock();
    auto& slot = (*players)[name];
    replaced = std::move(slot);
    slot = player;
  }
  if (replaced)
    replaced->close();
  return player;
}

bool media_service::close(const std::string& name) {
  media_player_ptr player;
  {
    auto players = players_.wlock();
    auto it = players->find(name);
    if (it == players->end())
      return false;
    player = std::move(it->second);
    players->erase(it);
  }
  player->close();
  return true;
}

media_player_ptr media_service::find(const std::string& name) const {
  auto players = players_.rlock();
  auto it = players->find(name);
  return it == players->end() ? nullptr : it->second;
}

std::vector<std::string> media_service::names() const {
  std::vector<std::string> out;
  for (const auto& [name, player] : *players_.rlock())
    out.push_back(name);
  return out;
}

bool media_service::empty() const {
  return players_.rlock()->empty();
}

bool media_service::pump(const context& s, renderer& r) {
  std::vector<media_player_ptr> players;
  for (const auto& [name, player] : *players_.rlock())
    players.push_back(player);
  bool changed = false;
  for (const auto& player : players)
    changed |= player->pump(s, r);
  return changed;
}

void register_media_service() {
  dynamic::addClass("wish"_key, dynamic_ptr{"__WishMedia"_key, {}});
  register_media_player();
}

void pump_sessions(renderer& r, const std::vector<sync_context_ptr>& sessions) {
  for (const auto& sync_ctx : sessions) {
    media_service_ptr svc = context_rlock{*sync_ctx}->media_service;
    if (!svc || svc->empty())
      continue;
    auto sess = context_wlock{*sync_ctx};
    if (svc->pump(*sess, r))
      sess->dirty.store(std::max(sess->dirty.load(std::memory_order_relaxed), 1), std::memory_order_release);
  }
}

} // namespace bdg::wish::media
