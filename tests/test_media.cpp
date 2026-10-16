// MIT License © 2026 Binary Dice Games
// Tests for server-side media playback (src/media/): the FFmpeg decoder, the
// playback state machine, the per-session media service and its sandbox
// rules, and frame upload through a renderer's dynamic textures.
#include <gtest/gtest.h>

#include "media_test_files.hpp"

#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <context/context.hpp>
#include <imgui/imgui_renderer.hpp>
#include <imgui/imgui_ui_renderer.hpp>
#include <ui/ui_importer.hpp>
#include <media/media_decoder.hpp>
#include <media/media_service.hpp>
#include <media/playback.hpp>
#include <server/registry.hpp>
#include <server/renderer.hpp>
#include <server/server.hpp>

#include "src/bison/bison_common.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace wish = bdg::wish;
namespace media = bdg::wish::media;
namespace fs = std::filesystem;

namespace {

constexpr int kFps = 25;

// A scratch directory unique to the running test (ctest runs every test in
// its own process, in parallel).
class MediaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wish::register_all();
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() /
        ("wish_media_" + std::string(info->test_suite_name()) + "_" + info->name() + "_" +
            std::to_string(rmi::shared::generate_id().id));
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // 1 s of red, then 1 s of green, 64x48 at 25 fps.
  fs::path video() {
    auto p = dir_ / "red_green.mkv";
    if (!fs::exists(p))
      wish_test::write_red_green_video(p, 64, 48, kFps, kFps, kFps);
    return p;
  }

  fs::path tone(double seconds = 1.0) {
    auto p = dir_ / "tone.wav";
    if (!fs::exists(p))
      wish_test::write_wav(p, seconds);
    return p;
  }

  fs::path dir_;
};

bool is_red(const media::video_frame& f) {
  const auto* px = &f.rgba[(f.height / 2 * f.width + f.width / 2) * 4];
  return px[0] > 200 && px[1] < 60 && px[2] < 60;
}

bool is_green(const media::video_frame& f) {
  const auto* px = &f.rgba[(f.height / 2 * f.width + f.width / 2) * 4];
  return px[0] < 60 && px[1] > 200 && px[2] < 60;
}

// A clock the test advances by hand, so positions are exact.
struct manual_clock {
  std::shared_ptr<double> now = std::make_shared<double>(100.0);
  std::function<double()> fn() const {
    auto n = now;
    return [n] { return *n; };
  }
  void advance(double s) const {
    *now += s;
  }
};

media::playback_options silent(const manual_clock& clock) {
  media::playback_options o;
  o.audio_device = false;
  o.clock = clock.fn();
  return o;
}

// Advances @p clock in small steps, waiting for the decoder at each, until
// @p done or @p max_seconds of media time passed. Collects every event.
template <class Pred>
std::vector<media::playback_event> advance_until(media::playback& pb, const manual_clock& clock, Pred done,
    media::video_frame* last_frame = nullptr, double max_seconds = 5.0) {
  std::vector<media::playback_event> all;
  media::video_frame scratch;
  media::video_frame& frame = last_frame ? *last_frame : scratch;
  for (double t = 0; t < max_seconds && !done(); t += 0.02) {
    pb.wait_buffered(2.0);
    clock.advance(0.02);
    for (auto& e : pb.update())
      all.push_back(e);
    pb.take_frame(frame); // like the render loop does
  }
  return all;
}

bool has_event(const std::vector<media::playback_event>& events, media::playback_event::kind k) {
  for (const auto& e : events)
    if (e.what == k)
      return true;
  return false;
}

} // namespace

// ── media_decoder ───────────────────────────────────────────────────────────

TEST_F(MediaTest, DecoderReadsVideoInfoAndFrames) {
  media::media_decoder d{video()};
  const auto& info = d.info();
  EXPECT_TRUE(info.has_video);
  EXPECT_FALSE(info.has_audio);
  EXPECT_EQ(info.width, 64);
  EXPECT_EQ(info.height, 48);
  EXPECT_NEAR(info.duration, 2.0, 0.1);

  media::video_frame v;
  media::audio_chunk a;
  ASSERT_EQ(d.decode(v, a), media::media_decoder::result::video);
  EXPECT_EQ(v.width, 64);
  EXPECT_EQ(v.rgba.size(), 64u * 48u * 4u);
  EXPECT_NEAR(v.pts, 0.0, 1e-3);
  EXPECT_TRUE(is_red(v));

  d.seek(1.5);
  // The keyframe at 1.0 s is green; everything after it too.
  ASSERT_EQ(d.decode(v, a), media::media_decoder::result::video);
  EXPECT_GE(v.pts, 0.99);
  EXPECT_TRUE(is_green(v));

  int frames = 1;
  while (d.decode(v, a) != media::media_decoder::result::eof)
    ++frames;
  EXPECT_GE(frames, 20); // the rest of the second second
}

TEST_F(MediaTest, DecoderResamplesAudioToStereo48k) {
  media::media_decoder d{tone(0.5)};
  EXPECT_TRUE(d.info().has_audio);
  EXPECT_FALSE(d.info().has_video);
  EXPECT_NEAR(d.info().duration, 0.5, 0.05);
  media::video_frame v;
  media::audio_chunk a;
  size_t samples = 0;
  for (;;) {
    const auto r = d.decode(v, a);
    if (r == media::media_decoder::result::eof)
      break;
    ASSERT_EQ(r, media::media_decoder::result::audio);
    EXPECT_EQ(a.samples.size() % media::kAudioChannels, 0u);
    samples += a.samples.size();
  }
  EXPECT_NEAR(static_cast<double>(samples) / media::kAudioChannels / media::kAudioRate, 0.5, 0.02);
}

TEST_F(MediaTest, DecoderRejectsFilesThatAreNotMedia) {
  std::ofstream(dir_ / "notes.txt") << "just some text, not a video\n";
  EXPECT_THROW(media::media_decoder{dir_ / "notes.txt"}, std::runtime_error);
  EXPECT_THROW(media::media_decoder{dir_ / "missing.mp4"}, std::runtime_error);
}

TEST_F(MediaTest, DecoderNeverOpensFilesAFileReferences) {
  // A concat list naming a real file next to it: the demuxer must not be
  // able to follow it (that is how a playlist could escape the sandbox).
  tone();
  std::ofstream(dir_ / "list.ffconcat") << "ffconcat version 1.0\nfile 'tone.wav'\n";
  bool refused = false;
  try {
    media::media_decoder d{dir_ / "list.ffconcat"};
    media::video_frame v;
    media::audio_chunk a;
    refused = d.decode(v, a) == media::media_decoder::result::eof;
  } catch (const std::runtime_error&) {
    refused = true;
  }
  EXPECT_TRUE(refused);
}

// ── playback ────────────────────────────────────────────────────────────────

TEST_F(MediaTest, PlaybackShowsTheFirstFrameBeforePlaying) {
  manual_clock clock;
  media::playback pb{video(), silent(clock)};
  EXPECT_EQ(pb.state(), media::play_state::stopped);
  ASSERT_TRUE(pb.wait_buffered(5.0));
  EXPECT_TRUE(pb.update().empty());
  media::video_frame f;
  ASSERT_TRUE(pb.take_frame(f));
  EXPECT_TRUE(is_red(f));
  pb.update();
  EXPECT_FALSE(pb.take_frame(f)) << "shown once, not every update";
}

TEST_F(MediaTest, PlaybackFollowsTheClockAndSeeks) {
  manual_clock clock;
  media::playback pb{video(), silent(clock)};
  pb.play();
  EXPECT_EQ(pb.state(), media::play_state::playing);
  auto events = pb.update();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].what, media::playback_event::kind::state_changed);
  EXPECT_EQ(events[0].state, media::play_state::playing);

  media::video_frame f;
  advance_until(pb, clock, [&] { return pb.position() >= 0.5; }, &f);
  EXPECT_NEAR(pb.position(), 0.5, 0.03);
  ASSERT_FALSE(f.rgba.empty());
  EXPECT_TRUE(is_red(f));
  EXPECT_GT(f.pts, 0.4) << "frames keep up with the clock";
  EXPECT_LE(f.pts, pb.position() + 1e-3);

  pb.pause();
  const double paused_at = pb.position();
  clock.advance(1.0);
  EXPECT_DOUBLE_EQ(pb.position(), paused_at) << "time stands still while paused";
  EXPECT_EQ(pb.state(), media::play_state::paused);

  pb.seek(1.5);
  EXPECT_DOUBLE_EQ(pb.position(), 1.5);
  ASSERT_TRUE(pb.wait_buffered(5.0));
  pb.update();
  ASSERT_TRUE(pb.take_frame(f));
  EXPECT_TRUE(is_green(f));

  pb.stop();
  EXPECT_EQ(pb.state(), media::play_state::stopped);
  EXPECT_DOUBLE_EQ(pb.position(), 0.0);
}

TEST_F(MediaTest, PlaybackEndsOrLoops) {
  manual_clock clock;
  media::playback pb{video(), silent(clock)};
  pb.play();
  auto events = advance_until(pb, clock, [&] { return pb.state() == media::play_state::ended; });
  EXPECT_EQ(pb.state(), media::play_state::ended);
  EXPECT_TRUE(has_event(events, media::playback_event::kind::ended));
  EXPECT_NEAR(pb.position(), 2.0, 0.1);

  // play() from the end restarts; with loop on, the end wraps around.
  pb.set_loop(true);
  pb.play();
  EXPECT_LT(pb.position(), 0.01);
  bool wrapped = false;
  double last = 0;
  events = advance_until(pb, clock, [&] {
    const double p = pb.position();
    wrapped = wrapped || p + 0.5 < last;
    last = p;
    return wrapped;
  });
  EXPECT_TRUE(wrapped);
  EXPECT_EQ(pb.state(), media::play_state::playing);
  EXPECT_FALSE(has_event(events, media::playback_event::kind::ended));
}

TEST_F(MediaTest, SilentAudioPlaybackKeepsTimeAndEnds) {
  manual_clock clock;
  media::playback pb{tone(0.5), silent(clock)};
  EXPECT_FALSE(pb.audio_output_active());
  pb.play();
  advance_until(pb, clock, [&] { return pb.state() == media::play_state::ended; });
  EXPECT_EQ(pb.state(), media::play_state::ended);
  EXPECT_NEAR(pb.position(), 0.5, 0.05);
}

TEST_F(MediaTest, PlaybackOnTheDefaultDeviceEndsInRealTime) {
  // Whatever this machine has -- a sound card, or none (CI): playback must
  // run in real time and end; without a device it is silent.
  media::playback pb{tone(0.3), media::playback_options{}};
  pb.play();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (pb.state() == media::play_state::playing && std::chrono::steady_clock::now() < deadline) {
    pb.wait_buffered(0.05);
    pb.update();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(pb.state(), media::play_state::ended);
  EXPECT_NEAR(pb.position(), 0.3, 0.05);
}

TEST_F(MediaTest, PlaybackClampsVolumeAndSeek) {
  manual_clock clock;
  media::playback pb{video(), silent(clock)};
  pb.set_volume(3.0f);
  EXPECT_FLOAT_EQ(pb.volume(), 1.0f);
  pb.set_volume(-1.0f);
  EXPECT_FLOAT_EQ(pb.volume(), 0.0f);
  pb.seek(99.0);
  EXPECT_NEAR(pb.position(), 2.0, 0.1);
  pb.seek(-5.0);
  EXPECT_DOUBLE_EQ(pb.position(), 0.0);
}

// ── media_service ───────────────────────────────────────────────────────────

namespace {

// Records what a player uploads to its texture.
struct fake_texture : wish::dynamic_texture {
  fake_texture(int w, int h) : w_(w), h_(h) {}
  int width() const override {
    return w_;
  }
  int height() const override {
    return h_;
  }
  void update(const uint8_t* rgba, int pitch) override {
    ++uploads;
    last.width = w_;
    last.height = h_;
    last.rgba.assign(rgba, rgba + static_cast<size_t>(pitch) * h_);
  }
  void* native_handle() const override {
    return nullptr;
  }
  int w_, h_;
  int uploads = 0;
  media::video_frame last;
};

struct texture_renderer : wish::null_renderer {
  wish::dynamic_texture_ptr create_dynamic_texture(int w, int h) override {
    auto t = std::make_shared<fake_texture>(w, h);
    textures.push_back(t);
    return t;
  }
  std::vector<std::shared_ptr<fake_texture>> textures;
};

} // namespace

TEST_F(MediaTest, ServiceOpensOnlyFilesInTheSandbox) {
  video();
  tone();
  media::playback_options quiet;
  quiet.audio_device = false;
  auto svc = media::media_service::instantiate(dir_, false, quiet);
  EXPECT_NO_THROW(svc->open("intro", "red_green.mkv"));
  EXPECT_NE(svc->find("intro"), nullptr);

  EXPECT_THROW(svc->open("x", "../red_green.mkv"), std::invalid_argument);
  EXPECT_THROW(svc->open("x", (dir_ / "red_green.mkv").string()), std::invalid_argument) << "absolute";
  EXPECT_THROW(svc->open("x", "https://example.com/a.mp4"), std::invalid_argument);
  EXPECT_THROW(svc->open("x", "file:///etc/passwd"), std::invalid_argument);
  EXPECT_THROW(svc->open("x", "nope.mp4"), std::invalid_argument);
  EXPECT_THROW(svc->open("", "tone.wav"), std::invalid_argument);
  EXPECT_THROW(svc->open(std::string(200, 'n'), "tone.wav"), std::invalid_argument);
  std::ofstream(dir_ / "readme.txt") << "not media";
  EXPECT_THROW(svc->open("x", "readme.txt"), std::runtime_error);
  EXPECT_EQ(svc->names(), std::vector<std::string>{"intro"});
}

TEST_F(MediaTest, ServiceAllowsAbsolutePathsOnlyWhenTheServerDoes) {
  tone();
  media::playback_options quiet;
  quiet.audio_device = false;
  auto svc = media::media_service::instantiate(dir_, true, quiet);
  EXPECT_NO_THROW(svc->open("a", (dir_ / "tone.wav").string()));
}

TEST_F(MediaTest, ServiceLimitsPlayersAndReplacesByName) {
  tone();
  media::playback_options quiet;
  quiet.audio_device = false;
  auto svc = media::media_service::instantiate(dir_, false, quiet);
  std::vector<media::media_player_ptr> players;
  for (size_t i = 0; i < media::media_service::kMaxPlayers; ++i)
    players.push_back(svc->open("p" + std::to_string(i), "tone.wav"));
  EXPECT_THROW(svc->open("one-too-many", "tone.wav"), std::invalid_argument);

  auto again = svc->open("p0", "tone.wav"); // replacing is not "one more"
  EXPECT_NE(again, players[0]);
  EXPECT_EQ(players[0]->engine(), nullptr) << "the replaced player was closed";
  EXPECT_THROW(players[0]->status(), std::runtime_error);

  EXPECT_TRUE(svc->close("p1"));
  EXPECT_FALSE(svc->close("p1"));
  EXPECT_EQ(svc->names().size(), media::media_service::kMaxPlayers - 1);
}

TEST_F(MediaTest, ServiceOpenHonorsAutoplayLoopAndVolume) {
  tone();
  media::playback_options quiet;
  quiet.audio_device = false;
  auto svc = media::media_service::instantiate(dir_, false, quiet);
  dynamic params;
  params["autoplay"_key] = true;
  params["loop"_key] = true;
  params["volume"_key] = 0.25f;
  auto p = svc->open("bgm", "tone.wav", params);
  auto st = p->status();
  EXPECT_EQ(st.get_as<int32_t>("state"_key, -1), static_cast<int32_t>(media::play_state::playing));
  EXPECT_EQ(st.get_as<std::string>("state_name"_key, ""), "playing");
  EXPECT_TRUE(st.get_as<bool>("loop"_key, false));
  EXPECT_FLOAT_EQ(st.get_as<float>("volume"_key, 0), 0.25f);
  EXPECT_TRUE(st.get_as<bool>("has_audio"_key, false));
  EXPECT_FALSE(st.get_as<bool>("audio_output"_key, true));
}

TEST_F(MediaTest, PumpUploadsFramesAndQueuesEvents) {
  video();
  manual_clock clock;
  auto svc = media::media_service::instantiate(dir_, false, silent(clock));
  auto player = svc->open("intro", "red_green.mkv");
  player->set_rmi_id(bdg::bison::key_t{1234u});
  wish::context ctx{rmi::shared::generate_id()};
  texture_renderer r;

  ASSERT_TRUE(player->engine()->wait_buffered(5.0));
  EXPECT_TRUE(svc->pump(ctx, r));
  ASSERT_EQ(r.textures.size(), 1u);
  EXPECT_EQ(r.textures[0]->width(), 64);
  EXPECT_EQ(r.textures[0]->uploads, 1);
  EXPECT_TRUE(is_red(r.textures[0]->last));
  EXPECT_EQ(player->texture(), r.textures[0]);

  player->engine()->play();
  player->engine()->seek(1.5);
  ASSERT_TRUE(player->engine()->wait_buffered(5.0));
  svc->pump(ctx, r);
  EXPECT_EQ(r.textures.size(), 1u) << "same size: the texture is reused";
  EXPECT_TRUE(is_green(r.textures[0]->last));

  ASSERT_FALSE(ctx.pending_events.empty());
  EXPECT_EQ(ctx.pending_events[0].id, bdg::bison::key_t{1234u});
  EXPECT_EQ(ctx.pending_events[0].event_name, bdg::bison::key_t{"state_changed"});
  EXPECT_EQ(ctx.pending_events[0].payload.get_as<std::string>("state_name"_key, ""), "playing");

  svc->close("intro");
  EXPECT_EQ(player->texture(), nullptr);
  EXPECT_FALSE(svc->pump(ctx, r));
}

// ── Video widget layout ─────────────────────────────────────────────────────

TEST(VideoLayout, NaturalSizeFollowsTheVideo) {
  auto box = wish::layout_video(0, 0, ImVec2(1000, 1000), ImVec2(640, 360), true);
  EXPECT_FLOAT_EQ(box.size.x, 640);
  EXPECT_FLOAT_EQ(box.size.y, 360);
  EXPECT_FLOAT_EQ(box.image_min.x, 0);
  EXPECT_FLOAT_EQ(box.image_max.y, 360);
}

TEST(VideoLayout, UnknownVideoIs320Wide16By9) {
  auto box = wish::layout_video(0, 0, ImVec2(1000, 1000), ImVec2(0, 0), true);
  EXPECT_FLOAT_EQ(box.size.x, 320);
  EXPECT_FLOAT_EQ(box.size.y, 180);
}

TEST(VideoLayout, WidthAloneKeepsTheAspectRatio) {
  auto box = wish::layout_video(320, 0, ImVec2(1000, 1000), ImVec2(640, 480), true);
  EXPECT_FLOAT_EQ(box.size.y, 240);
}

TEST(VideoLayout, LetterboxesOrStretches) {
  // A 16:9 video in a square 200x200 box: bars above and below.
  auto box = wish::layout_video(200, 200, ImVec2(1000, 1000), ImVec2(1600, 900), true);
  EXPECT_FLOAT_EQ(box.image_min.x, 0);
  EXPECT_FLOAT_EQ(box.image_max.x, 200);
  EXPECT_NEAR(box.image_min.y, (200 - 112.5f) / 2, 1e-3);
  EXPECT_NEAR(box.image_max.y - box.image_min.y, 112.5f, 1e-3);
  // A 4:3 video in a wide box: bars left and right.
  box = wish::layout_video(400, 150, ImVec2(1000, 1000), ImVec2(640, 480), true);
  EXPECT_NEAR(box.image_max.x - box.image_min.x, 200, 1e-3);
  EXPECT_NEAR(box.image_min.x, 100, 1e-3);
  // keep_aspect off: the frame fills the box.
  box = wish::layout_video(400, 150, ImVec2(1000, 1000), ImVec2(640, 480), false);
  EXPECT_FLOAT_EQ(box.image_max.x, 400);
  EXPECT_FLOAT_EQ(box.image_max.y, 150);
}

TEST(VideoLayout, MinusOneFillsTheAvailableSpace) {
  auto box = wish::layout_video(-1, -1, ImVec2(800, 300), ImVec2(640, 360), true);
  EXPECT_FLOAT_EQ(box.size.x, 800);
  EXPECT_FLOAT_EQ(box.size.y, 300);
}

// ── Drawing: Video / Image media:// with a real imgui_renderer ─────────────

TEST_F(MediaTest, VideoAndMediaImageDrawThePlayersTexture) {
  video();
  ImGuiContext* ctx = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(800.0f, 600.0f);
  io.DeltaTime = 1.0f / 60.0f;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // like SDL3/web
  {
    wish::imgui_renderer r;
    wish::context sess{rmi::shared::generate_id()};
    manual_clock clock;
    sess.media_service = media::media_service::instantiate(dir_, false, silent(clock));
    auto player = sess.media_service->open("clip", "red_green.mkv");
    ASSERT_TRUE(player->engine()->wait_buffered(5.0));
    sess.media_service->pump(sess, r);
    ASSERT_NE(player->texture(), nullptr) << "the renderer made a dynamic texture";
    auto* tex = static_cast<ImTextureData*>(player->texture()->native_handle());
    EXPECT_EQ(tex->Width, 64);
    EXPECT_GT(static_cast<unsigned char*>(tex->GetPixelsAt(32, 24))[0], 200) << "the red frame";

    auto video_node = wish::import_json(R"({"type":"Video","player":"clip","width":128,"height":96})");
    auto image_node = wish::import_json(R"({"type":"Image","src":"media://clip","width":32,"height":24})");
    auto draws_texture = [&](ImDrawList* dl) {
      for (const auto& cmd : dl->CmdBuffer)
        if (cmd.TexRef._TexData == tex)
          return true;
      return false;
    };
    for (auto* node : {&video_node, &image_node}) {
      r.begin_frame();
      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImVec2(400, 300));
      ImGui::Begin("MediaTestWindow");
      r.render_node(*(*node)[""], sess);
      // A backend resolves _TexData's id at draw time; a raw pointer passed
      // as an id would draw an unknown (white) texture.
      EXPECT_TRUE(draws_texture(ImGui::GetWindowDrawList()));
      ImGui::End();
      r.end_frame();
    }

    sess.media_service.reset(); // closes the player: its texture is retired
    for (int i = 0; i < 3; ++i) { // ...and freed by the next frames
      r.begin_frame();
      r.end_frame();
    }
    EXPECT_TRUE(GImGui->UserTextures.empty());
  }
  ImGui::DestroyContext(ctx);
}

// ── Over RMI: wish::client ──────────────────────────────────────────────────

namespace {

// Runs @p body inside on_session().
class media_test_client : public wish::client {
 public:
  using wish::client::client;
  std::function<void(media_test_client&)> body;

 protected:
  void on_session() override {
    if (body)
      body(*this);
  }
};

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

} // namespace

TEST_F(MediaTest, ClientPlaysAnUploadedFileAndHearsItsEvents) {
  rmi::transport::memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();
  const std::string video_bytes = read_file(video());
  const std::string tone_bytes = read_file(tone(0.3));

  bool has = false;
  wish::media_status opened, after_seek;
  bool ended = false;
  std::vector<wish::media_state> states;
  std::string refused_escape, refused_missing, after_close;
  media_test_client c{transport.connect()};
  c.body = [&](media_test_client& c) {
    has = c.has_media();
    c.upload_file("clip.mkv", video_bytes).get();
    c.upload_file("beep.wav", tone_bytes).get();

    auto clip = c.open_media("clip", "clip.mkv").get();
    EXPECT_EQ(clip.name(), "clip");
    opened = clip.status().get();
    clip.seek(1.5).get();
    after_seek = clip.status().get();

    // Quiet, in case this machine has a sound card.
    wish::media_options quiet;
    quiet.volume = 0.0f;
    auto beep = c.open_media("beep", "beep.wav", quiet).get();
    std::promise<void> ended_promise;
    std::mutex states_mutex;
    beep.on_state_changed([&](wish::media_state st) {
      std::lock_guard<std::mutex> lock(states_mutex);
      states.push_back(st);
    });
    beep.on_ended([&] { ended_promise.set_value(); });
    beep.play().get();
    ended = ended_promise.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready;

    try {
      c.open_media("x", "../../etc/passwd").get();
    } catch (const std::exception& e) {
      refused_escape = e.what();
    }
    try {
      c.open_media("x", "nope.mp4").get();
    } catch (const std::exception& e) {
      refused_missing = e.what();
    }

    clip.close().get();
    try {
      clip.status().get();
    } catch (const std::exception& e) {
      after_close = e.what();
    }
  };
  c.run();
  srv.stop();

  EXPECT_TRUE(has);
  EXPECT_TRUE(opened.has_video);
  EXPECT_FALSE(opened.has_audio);
  EXPECT_EQ(opened.width, 64);
  EXPECT_EQ(opened.height, 48);
  EXPECT_NEAR(opened.duration, 2.0, 0.1);
  EXPECT_EQ(opened.state, wish::media_state::stopped);
  EXPECT_NEAR(after_seek.position, 1.5, 1e-3);

  EXPECT_TRUE(ended) << "the ended event reached the client";
  ASSERT_GE(states.size(), 2u);
  EXPECT_EQ(states.front(), wish::media_state::playing);
  EXPECT_EQ(states.back(), wish::media_state::ended);

  EXPECT_NE(refused_escape.find("sandbox"), std::string::npos) << refused_escape;
  EXPECT_NE(refused_missing.find("no such media file"), std::string::npos) << refused_missing;
  EXPECT_FALSE(after_close.empty()) << "a closed player can't be used";
}

TEST_F(MediaTest, ClientWithoutServerMediaReportsItUnavailable) {
  // No renderer: the server attaches no media service to the session.
  rmi::transport::memory_server_transport transport;
  wish::server srv{transport, nullptr};
  srv.start();
  bool has = true;
  std::string error;
  media_test_client c{transport.connect()};
  c.body = [&](media_test_client& c) {
    has = c.has_media();
    try {
      c.open_media("x", "a.mp4").get();
    } catch (const std::logic_error& e) {
      error = e.what();
    }
  };
  c.run();
  srv.stop();
  EXPECT_FALSE(has);
  EXPECT_NE(error.find("has_media"), std::string::npos) << error;
}
