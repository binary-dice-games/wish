// MIT License © 2026 Binary Dice Games
// Tests for server-side audio playback (src/media/): the miniaudio decoder,
// the playback state machine, the per-session media service and its sandbox
// rules, and the client API over RMI.
#include <gtest/gtest.h>

#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <context/context.hpp>
#include <media/audio_decoder.hpp>
#include <media/media_service.hpp>
#include <media/playback.hpp>
#include <server/registry.hpp>
#include <server/renderer.hpp>
#include <server/server.hpp>

#include "src/bison/bison_common.hpp"

#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <chrono>
#include <cmath>
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

/// @brief Writes a 16-bit PCM WAV of @p frames frames; every channel of
///        frame @p i holds @p sample(i).
void write_wav(const fs::path& path, uint32_t rate, uint16_t channels, uint32_t frames,
    const std::function<int16_t(uint32_t)>& sample) {
  const uint32_t data_bytes = frames * channels * 2;
  std::ofstream out(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  out.write("RIFF", 4);
  u32(36 + data_bytes);
  out.write("WAVEfmt ", 8);
  u32(16);
  u16(1); // PCM
  u16(channels);
  u32(rate);
  u32(rate * channels * 2);
  u16(static_cast<uint16_t>(channels * 2));
  u16(16);
  out.write("data", 4);
  u32(data_bytes);
  for (uint32_t i = 0; i < frames; ++i)
    for (uint16_t c = 0; c < channels; ++c)
      u16(static_cast<uint16_t>(sample(i)));
}

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

  /// A quiet 440 Hz stereo 48 kHz sine tone of @p seconds, as "tone.wav".
  fs::path tone(double seconds = 1.0) {
    auto p = dir_ / "tone.wav";
    if (!fs::exists(p))
      write_wav(p, 48000, 2, static_cast<uint32_t>(seconds * 48000), [](uint32_t i) {
        return static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000) * 8000.0);
      });
    return p;
  }

  /// 2 s of stereo 48 kHz whose sample i is `ramp_value(i)`, as "ramp.wav":
  /// the value at a position tells where the decoder is.
  fs::path ramp() {
    auto p = dir_ / "ramp.wav";
    if (!fs::exists(p))
      write_wav(p, 48000, 2, 96000, [](uint32_t i) { return static_cast<int16_t>(i % 30000); });
    return p;
  }
  static float ramp_value(uint32_t frame) {
    return static_cast<float>(frame % 30000) / 32768.0f;
  }

  fs::path dir_;
};

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
std::vector<media::playback_event> advance_until(
    media::playback& pb, const manual_clock& clock, Pred done, double max_seconds = 5.0) {
  std::vector<media::playback_event> all;
  for (double t = 0; t < max_seconds && !done(); t += 0.02) {
    pb.wait_buffered(2.0);
    clock.advance(0.02);
    for (auto& e : pb.update())
      all.push_back(e);
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

// ── audio_decoder ───────────────────────────────────────────────────────────

TEST_F(MediaTest, DecoderReadsAWavInChunks) {
  media::audio_decoder d{tone(0.5)};
  EXPECT_NEAR(d.info().duration, 0.5, 1e-3);
  media::audio_chunk a;
  size_t samples = 0;
  double expected_pts = 0.0;
  while (d.decode(a)) {
    EXPECT_EQ(a.samples.size() % media::kAudioChannels, 0u);
    EXPECT_LE(a.samples.size(), media::kAudioChunkFrames * media::kAudioChannels);
    EXPECT_NEAR(a.pts, expected_pts, 1e-6) << "chunks follow each other";
    expected_pts += static_cast<double>(a.samples.size() / media::kAudioChannels) / media::kAudioRate;
    samples += a.samples.size();
  }
  EXPECT_TRUE(a.samples.empty());
  EXPECT_EQ(samples, 24000u * media::kAudioChannels);
}

TEST_F(MediaTest, DecoderConvertsMonoAndOtherRatesToStereo48k) {
  // 1 s of mono 22.05 kHz: comes out as 1 s of stereo 48 kHz.
  write_wav(dir_ / "mono.wav", 22050, 1, 22050, [](uint32_t) { return int16_t{16384}; });
  media::audio_decoder d{dir_ / "mono.wav"};
  EXPECT_NEAR(d.info().duration, 1.0, 0.01);
  media::audio_chunk a;
  size_t frames = 0;
  float left = 0, right = 0;
  while (d.decode(a)) {
    frames += a.samples.size() / media::kAudioChannels;
    left = a.samples[0];
    right = a.samples[1];
  }
  EXPECT_NEAR(static_cast<double>(frames) / media::kAudioRate, 1.0, 0.01);
  EXPECT_NEAR(left, 0.5f, 0.05f) << "mono goes to both channels";
  EXPECT_NEAR(right, left, 1e-4f);
}

TEST_F(MediaTest, DecoderSeeksSampleAccurately) {
  media::audio_decoder d{ramp()};
  EXPECT_NEAR(d.info().duration, 2.0, 1e-3);
  d.seek(0.5);
  media::audio_chunk a;
  ASSERT_TRUE(d.decode(a));
  EXPECT_NEAR(a.pts, 0.5, 1e-6);
  EXPECT_NEAR(a.samples[0], ramp_value(24000), 1e-4f);
  d.seek(0.0);
  ASSERT_TRUE(d.decode(a));
  EXPECT_NEAR(a.pts, 0.0, 1e-6);
  EXPECT_NEAR(a.samples[2], ramp_value(1), 1e-4f);
}

TEST_F(MediaTest, DecoderRejectsFilesThatAreNotAudio) {
  std::ofstream(dir_ / "notes.txt") << "just some text, not audio\n";
  EXPECT_THROW(media::audio_decoder{dir_ / "notes.txt"}, std::runtime_error);
  std::ofstream(dir_ / "movie.mp4") << std::string(4096, '\x42');
  EXPECT_THROW(media::audio_decoder{dir_ / "movie.mp4"}, std::runtime_error);
  EXPECT_THROW(media::audio_decoder{dir_ / "missing.mp3"}, std::runtime_error);
}

// ── playback ────────────────────────────────────────────────────────────────

TEST_F(MediaTest, PlaybackStartsStoppedAndBuffersAhead) {
  manual_clock clock;
  media::playback pb{tone(2.0), silent(clock)};
  EXPECT_EQ(pb.state(), media::play_state::stopped);
  EXPECT_NEAR(pb.info().duration, 2.0, 1e-3);
  ASSERT_TRUE(pb.wait_buffered(5.0));
  EXPECT_TRUE(pb.update().empty());
  EXPECT_DOUBLE_EQ(pb.position(), 0.0);
}

TEST_F(MediaTest, PlaybackFollowsTheClockAndSeeks) {
  manual_clock clock;
  media::playback pb{tone(2.0), silent(clock)};
  pb.play();
  EXPECT_EQ(pb.state(), media::play_state::playing);
  auto events = pb.update();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].what, media::playback_event::kind::state_changed);
  EXPECT_EQ(events[0].state, media::play_state::playing);

  advance_until(pb, clock, [&] { return pb.position() >= 0.5; });
  EXPECT_NEAR(pb.position(), 0.5, 0.03);

  pb.pause();
  const double paused_at = pb.position();
  clock.advance(1.0);
  EXPECT_DOUBLE_EQ(pb.position(), paused_at) << "time stands still while paused";
  EXPECT_EQ(pb.state(), media::play_state::paused);

  pb.seek(1.5);
  EXPECT_DOUBLE_EQ(pb.position(), 1.5);
  ASSERT_TRUE(pb.wait_buffered(5.0));
  EXPECT_EQ(pb.state(), media::play_state::paused);

  pb.stop();
  EXPECT_EQ(pb.state(), media::play_state::stopped);
  EXPECT_DOUBLE_EQ(pb.position(), 0.0);
}

TEST_F(MediaTest, PlaybackEndsOrLoops) {
  manual_clock clock;
  media::playback pb{tone(1.0), silent(clock)};
  pb.play();
  auto events = advance_until(pb, clock, [&] { return pb.state() == media::play_state::ended; });
  EXPECT_EQ(pb.state(), media::play_state::ended);
  EXPECT_TRUE(has_event(events, media::playback_event::kind::ended));
  EXPECT_NEAR(pb.position(), 1.0, 0.05);

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
  media::playback pb{tone(2.0), silent(clock)};
  pb.set_volume(3.0f);
  EXPECT_FLOAT_EQ(pb.volume(), 1.0f);
  pb.set_volume(-1.0f);
  EXPECT_FLOAT_EQ(pb.volume(), 0.0f);
  pb.seek(99.0);
  EXPECT_NEAR(pb.position(), 2.0, 1e-3);
  pb.seek(-5.0);
  EXPECT_DOUBLE_EQ(pb.position(), 0.0);
}

// ── media_service ───────────────────────────────────────────────────────────

TEST_F(MediaTest, ServiceOpensOnlyFilesInTheSandbox) {
  tone();
  media::playback_options quiet;
  quiet.audio_device = false;
  auto svc = media::media_service::instantiate(dir_, false, quiet);
  EXPECT_NO_THROW(svc->open("intro", "tone.wav"));
  EXPECT_NE(svc->find("intro"), nullptr);

  EXPECT_THROW(svc->open("x", "../tone.wav"), std::invalid_argument);
  EXPECT_THROW(svc->open("x", (dir_ / "tone.wav").string()), std::invalid_argument) << "absolute";
  EXPECT_THROW(svc->open("x", "https://example.com/a.mp3"), std::invalid_argument);
  EXPECT_THROW(svc->open("x", "file:///etc/passwd"), std::invalid_argument);
  EXPECT_THROW(svc->open("x", "nope.mp3"), std::invalid_argument);
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
  EXPECT_FALSE(st.get_as<bool>("audio_output"_key, true));
  EXPECT_EQ(st.findField("has_video"_key), nullptr) << "audio only: no video fields";
  EXPECT_EQ(st.findField("width"_key), nullptr);
}

TEST_F(MediaTest, PumpQueuesPlayerEventsForTheClient) {
  tone();
  manual_clock clock;
  auto svc = media::media_service::instantiate(dir_, false, silent(clock));
  auto player = svc->open("intro", "tone.wav");
  player->set_rmi_id(bdg::bison::key_t{1234u});
  wish::context ctx{rmi::shared::generate_id()};

  EXPECT_FALSE(svc->pump(ctx)) << "nothing happened yet";
  player->engine()->play();
  EXPECT_TRUE(svc->pump(ctx));
  ASSERT_EQ(ctx.pending_events.size(), 1u);
  EXPECT_EQ(ctx.pending_events[0].id, bdg::bison::key_t{1234u});
  EXPECT_EQ(ctx.pending_events[0].event_name, bdg::bison::key_t{"state_changed"});
  EXPECT_EQ(ctx.pending_events[0].payload.get_as<std::string>("state_name"_key, ""), "playing");

  svc->close("intro");
  EXPECT_FALSE(svc->pump(ctx));
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
  write_wav(dir_ / "song.wav", 44100, 2, 88200, [](uint32_t i) { return static_cast<int16_t>(i % 1000); });
  const std::string song_bytes = read_file(dir_ / "song.wav");
  const std::string tone_bytes = read_file(tone(0.3));

  bool has = false;
  wish::media_status opened, after_seek;
  bool ended = false;
  std::vector<wish::media_state> states;
  std::string refused_escape, refused_missing, after_close;
  media_test_client c{transport.connect()};
  c.body = [&](media_test_client& c) {
    has = c.has_media();
    c.upload_file("media/song.wav", song_bytes).get();
    c.upload_file("beep.wav", tone_bytes).get();

    auto clip = c.open_media("clip", "media/song.wav").get();
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
      c.open_media("x", "nope.mp3").get();
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
  EXPECT_NEAR(opened.duration, 2.0, 0.01);
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
      c.open_media("x", "a.mp3").get();
    } catch (const std::logic_error& e) {
      error = e.what();
    }
  };
  c.run();
  srv.stop();
  EXPECT_FALSE(has);
  EXPECT_NE(error.find("has_media"), std::string::npos) << error;
}
