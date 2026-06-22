// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/client.hpp>
#include <wish/server.hpp>

#include "src/rmi/rmi.hpp"

#include <stdexcept>
#include <string>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Fixture ───────────────────────────────────────────────────────────────────

// Thin RAII wrapper: starts a wish server over in-memory transport and stops
// it in TearDown so every test gets a clean server with no shared state.
class HandlersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<wish::server>(
        transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
  }

  void TearDown() override {
    srv_->stop();
    srv_.reset();
  }

  memory_server_transport transport_;
  std::unique_ptr<wish::server> srv_;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static const char* kWindowJson =
    R"({"type":"Window","title":"Root"})";

static const char* kWindowWithLabelJson = R"({
  "type": "Window",
  "title": "Root",
  "children": {
    "lbl": { "type": "Label", "text": "Hello" }
  }
})";

static const char* kWindowWithLabelYaml = R"(
type: Window
title: Root
children:
  lbl:
    type: Label
    text: Hello
)";

// ── Test: register + instantiate returns valid proxies ────────────────────────

TEST_F(HandlersTest, RegisterThenInstantiateReturnsValidProxies) {
  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, kWindowJson).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport_.connect()};
  c.run();

  ASSERT_FALSE(c.result.empty());
  ASSERT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.at("").valid());
}

// ── Test: instantiate unregistered template throws ────────────────────────────

TEST_F(HandlersTest, InstantiateUnregisteredTemplateThrows) {
  class test_client : public wish::client {
   public:
    using wish::client::client;
    bool threw{false};

   protected:
    void on_session() override {
      try {
        instantiate_template("no_such"_key).get();
      } catch (const std::exception&) {
        threw = true;
      }
    }
  };

  test_client c{transport_.connect()};
  c.run();

  EXPECT_TRUE(c.threw);
}

// ── Test: proxy.get() on named child returns correct field values ─────────────

TEST_F(HandlersTest, ProxyGetOnNamedChildReturnsCorrectFieldValues) {
  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string lbl_text;
    bool got_field{false};

   protected:
    void on_session() override {
      register_template("tpl"_key, kWindowWithLabelJson).get();
      auto pm = instantiate_template("tpl"_key).get();

      auto it = pm.find("lbl");
      ASSERT_NE(it, pm.end()) << "named child 'lbl' not in proxy map";

      auto snapshot = it->second.get().get();
      const auto* tf = snapshot.findField("text"_key);
      if (tf && tf->is<std::string>()) {
        lbl_text = tf->as<std::string>();
        got_field = true;
      }
    }
  };

  test_client c{transport_.connect()};
  c.run();

  ASSERT_TRUE(c.got_field);
  EXPECT_EQ(c.lbl_text, "Hello");
}

// ── Test: YAML descriptor produces the same field values as JSON ──────────────

TEST_F(HandlersTest, YamlDescriptorProducesCorrectFieldValues) {
  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string lbl_text;
    bool got_field{false};

   protected:
    void on_session() override {
      register_template("tpl"_key, kWindowWithLabelYaml).get();
      auto pm = instantiate_template("tpl"_key).get();

      auto it = pm.find("lbl");
      ASSERT_NE(it, pm.end()) << "named child 'lbl' not in proxy map";

      auto snapshot = it->second.get().get();
      const auto* tf = snapshot.findField("text"_key);
      if (tf && tf->is<std::string>()) {
        lbl_text = tf->as<std::string>();
        got_field = true;
      }
    }
  };

  test_client c{transport_.connect()};
  c.run();

  ASSERT_TRUE(c.got_field);
  EXPECT_EQ(c.lbl_text, "Hello");
}
