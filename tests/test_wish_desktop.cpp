// MIT License © 2025 Binary Dice Games
// Tests for app/wish_cli/desktop/wish_desktop_app.hpp -- desktop shell chrome.
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include <server/server.hpp>

#include "src/rmi/rmi.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

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

TEST_F(WishDesktopTest, BuildChromeCreatesDockspaceMenuAndClock) {
  desktop_.build_chrome();

  ASSERT_NE(upstream_srv_.last_session, nullptr);
  const wish::context& sess = *upstream_srv_.last_session;

  // Root DockSpaceViewport is registered as a top-level renderable object.
  EXPECT_EQ(sess.top_level_objects.size(), size_t{1});

  EXPECT_TRUE(sess.ui_objects.count("main_menu"));
  EXPECT_TRUE(sess.ui_objects.count("main_menu.m_file"));
  EXPECT_TRUE(sess.ui_objects.count("main_menu.m_file.mi_quit"));
  EXPECT_TRUE(sess.ui_objects.count("main_menu.clock"));
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

TEST_F(WishDesktopTest, ClockLabelTextTicks) {
  desktop_.build_chrome();
  ASSERT_NE(upstream_srv_.last_session, nullptr);

  // Give the clock thread's first iteration time to run, then check the
  // label was stamped with an "HH:MM:SS"-shaped string (avoids asserting on
  // an exact value or diffing two snapshots, which would be flaky around
  // second boundaries).
  std::this_thread::sleep_for(std::chrono::milliseconds{200});

  std::string text;
  upstream_srv_.last_session->ui_objects.with(
      "main_menu.clock", [&](const wish::ui_element_ptr& elem) { text = elem->get_as<std::string>("text"_key, ""); });

  ASSERT_EQ(text.size(), size_t{8});
  EXPECT_EQ(text[2], ':');
  EXPECT_EQ(text[5], ':');
}
