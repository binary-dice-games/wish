// MIT License © 2026 Binary Dice Games
// Tests for the mpv media player form (modules/bdg/desktop/mpv/server/):
// its helpers, the client-facing RMI methods, and the events it asks the
// client half to act on.
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include "modules/bdg/desktop/mpv/server/mpv.hpp"

#include <context/context.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace fs = std::filesystem;

// ── Helpers ─────────────────────────────────────────────────────────────────

TEST(MpvHelpers, RecognizesMediaFileNames) {
  EXPECT_TRUE(wish::mpv::is_media_name("movie.mp4"));
  EXPECT_TRUE(wish::mpv::is_media_name("Song.MP3"));
  EXPECT_TRUE(wish::mpv::is_media_name("clip.webm"));
  EXPECT_TRUE(wish::mpv::is_media_name("a.b.flac"));
  EXPECT_FALSE(wish::mpv::is_media_name("notes.txt"));
  EXPECT_FALSE(wish::mpv::is_media_name("mp4"));
  EXPECT_FALSE(wish::mpv::is_media_name("trailing."));
}

TEST(MpvHelpers, FormatsTimes) {
  EXPECT_EQ(wish::mpv::format_time(0), "0:00");
  EXPECT_EQ(wish::mpv::format_time(65.9), "1:05");
  EXPECT_EQ(wish::mpv::format_time(3723), "1:02:03");
  EXPECT_EQ(wish::mpv::format_time(-3), "0:00");
}

TEST(MpvHelpers, LibraryListsMediaButNotBuiltInOrPrivateFolders) {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const fs::path root =
      fs::temp_directory_path() / ("wish_mpv_" + std::string{info->name()} + "_" +
                                   std::to_string(rmi::shared::generate_id().id));
  for (const char* d : {"media", "res/icons", "private/apps/x", "a/b/c/d/e"})
    fs::create_directories(root / d);
  for (const char* f : {"media/movie.mp4", "media/notes.txt", "song.mp3", "res/icons/beep.wav",
           "private/apps/x/secret.mp4", "a/b/c/d/e/too_deep.mp4"})
    std::ofstream(root / f) << "x";

  const auto lib = wish::mpv::scan_library(root);
  std::vector<std::string> names;
  for (const auto& [name, size] : lib)
    names.push_back(name);
  EXPECT_EQ(names, (std::vector<std::string>{"media/movie.mp4", "song.mp3"}));
  EXPECT_EQ(lib[0].second, 1u);
  fs::remove_all(root);
}

// ── Form fixture ────────────────────────────────────────────────────────────

namespace {

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::context* last_session{nullptr};
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

std::string find_form_root(const wish::name_map& objects) {
  static const std::string prefix = "__mpv_";
  for (const auto& [k, _] : objects)
    if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
      return k;
  return {};
}

dynamic listing(const std::string& path, const std::vector<std::pair<std::string, std::string>>& entries) {
  dynamic files;
  size_t i = 0;
  for (const auto& [name, type] : entries) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = name;
    (*e)["type"_key] = type;
    (*e)["size"_key] = std::string{"1 KB"};
    files[i++] = dynamic_ptr{e};
  }
  dynamic args;
  args["path"_key] = path;
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};
  return args;
}

class MpvFormTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Mpv"_key).get());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());
    handler_ = srv_->last_session->top_level_handlers.find(root_)->second;
    ASSERT_NE(handler_, nullptr);
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  const wish::ui_element_ptr& el(const std::string& path) const {
    return srv_->last_session->ui_objects.at(root_ + path);
  }
  bison::key_t id(const std::string& path) const {
    return el(path)->as<bison::key_t>("__wish_id"_key);
  }
  size_t rows(const std::string& path) const {
    auto* cf = el(path)->findField<dynamic_ptr>("children"_key);
    return cf && *cf ? (*cf)->size() : 0;
  }
  std::string text(const std::string& path) const {
    return el(path)->get_as<std::string>("text"_key, "");
  }
  void fire(const std::string& path, bison::key_t event, dynamic payload = {}) {
    handler_->on_event(id(path), event, payload);
  }
  static dynamic row(int32_t index) {
    dynamic p;
    p["index"_key] = index;
    return p;
  }
  void put_sandbox_file(const std::string& rel) {
    const fs::path p = srv_->last_session->resource_dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "not really media";
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  wish::ui_root* handler_{nullptr};
};

const std::string kLocal = ".split.files.local_table";
const std::string kLibrary = ".split.files.library_table";

} // namespace

// ── Window and RMI methods ──────────────────────────────────────────────────

TEST_F(MpvFormTest, VideoWidgetShowsTheFormsPlayer) {
  const auto player = proxy_->get().get().get_as<std::string>("player"_key, "");
  EXPECT_EQ(player.rfind("bdg.desktop.mpv.", 0), 0u) << player;
  EXPECT_EQ(el(".split.player.video")->as<std::string>("player"_key), player);
}

TEST_F(MpvFormTest, LocalListingKeepsFoldersAndMediaFilesOnly) {
  proxy_->call("update_local_listing"_key,
            listing("/home/u/Videos", {{"..", "dir"}, {"zeta.mkv", "file"}, {"notes.txt", "file"},
                                          {"Alpha", "dir"}, {"song.mp3", "file"}}))
      .get();
  EXPECT_EQ(rows(kLocal), 4u) << ".., Alpha, song.mp3, zeta.mkv";
  EXPECT_EQ(el(".split.files.path_row.path")->as<std::string>("value"_key), "/home/u/Videos");
}

TEST_F(MpvFormTest, LibraryListsSandboxMediaOnRefresh) {
  EXPECT_EQ(rows(kLibrary), 0u);
  put_sandbox_file("media/clip.mp4");
  put_sandbox_file("media/readme.txt");
  proxy_->call("refresh_library"_key, dynamic{}).get();
  EXPECT_EQ(rows(kLibrary), 1u);
  EXPECT_EQ(text(".split.files.library_actions.count"), "1 file");
}

TEST_F(MpvFormTest, UpdateStatusMovesTheSeekBarAndTime) {
  dynamic st;
  st["state"_key] = int32_t{1};
  st["position"_key] = 65.0f;
  st["duration"_key] = 3723.0f;
  st["has_video"_key] = true;
  proxy_->call("update_status"_key, std::move(st)).get();
  EXPECT_EQ(text(".split.player.controls.time"), "1:05 / 1:02:03");
  EXPECT_FLOAT_EQ(el(".split.player.seek")->as<float>("max"_key), 3723.0f);
  EXPECT_FLOAT_EQ(el(".split.player.seek")->as<float>("value"_key), 65.0f);
  EXPECT_EQ(el(".split.player.controls.play")->as<std::string>("label"_key), "&Pause") << "playing: the toggle pauses";
}

TEST_F(MpvFormTest, MediaUnavailableShowsTheBanner) {
  EXPECT_FALSE(el(".split.player.unavailable")->as<bool>("visible"_key));
  proxy_->call("media_unavailable"_key, dynamic{}).get();
  EXPECT_TRUE(el(".split.player.unavailable")->as<bool>("visible"_key));
}

TEST_F(MpvFormTest, ClientStatusIsShownAndCanBeAnError) {
  dynamic patch;
  patch["status"_key] = std::string{"Could not play x"};
  patch["status_error"_key] = true;
  proxy_->set(std::move(patch)).get();
  EXPECT_EQ(text(".split.player.status"), "Could not play x");
  EXPECT_FALSE(el(".split.player.status")->get_as<std::string>("text_color_dark"_key, "").empty());
}

// ── Events for the client half ──────────────────────────────────────────────

TEST_F(MpvFormTest, ActivatingALocalFolderNavigates) {
  proxy_->call("update_local_listing"_key, listing("/home", {{"Music", "dir"}})).get();
  const auto since = srv_->events->mark();
  fire(kLocal, "row_activated"_key, row(0));
  auto ev = srv_->events->wait_for("on_local_navigate"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "Music");
  EXPECT_EQ(ev->as<std::string>("type"_key), "dir");
}

TEST_F(MpvFormTest, ActivatingALocalFileUploadsAndPlaysIt) {
  proxy_->call("update_local_listing"_key, listing("/home/u", {{"clip.mp4", "file"}})).get();
  const auto since = srv_->events->mark();
  fire(kLocal, "row_activated"_key, row(0));
  auto ev = srv_->events->wait_for("on_upload_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("local_path"_key), "/home/u");
  EXPECT_EQ(ev->as<std::string>("dest"_key), "media");
  EXPECT_TRUE(ev->as<bool>("play"_key));
  auto names = ev->as<dynamic_ptr>("names"_key);
  ASSERT_TRUE(names);
  EXPECT_EQ((*names)[size_t{0}].as<std::string>(), "clip.mp4");
}

TEST_F(MpvFormTest, UploadWithoutSelectionAsksForOne) {
  const auto since = srv_->events->mark();
  fire(".split.files.local_actions.upload", "clicked"_key);
  EXPECT_FALSE(srv_->events->saw("on_upload_requested"_key, since));
  EXPECT_NE(text(".split.player.status").find("Select"), std::string::npos);
}

TEST_F(MpvFormTest, SelectedLocalFilesAreUploadedTogether) {
  proxy_->call("update_local_listing"_key, listing("/m", {{"a.mp3", "file"}, {"b.mp3", "file"}})).get();
  fire(kLocal, "row_selected"_key, row(0));
  dynamic ctrl = row(1);
  ctrl["ctrl"_key] = true;
  fire(kLocal, "row_selected"_key, ctrl);
  const auto since = srv_->events->mark();
  fire(".split.files.local_actions.upload", "clicked"_key);
  auto ev = srv_->events->wait_for("on_upload_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_FALSE(ev->as<bool>("play"_key));
  EXPECT_EQ(ev->as<dynamic_ptr>("names"_key)->size(), 2u);
}

TEST_F(MpvFormTest, ActivatingALibraryFileOpensItWithThePlayerSettings) {
  put_sandbox_file("media/clip.mp4");
  proxy_->call("refresh_library"_key, dynamic{}).get();
  el(".split.player.controls.volume")["value"_key] = 0.5f;
  el(".split.player.controls.loop")["value"_key] = true;
  const auto since = srv_->events->mark();
  fire(kLibrary, "row_activated"_key, row(0));
  auto ev = srv_->events->wait_for("on_open_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "media/clip.mp4");
  EXPECT_EQ(ev->as<std::string>("player"_key), el(".split.player.video")->as<std::string>("player"_key));
  EXPECT_FLOAT_EQ(ev->as<float>("volume"_key), 0.5f);
  EXPECT_TRUE(ev->as<bool>("loop"_key));
}

TEST_F(MpvFormTest, TransportControlsBecomeOnControl) {
  // Nothing is open yet: no command goes out.
  auto since = srv_->events->mark();
  fire(".split.player.controls.play", "clicked"_key);
  EXPECT_FALSE(srv_->events->saw("on_control"_key, since));

  put_sandbox_file("media/clip.mp4");
  proxy_->call("refresh_library"_key, dynamic{}).get();
  fire(kLibrary, "row_activated"_key, row(0));

  auto expect_control = [&](const std::string& path, bison::key_t event, dynamic payload, const std::string& action,
                            float value) {
    const auto mark = srv_->events->mark();
    fire(path, event, std::move(payload));
    auto ev = srv_->events->wait_for("on_control"_key, mark);
    ASSERT_TRUE(ev) << action;
    EXPECT_EQ(ev->as<std::string>("action"_key), action);
    EXPECT_FLOAT_EQ(ev->as<float>("value"_key), value) << action;
  };
  expect_control(".split.player.controls.play", "clicked"_key, {}, "play", 0.0f);
  dynamic playing;
  playing["state"_key] = int32_t{1};
  proxy_->call("update_status"_key, std::move(playing)).get();
  expect_control(".split.player.controls.play", "clicked"_key, {}, "pause", 0.0f);
  expect_control(".split.player.controls.stop", "clicked"_key, {}, "stop", 0.0f);
  dynamic v;
  v["value"_key] = 12.5f;
  expect_control(".split.player.seek", "changed"_key, v, "seek", 12.5f);
  v["value"_key] = 0.25f;
  expect_control(".split.player.controls.volume", "changed"_key, v, "volume", 0.25f);
  dynamic b;
  b["value"_key] = true;
  expect_control(".split.player.controls.loop", "changed"_key, b, "loop", 1.0f);
}

TEST_F(MpvFormTest, ClosingTheWindowEmitsClosed) {
  const auto since = srv_->events->mark();
  handler_->on_event(el("")->as<bison::key_t>("__wish_id"_key), "closed"_key, dynamic{});
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since).has_value());
}
