// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <client.hpp>
#include <server.hpp>
#include <ui_descriptor.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <stdexcept>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Test helpers ──────────────────────────────────────────────────────────────

class tracking_client : public wish::client {
 public:
  using wish::client::client;

  std::atomic<bool> session_fired{false};

 protected:
  void on_session() override {
    session_fired.store(true, std::memory_order_release);
  }
};

// Minimal JSON descriptor helpers used across tests.
static const char* kWindowJson = R"({"type":"Window","title":"Root"})";

static const char* kWindowWithChildJson =
    R"({"type":"Window","title":"Root","children":{"lbl":{"type":"Label","text":"Hi"}}})";

// ── run() lifecycle ───────────────────────────────────────────────────────────

TEST(ClientTest, RunCallsOnSessionAndDisconnects) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  tracking_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.session_fired.load(std::memory_order_acquire));
  srv.stop();
}

TEST(ClientTest, RunDisconnectsOnException) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class throwing_client : public wish::client {
   public:
    using wish::client::client;

   protected:
    void on_session() override {
      throw std::runtime_error("test error");
    }
  };

  throwing_client c{transport.connect()};
  EXPECT_THROW(c.run(), std::runtime_error);
  srv.stop();
}

// ── Inherited bison RMI operations ───────────────────────────────────────────

TEST(ClientTest, InheritedInstantiateWorks) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class rmi_client : public wish::client {
   public:
    using wish::client::client;

   protected:
    void on_session() override {}
  };

  rmi_client c{transport.connect()};
  c.connect();
  auto proxy = c.instantiate("wish"_key, "Window"_key).get();
  EXPECT_TRUE(proxy.valid());
  EXPECT_NE(proxy.id(), 0u);
  c.disconnect();
  srv.stop();
}

TEST(ClientTest, InheritedSetGetRoundTrips) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class rmi_client : public wish::client {
   public:
    using wish::client::client;

   protected:
    void on_session() override {}
  };

  rmi_client c{transport.connect()};
  c.connect();

  auto proxy = c.instantiate("wish"_key, "Window"_key).get();
  dynamic fields;
  fields["title"_key] = std::string{"ClientTest"};
  EXPECT_TRUE(proxy.set(std::move(fields)).get());
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "ClientTest");

  c.disconnect();
  srv.stop();
}

// ── register_template + instantiate_template ─────────────────────────────────

TEST(ClientTest, InstantiateReturnsNonEmptyMap) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowJson)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_FALSE(c.result.empty());
  srv.stop();
}

TEST(ClientTest, InstantiateRootProxyIsValid) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowJson)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  ASSERT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.at("").valid());
  srv.stop();
}

TEST(ClientTest, InstantiateNamedChildrenAppearInMap) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowWithChildJson)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.count("lbl"));
  if (c.result.count("lbl")) {
    EXPECT_TRUE(c.result.at("lbl").valid());
  }
  srv.stop();
}

TEST(ClientTest, InstantiateInvalidDescriptorThrows) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    bool threw{false};

   protected:
    void on_session() override {
      try {
        register_template("tpl"_key, wish::import_descriptor_json("{invalid json}")).get();
        instantiate_template("tpl"_key).get();
      } catch (const std::exception&) {
        threw = true;
      }
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.threw);
  srv.stop();
}

TEST(ClientTest, TemplateCanBeInstantiatedMultipleTimes) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map first;
    wish::proxy_map second;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowWithChildJson)).get();
      first = instantiate_template("tpl"_key).get();
      second = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.first.count("") && c.first.count("lbl"));
  EXPECT_TRUE(c.second.count("") && c.second.count("lbl"));
  srv.stop();
}

// A hand-built bison::dynamic descriptor, with no JSON/YAML text at all --
// proves register_template's argument is a real dynamic tree, not just a
// convenience wrapper around text parsing.
TEST(ClientTest, RegisterTemplateAcceptsHandBuiltDescriptor) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      dynamic desc;
      desc["__type__"_key] = "Window"_key;
      desc["title"_key] = std::string{"Hand-built"};

      register_template("tpl"_key, std::move(desc)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  ASSERT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.at("").valid());
  srv.stop();
}

TEST(ClientTest, InstantiateUnregisteredTemplateThrows) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    bool threw{false};

   protected:
    void on_session() override {
      try {
        instantiate_template("no_such_template"_key).get();
      } catch (const std::exception&) {
        threw = true;
      }
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.threw);
  srv.stop();
}

// ── upload_file / download_file ───────────────────────────────────────────────

TEST(ClientTest, UploadDownloadRoundTrips) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string downloaded;

   protected:
    void on_session() override {
      upload_file("hello.txt", "world").get();
      downloaded = download_file("hello.txt").get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_EQ(c.downloaded, "world");
  srv.stop();
}
