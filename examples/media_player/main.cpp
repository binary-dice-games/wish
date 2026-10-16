// MIT License © 2026 Binary Dice Games
//
// Media player example: single-binary wish demo of server-side media
// playback (docs/media.md). The client uploads a local audio/video file to
// its session sandbox, opens it with open_media(), and shows it in a Video
// widget with Play/Pause/Stop buttons, a seek bar and a volume slider. The
// server decodes and plays the file; the client only sends commands.
//
// Usage: media_player --file=movie.mp4 [--loop] [--verbose]
//        [--renderer sdl3|web] [--theme dark|light|classic]

#include "../common/example_app.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

DEFINE_string(file, "", "Audio or video file to play (uploaded to the session sandbox first)");
DEFINE_bool(loop, false, "Restart from the beginning at the end");

// ── UI descriptor ─────────────────────────────────────────────────────────────

static constexpr const char* kPlayerDesc = R"({
  "type": "Window",
  "title": "Media Player",
  "width": 680,
  "height": 520,
  "children": {
    "video": { "type": "Video", "player": "movie", "width": -1, "height": 360 },
    "seek":  { "type": "SliderFloat", "label": "##seek", "min": 0.0, "max": 1.0, "format": "%.1f s", "width": -1 },
    "controls": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "play":   { "type": "Button", "label": "&Play",  "width": 80 },
        "pause":  { "type": "Button", "label": "P&ause", "width": 80 },
        "stop":   { "type": "Button", "label": "&Stop",  "width": 80 },
        "volume": { "type": "SliderFloat", "label": "Volume", "value": 1.0, "min": 0.0, "max": 1.0, "width": 160 }
      }
    },
    "status": { "type": "Label", "text": "" }
  }
})";

// ── Client ────────────────────────────────────────────────────────────────────

class media_client : public wish::examples::example_client {
 public:
  media_client(memory_client_transport t, wish::renderer* renderer, bool verbose, std::string theme)
      : wish::examples::example_client(std::move(t), renderer, verbose, std::move(theme), "media") {}

 protected:
  void on_session() override {
    set_style_preset(theme_).get();
    if (!has_media()) {
      std::cerr << "media_player: this server was built without media support (FFmpeg)\n";
      return;
    }

    // 1. The server only plays files in the session sandbox: upload first.
    const std::filesystem::path local = FLAGS_file;
    const std::string sandbox_name = "media/" + local.filename().string();
    {
      std::ifstream in(local, std::ios::binary);
      if (!in) {
        std::cerr << "media_player: cannot read " << local << "\n";
        return;
      }
      vlog("uploading " + local.string() + " as " + sandbox_name);
      upload_file(sandbox_name, in).get();
    }

    // 2. Open it as player "movie" (the name the Video widget refers to).
    wish::media_options options;
    options.autoplay = true;
    options.loop = FLAGS_loop;
    auto player = open_media("movie", sandbox_name, options).get();
    const auto info = player.status().get();
    vlog("opened: " + std::to_string(info.width) + "x" + std::to_string(info.height) + ", " +
        std::to_string(info.duration) + " s");

    register_template_from_json("player"_key, kPlayerDesc).get();
    auto ui = instantiate_template("player"_key).get();
    {
      dynamic f;
      f["max"_key] = static_cast<float>(info.duration > 0 ? info.duration : 1.0);
      ui.at("seek").set(std::move(f)).get();
    }

    // 3. Wire the controls to the player.
    ui.at("controls.play").onEvent("clicked"_key, [player](dynamic) mutable { player.play(); });
    ui.at("controls.pause").onEvent("clicked"_key, [player](dynamic) mutable { player.pause(); });
    ui.at("controls.stop").onEvent("clicked"_key, [player](dynamic) mutable { player.stop(); });
    ui.at("video").onEvent("clicked"_key, [player](dynamic) mutable {
      // Click the picture to toggle play/pause.
      const bool playing = player.status().get().state == wish::media_state::playing;
      playing ? player.pause() : player.play();
    });
    ui.at("seek").onEvent("changed"_key, [player](dynamic p) mutable {
      player.seek(p.get_as<float>("value"_key, 0.0f));
    });
    ui.at("controls.volume").onEvent("changed"_key, [player](dynamic p) mutable {
      player.set_volume(p.get_as<float>("value"_key, 1.0f));
    });
    player.on_ended([this] { vlog("ended"); });
    player.on_error([](const std::string& message) { std::cerr << "media_player: " << message << "\n"; });

    // 4. Show the position until the window closes.
    static const char* kStateNames[] = {"stopped", "playing", "paused", "ended", "error"};
    while (!renderer_->should_quit()) {
      try {
        const auto st = player.status().get();
        char text[128];
        std::snprintf(text, sizeof(text), "%.1f / %.1f s  -  %s%s", st.position, st.duration,
            kStateNames[static_cast<int>(st.state)], st.audio_output || !st.has_audio ? "" : "  (no sound card)");
        dynamic f;
        f["text"_key] = std::string{text};
        ui.at("status").set(std::move(f));
      } catch (const std::exception&) {
        break; // the session ended
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
    player.close().get();
  }
};

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  // run_example() parses the flags; check --file once it has.
  return wish::examples::run_example(
      argc, argv, "media", "Media Player", 700, 560, false,
      [](memory_client_transport t, wish::renderer* r, bool verbose, std::string theme)
          -> std::unique_ptr<wish::examples::example_client> {
        if (FLAGS_file.empty()) {
          std::cerr << "usage: media_player --file=<audio or video file> [--loop]\n";
          std::exit(2);
        }
        return std::make_unique<media_client>(std::move(t), r, verbose, std::move(theme));
      });
}
