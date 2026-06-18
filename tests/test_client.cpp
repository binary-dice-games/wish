// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/client.hpp>
#include <wish/server.hpp>

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
    void on_session() override { throw std::runtime_error("test error"); }
  };

  throwing_client c{transport.connect()};
  EXPECT_THROW(c.run(), std::runtime_error);
  // If disconnect() was not called, srv.stop() would hang — verify it doesn't.
  srv.stop();
}

// ── Inherited bison RMI operations ───────────────────────────────────────────

// wish::client inherits bison::rmi::client directly, so subclasses have full
// access to connect/disconnect/instantiate/describe/send_request without any
// extra surface.

TEST(ClientTest, InheritedInstantiateWorks) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class rmi_client : public wish::client {
   public:
    using wish::client::client;

    proxy::dynamic make_window() {
      return instantiate("wish"_key, "Window"_key).get();
    }

   protected:
    void on_session() override {}
  };

  rmi_client c{transport.connect()};
  c.connect();

  auto proxy = c.make_window();
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
