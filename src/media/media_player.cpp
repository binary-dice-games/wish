// MIT License © 2026 Binary Dice Games
/// @file media_player.cpp
/// @brief Implementation of wish::media::media_player.
#include <media/media_player.hpp>

#include <context/context.hpp>

#include <stdexcept>
#include <utility>

namespace bdg::wish::media {

using namespace bison;

media_player_ptr media_player::instantiate(std::string name, const std::filesystem::path& file, playback_options options) {
  auto pb = std::make_unique<playback>(file, std::move(options));
  return std::make_shared<media_player>(
      dynamic::instantiate(key_t{"wish"}, key_t{"__WishMediaPlayer"}), std::move(name), std::move(pb));
}

media_player::media_player(dynamic&& base, std::string name, std::unique_ptr<playback> pb)
    : dynamic(std::move(base)), name_(std::move(name)) {
  *pb_.wlock() = std::move(pb);

  auto no_result = [](auto fn) {
    return method{[fn](dynamic& /*self*/, const dynamic& p) -> dynamic {
      fn(p);
      return dynamic{};
    }};
  };
  addMethod("play"_key, no_result([this](const dynamic&) { require()->play(); }));
  addMethod("pause"_key, no_result([this](const dynamic&) { require()->pause(); }));
  addMethod("stop"_key, no_result([this](const dynamic&) { require()->stop(); }));
  addMethod("seek"_key, no_result([this](const dynamic& p) { require()->seek(p.get_as<float>("seconds"_key, 0.0f)); }));
  addMethod("set_volume"_key,
      no_result([this](const dynamic& p) { require()->set_volume(p.get_as<float>("volume"_key, 1.0f)); }));
  addMethod("set_loop"_key, no_result([this](const dynamic& p) { require()->set_loop(p.get_as<bool>("loop"_key, false)); }));
  addMethod("status"_key, method{[this](dynamic& /*self*/, const dynamic&) -> dynamic { return status(); }});
}

std::shared_ptr<playback> media_player::require() const {
  auto pb = engine();
  if (!pb)
    throw std::runtime_error("wish: media player closed");
  return pb;
}

dynamic media_player::status() const {
  auto pb = require();
  const auto& info = pb->info();
  const auto state = pb->state();
  dynamic r;
  r["state"_key] = static_cast<int32_t>(state);
  r["state_name"_key] = std::string{to_string(state)};
  r["position"_key] = static_cast<float>(pb->position());
  r["duration"_key] = static_cast<float>(info.duration);
  r["width"_key] = static_cast<int32_t>(info.width);
  r["height"_key] = static_cast<int32_t>(info.height);
  r["has_video"_key] = info.has_video;
  r["has_audio"_key] = info.has_audio;
  r["volume"_key] = pb->volume();
  r["loop"_key] = pb->loop();
  r["audio_output"_key] = pb->audio_output_active();
  r["error"_key] = pb->error();
  return r;
}

bool media_player::pump(const context& s, renderer& r) {
  auto pb = engine();
  if (!pb)
    return false;
  bool changed = false;
  for (auto& ev : pb->update()) {
    changed = true;
    if (rmi_id_.id == 0)
      continue;
    dynamic payload;
    key_t event_name{"state_changed"};
    switch (ev.what) {
      case playback_event::kind::state_changed:
        payload["state"_key] = static_cast<int32_t>(ev.state);
        payload["state_name"_key] = std::string{to_string(ev.state)};
        break;
      case playback_event::kind::ended:
        event_name = key_t{"ended"};
        break;
      case playback_event::kind::error:
        event_name = key_t{"error"};
        payload["message"_key] = ev.message;
        break;
    }
    enqueue_event(s, rmi_id_, event_name, std::move(payload));
  }
  if (pb->take_frame(frame_)) {
    changed = true;
    if (!texture_ || texture_->width() != frame_.width || texture_->height() != frame_.height)
      texture_ = r.create_dynamic_texture(frame_.width, frame_.height);
    if (texture_)
      texture_->update(frame_.rgba.data(), frame_.width * 4);
  }
  return changed;
}

void media_player::close() {
  std::shared_ptr<playback> pb;
  {
    auto lock = pb_.wlock();
    pb = std::move(*lock);
    lock->reset();
  }
  pb.reset(); // joins the decode thread, closes the device
  texture_.reset();
}

void register_media_player() {
  dynamic::addClass("wish"_key, dynamic_ptr{"__WishMediaPlayer"_key, {}});
}

} // namespace bdg::wish::media
