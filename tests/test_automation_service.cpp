// MIT License © 2025 Binary Dice Games
//
// End-to-end coverage for automation_service's RMI wiring -- registration
// (context::automation_service, find_singleton_service's throw-when-unset
// behavior, wish::client::on_connect()'s non-fatal resolution), and each RMI
// method forwarding correctly to an automation::automation_backend. Uses a
// fake in-memory backend and a null_renderer subclass exposing it, so this
// is purely about the RMI plumbing any automation_backend-implementing
// renderer shares -- SDL3-specific rendering mechanics (hit-test capture,
// screenshot pixel readback, real SDL event injection) are covered
// separately by test_sdl3_automation.cpp. This file needs no renderer
// backend at all (gated only on WISH_ENABLE_AUTOMATION), so it exercises the
// shared plumbing regardless of which concrete backend a build enables.
#include <gtest/gtest.h>

#include <automation/automation_backend.hpp>
#include <client/client.hpp>
#include <server/server.hpp>

#include "src/rmi/rmi.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

namespace {

// A minimal, deterministic automation_backend fake: records every injected
// input call and returns canned tree/screenshot payloads, so tests assert on
// automation_service's RMI forwarding without any real rendering.
class fake_automation_backend : public wish::automation::automation_backend {
 public:
  std::future<std::string> query_tree(uint32_t request_id, const std::string& root) override {
    last_request_id = request_id;
    last_root = root;
    std::promise<std::string> p;
    p.set_value(R"({"request_id":)" + std::to_string(request_id) + R"(,"widgets":[{"path":"ok"}]})");
    return p.get_future();
  }

  std::future<std::vector<uint8_t>> capture_screenshot() override {
    std::promise<std::vector<uint8_t>> p;
    p.set_value(std::vector<uint8_t>{0x89, 'P', 'N', 'G'});
    return p.get_future();
  }

  void inject_mouse_move(float x, float y) override {
    last_mouse_x = x;
    last_mouse_y = y;
    ++mouse_move_calls;
  }
  void inject_mouse_button(int button, bool down) override {
    last_button = button;
    last_button_down = down;
    ++mouse_button_calls;
  }
  void inject_key(int keycode, bool down) override {
    last_keycode = keycode;
    last_key_down = down;
    ++key_calls;
  }
  void inject_text(const std::string& utf8) override {
    last_text = utf8;
    ++text_calls;
  }

  uint32_t last_request_id = 0;
  std::string last_root;
  float last_mouse_x = 0.0f;
  float last_mouse_y = 0.0f;
  int mouse_move_calls = 0;
  int last_button = -1;
  bool last_button_down = false;
  int mouse_button_calls = 0;
  int last_keycode = 0;
  bool last_key_down = false;
  int key_calls = 0;
  std::string last_text;
  int text_calls = 0;
};

// null_renderer that reports a fake_automation_backend as its automation
// backend, so wish::server::on_session_created() attaches an
// automation_service the same way it would for a real sdl3_renderer.
class automation_capable_null_renderer : public wish::null_renderer {
 public:
  explicit automation_capable_null_renderer(wish::automation::automation_backend* backend) : backend_(backend) {}

  wish::automation::automation_backend* as_automation_backend() override {
    return backend_;
  }

 private:
  wish::automation::automation_backend* backend_;
};

} // namespace

// ── automation_supported() ──────────────────────────────────────────────────

class unsupported_automation_client : public wish::client {
 public:
  using wish::client::client;

  bool ran = false;
  bool supported = false;

 protected:
  void on_session() override {
    ran = true;
    supported = automation_supported();
  }
};

TEST(AutomationServiceTest, ClientReportsUnsupportedAgainstPlainRenderer) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  unsupported_automation_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.ran);
  EXPECT_FALSE(c.supported);
  srv.stop();
}

// ── RMI round trip through automation_service to a fake backend ────────────

class automation_driving_client : public wish::client {
 public:
  using wish::client::client;

  bool ran = false;
  bool supported = false;
  std::string tree_json;
  std::vector<uint8_t> screenshot_bytes;

 protected:
  void on_session() override {
    ran = true;
    supported = automation_supported();
    if (!supported)
      return;
    tree_json = get_automation_tree("").get();
    screenshot_bytes = take_screenshot().get();
    inject_mouse_move(3.0f, 4.0f).get();
    inject_mouse_button(1, true).get();
    inject_key(42, false).get();
    inject_text("hello").get();
  }
};

TEST(AutomationServiceTest, ClientDrivesAutomationAgainstBackendSupportedRenderer) {
  fake_automation_backend backend;
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<automation_capable_null_renderer>(&backend)};
  srv.start();

  automation_driving_client c{transport.connect()};
  c.run();

  ASSERT_TRUE(c.ran);
  EXPECT_TRUE(c.supported);

  auto j = nlohmann::json::parse(c.tree_json);
  ASSERT_FALSE(j["widgets"].empty());
  EXPECT_EQ(j["widgets"][0]["path"], "ok");

  ASSERT_GE(c.screenshot_bytes.size(), 4u);
  EXPECT_EQ(c.screenshot_bytes[0], 0x89);

  EXPECT_EQ(backend.mouse_move_calls, 1);
  EXPECT_FLOAT_EQ(backend.last_mouse_x, 3.0f);
  EXPECT_FLOAT_EQ(backend.last_mouse_y, 4.0f);
  EXPECT_EQ(backend.mouse_button_calls, 1);
  EXPECT_EQ(backend.last_button, 1);
  EXPECT_TRUE(backend.last_button_down);
  EXPECT_EQ(backend.key_calls, 1);
  EXPECT_EQ(backend.last_keycode, 42);
  EXPECT_FALSE(backend.last_key_down);
  EXPECT_EQ(backend.text_calls, 1);
  EXPECT_EQ(backend.last_text, "hello");

  srv.stop();
}
