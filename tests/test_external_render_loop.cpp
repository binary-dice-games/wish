// MIT License © 2026 Binary Dice Games
/**
 * @file test_external_render_loop.cpp
 * @brief wish::server with set_external_render_loop(true): the host drives
 *        frames with run_frame() on its own thread (the browser build's
 *        layout) instead of start() spawning a render thread.
 */
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>

#include "src/rmi/transport/memory_transport.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

namespace {

// Records which thread each lifecycle hook ran on, counts frames, and asks
// to quit once `quit_after` frames have been drawn.
class recording_renderer : public wish::renderer {
 public:
  struct record {
    std::atomic<int> setups{0};
    std::atomic<int> teardowns{0};
    std::atomic<int> polls{0};
    std::thread::id setup_thread;
    std::thread::id poll_thread;
    std::thread::id teardown_thread;
    int quit_after{-1};
  };

  explicit recording_renderer(std::shared_ptr<record> rec) : rec_(std::move(rec)) {}

  void setup() override {
    rec_->setup_thread = std::this_thread::get_id();
    rec_->setups.fetch_add(1);
  }
  void teardown() override {
    rec_->teardown_thread = std::this_thread::get_id();
    rec_->teardowns.fetch_add(1);
  }
  bool poll_events() override {
    rec_->poll_thread = std::this_thread::get_id();
    rec_->polls.fetch_add(1);
    return false;
  }
  bool should_quit() const override {
    return rec_->quit_after >= 0 && rec_->polls.load() >= rec_->quit_after;
  }
  void begin_frame() override {}
  void render_node(const wish::ui_element&, const wish::context&) override {}
  void end_frame() override {}

 private:
  std::shared_ptr<record> rec_;
};

} // namespace

// ── External render loop ──────────────────────────────────────────────────────

TEST(ExternalRenderLoopTest, StartDoesNotRunTheRendererOnAnotherThread) {
  auto rec = std::make_shared<recording_renderer::record>();
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<recording_renderer>(rec)};
  srv.set_external_render_loop(true);
  ASSERT_TRUE(srv.external_render_loop());
  srv.start();
  // No render thread: nothing touches the renderer until run_frame().
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_EQ(rec->setups.load(), 0);
  EXPECT_EQ(rec->polls.load(), 0);
  srv.stop();
  EXPECT_EQ(rec->teardowns.load(), 0); // never set up, so never torn down
}

TEST(ExternalRenderLoopTest, RunFrameDrivesTheRendererOnTheCallingThread) {
  auto rec = std::make_shared<recording_renderer::record>();
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<recording_renderer>(rec)};
  srv.set_external_render_loop(true);
  srv.start();

  for (int i = 0; i < 3; ++i)
    ASSERT_TRUE(srv.run_frame());
  EXPECT_EQ(rec->setups.load(), 1);
  EXPECT_EQ(rec->polls.load(), 3);
  EXPECT_EQ(rec->setup_thread, std::this_thread::get_id());
  EXPECT_EQ(rec->poll_thread, std::this_thread::get_id());

  srv.stop();
  EXPECT_EQ(rec->teardowns.load(), 1);
  EXPECT_EQ(rec->teardown_thread, std::this_thread::get_id());
  EXPECT_FALSE(srv.run_frame());
  EXPECT_EQ(rec->teardowns.load(), 1); // not torn down twice
}

TEST(ExternalRenderLoopTest, RunFrameReturnsFalseAndTearsDownOnceTheRendererQuits) {
  auto rec = std::make_shared<recording_renderer::record>();
  rec->quit_after = 2;
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<recording_renderer>(rec)};
  srv.set_external_render_loop(true);
  srv.start();

  int frames = 0;
  while (srv.run_frame() && frames < 100)
    ++frames;
  EXPECT_EQ(frames, 1); // the second iteration sees should_quit()
  EXPECT_TRUE(srv.should_quit());
  EXPECT_EQ(rec->teardowns.load(), 1);
  srv.stop();
  EXPECT_EQ(rec->teardowns.load(), 1);
}

TEST(ExternalRenderLoopTest, RunFrameThrowsWithoutExternalMode) {
  auto rec = std::make_shared<recording_renderer::record>();
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<recording_renderer>(rec)};
  srv.start();
  EXPECT_THROW(srv.run_frame(), std::logic_error);
  srv.stop();
}

TEST(ExternalRenderLoopTest, DefaultModeStillRunsItsOwnRenderThread) {
  auto rec = std::make_shared<recording_renderer::record>();
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<recording_renderer>(rec)};
  srv.start();
  for (int i = 0; i < 200 && rec->polls.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  srv.stop();
  EXPECT_EQ(rec->setups.load(), 1);
  EXPECT_GT(rec->polls.load(), 0);
  EXPECT_NE(rec->setup_thread, std::this_thread::get_id());
  EXPECT_EQ(rec->teardowns.load(), 1);
}
