// MIT License © 2025 Binary Dice Games
// Tests for app/wish_cli/desktop/wish_desktop_app.hpp -- desktop shell chrome.
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include <server/server.hpp>

#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <chrono>

// wish_desktop_app.cpp DECLAREs these (DEFINEd in main.cpp / standalone_main.cpp
// for the real binaries, neither of which this test links) and DEFINEs the
// upstream_* flags itself -- mirrors extern/bison/tests/bridge_app_tests.cpp.
DEFINE_string(cmd, "", "test default");
DEFINE_bool(verbose, false, "test default");
DEFINE_bool(debugger, false, "test default");
DEFINE_int32(timeout, 30000, "test default");

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

namespace {

// Captures the wish::context of the single session wish_desktop opens on the
// upstream server, so tests can inspect the chrome elements it created.
class tracking_upstream_server : public wish::server {
 public:
  tracking_upstream_server(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

// Fixture: a real `wish::server` (upstream) reachable via in-process memory
// transports, and a `wish_desktop` bridging a (never-connected) downstream
// memory transport to it.
class WishDesktopTest : public ::testing::Test {
 protected:
  memory_server_transport upstream_transport_;
  tracking_upstream_server upstream_srv_{upstream_transport_, std::make_unique<wish::null_renderer>()};

  memory_server_transport downstream_transport_;
  wish::wish_desktop desktop_{downstream_transport_, std::make_unique<memory_client_transport>(upstream_transport_.connect())};

  void SetUp() override {
    upstream_srv_.start();
    desktop_.start();
  }

  void TearDown() override {
    desktop_.stop();
    upstream_srv_.stop();
  }
};

} // namespace

TEST_F(WishDesktopTest, BuildChromeCreatesMenuBarExtensionMenu) {
  desktop_.build_chrome();

  ASSERT_NE(upstream_srv_.last_session, nullptr);
  const wish::context& sess = *upstream_srv_.last_session;

  // Root MenuBarExtension is registered as a top-level renderable object --
  // spliced into the server's own chrome menu bar rather than opening a
  // competing dockspace/menu bar of its own.
  EXPECT_EQ(sess.top_level_objects.size(), size_t{1});

  EXPECT_TRUE(sess.ui_objects.count("m_file"));
  EXPECT_TRUE(sess.ui_objects.count("m_file.mi_quit"));
}

TEST_F(WishDesktopTest, BuildChromeIsIdempotent) {
  desktop_.build_chrome();
  desktop_.build_chrome();
  desktop_.build_chrome();

  ASSERT_NE(upstream_srv_.last_session, nullptr);
  // Only the first call should have registered/instantiated the template --
  // a second top-level object would appear if it ran twice.
  EXPECT_EQ(upstream_srv_.last_session->top_level_objects.size(), size_t{1});
}

TEST_F(WishDesktopTest, QuitClickInvokesRequestQuit) {
  desktop_.build_chrome();

  ASSERT_NE(upstream_srv_.last_session, nullptr);
  wish::context& sess = *upstream_srv_.last_session;

  bdg::bison::key_t quit_id{0u};
  sess.ui_objects.with(
      "m_file.mi_quit",
      [&](const wish::ui_element_ptr& elem) {
        quit_id = elem->get_as<bdg::bison::key_t>("__wish_id"_key, bdg::bison::key_t{});
      });
  ASSERT_NE(quit_id.id, 0u);

  // Simulate the renderer's enqueue_event() for a Quit click by emitting
  // directly through the session, the same way the server drains a pending
  // "clicked" event -- exercises the same onEvent registration/dispatch path
  // build_chrome() wires up.
  ASSERT_TRUE(sess.emit_event);
  sess.emit_event(quit_id, "clicked"_key, dynamic{});

  // wait_for_quit() blocks until request_quit() fires; a hang here means the
  // "clicked" handler didn't call it.
  desktop_.wait_for_quit();
}

TEST_F(WishDesktopTest, WaitForQuitForTimesOutThenReturnsTrueAfterRequestQuit) {
  // Before request_quit() is called, wait_for_quit_for() must time out and
  // report false -- this is what lets wish_desktop_app::wait_for_shutdown()
  // poll an active_term_ for exit alongside the Quit menu item instead of
  // blocking on wait_for_quit() forever.
  EXPECT_FALSE(desktop_.wait_for_quit_for(std::chrono::milliseconds{20}));

  desktop_.request_quit();

  EXPECT_TRUE(desktop_.wait_for_quit_for(std::chrono::milliseconds{20}));
}
