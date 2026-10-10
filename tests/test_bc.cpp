// MIT License © 2026 Binary Dice Games
/// @file test_bc.cpp
/// @brief Tests for the bc (calculator) form: its keypad drives the display
///        through common::tool_form's click dispatch.
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <memory>
#include <optional>
#include <string>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

} // namespace

class BcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Bc"_key).get());
    ASSERT_TRUE(proxy_->valid());
    for (const auto& [k, _] : srv_->last_session->ui_objects)
      if (k.rfind("__calc_", 0) == 0 && k.find('.') == std::string::npos)
        root_ = k;
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  // Clicks the keypad button at @p path (e.g. "row1.n7").
  void press(const std::string& path) {
    auto it = srv_->last_session->ui_objects.find(root_ + "." + path);
    ASSERT_NE(it, srv_->last_session->ui_objects.end()) << path;
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(it->second->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});
  }

  std::string display() const {
    return srv_->last_session->ui_objects.at(root_ + ".display")->as<std::string>("text"_key);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bison::rmi::client> client_;
  std::optional<bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
};

TEST_F(BcTest, KeypadComputesASum) {
  EXPECT_EQ(display(), "0");
  press("row1.n7");
  press("row2.add");
  press("row3.n1");
  EXPECT_EQ(display(), "1");
  press("row3.eq");
  EXPECT_EQ(display(), "8");
}

TEST_F(BcTest, ClearResetsTheDisplay) {
  press("row1.n9");
  press("row1.n8");
  EXPECT_EQ(display(), "98");
  press("row0.c");
  EXPECT_EQ(display(), "0");
}
