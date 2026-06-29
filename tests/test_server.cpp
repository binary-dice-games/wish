// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Test server subclasses ────────────────────────────────────────────────────

class tracking_server : public wish::server {
 public:
  tracking_server(server_transport_iface& t, std::unique_ptr<wish::renderer> r) : wish::server(t, std::move(r)) {}

  std::atomic<int> created_count{0};
  std::atomic<int> destroyed_count{0};
  wish::session* last_session{nullptr};

 protected:
  void on_session_created(wish::session& s) override {
    last_session = &s;
    created_count.fetch_add(1, std::memory_order_release);
  }
  void on_session_destroyed(wish::session& s) override {
    (void)s;
    destroyed_count.fetch_add(1, std::memory_order_release);
  }
};

class multi_tracking_server : public wish::server {
 public:
  multi_tracking_server(server_transport_iface& t, std::unique_ptr<wish::renderer> r) : wish::server(t, std::move(r)) {}

  std::atomic<int> created_count{0};
  std::mutex sessions_mutex;
  std::vector<wish::session*> sessions;

 protected:
  void on_session_created(wish::session& s) override {
    std::lock_guard<std::mutex> lk(sessions_mutex);
    sessions.push_back(&s);
    created_count.fetch_add(1, std::memory_order_release);
  }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TEST(ServerTest, StartStopDoesNotHang) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();
  srv.stop();
}

// ── Session lifecycle ─────────────────────────────────────────────────────────

TEST(ServerTest, ClientConnectTriggersOnSessionCreated) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();
    // on_session_created fires during accept() — before connect() returns
    EXPECT_EQ(srv.created_count.load(std::memory_order_acquire), 1);
    EXPECT_NE(srv.last_session, nullptr);
    c.disconnect();
  }

  srv.stop();
}

TEST(ServerTest, ClientDisconnectTriggersOnSessionDestroyed) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();
    c.disconnect();
  }

  // stop() joins all bison workers; on_session_destroyed has fired by then
  srv.stop();
  EXPECT_EQ(srv.destroyed_count.load(std::memory_order_acquire), 1);
}

// ── RMI operations ────────────────────────────────────────────────────────────

TEST(ServerTest, InstantiateWindowSucceeds) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();

    auto proxy = c.instantiate("wish"_key, "Window"_key).get();
    EXPECT_TRUE(proxy.valid());
    EXPECT_NE(proxy.id(), 0u);

    c.disconnect();
  }

  srv.stop();
}

TEST(ServerTest, SetAppliesFieldValue) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();

    auto proxy = c.instantiate("wish"_key, "Window"_key).get();

    dynamic fields;
    fields["title"_key] = std::string{"Hello"};
    bool ok = proxy.set(std::move(fields)).get();
    EXPECT_TRUE(ok);

    // Verify the field was actually applied by reading it back.
    auto snapshot = proxy.get().get();
    EXPECT_EQ(snapshot.as<std::string>("title"_key), "Hello");

    c.disconnect();
  }

  srv.stop();
}

// ── Multiple clients ──────────────────────────────────────────────────────────

TEST(ServerTest, TwoClientsGetSeparateSessions) {
  memory_server_transport transport;
  multi_tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c1{transport.connect()};
    client c2{transport.connect()};

    c1.connect();
    c2.connect();

    EXPECT_EQ(srv.created_count.load(std::memory_order_acquire), 2);

    {
      std::lock_guard<std::mutex> lk(srv.sessions_mutex);
      ASSERT_EQ(srv.sessions.size(), 2u);
      EXPECT_NE(srv.sessions[0], srv.sessions[1]);
      EXPECT_NE(srv.sessions[0]->id, srv.sessions[1]->id);
    }

    c1.disconnect();
    c2.disconnect();
  }

  srv.stop();
}
