// MIT License © 2025 Binary Dice Games
//
// End-to-end tests for the persistent server and user stores (see
// docs/persistent-store.md): a client reads/writes its own user store over
// RMI, identities are isolated, entries persist across sessions and server
// restarts, anonymous sessions have no user store, and the server store is
// reachable only from server-side code.
#include <gtest/gtest.h>

#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <context/persistent_store.hpp>
#include <server/server.hpp>
#include <standalone/standalone.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

namespace {

// Runs @p body inside on_session() so the test can drive the client API
// while connected; run() disconnects afterwards.
class store_test_client : public wish::client {
 public:
  using wish::client::client;

  std::function<void(store_test_client&)> body;

 protected:
  void on_session() override {
    if (body)
      body(*this);
  }
};

dynamic params_with_username(const std::string& username) {
  dynamic p;
  if (!username.empty())
    p["username"_key] = username;
  return p;
}

dynamic make_value(const std::string& text) {
  dynamic d;
  d["text"_key] = text;
  return d;
}

// Records what server-side code sees on each session, from the
// session-destroyed hook (by then on_authenticated() has run).
class recording_server : public wish::server {
 public:
  using wish::server::server;

  std::atomic<bool> saw_server_store{false};
  std::atomic<bool> saw_user_store{false};

 protected:
  void on_session_destroyed(wish::context& s) override {
    saw_server_store = s.server_store != nullptr;
    saw_user_store = s.user_store != nullptr;
  }
};

class UserStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string{"wish_user_store_test_"} + info->name());
    std::filesystem::remove_all(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  // Runs one client session as @p username (empty: anonymous).
  static void run_as(
      memory_server_transport& transport,
      const std::string& username,
      std::function<void(store_test_client&)> body) {
    store_test_client c{transport.connect()};
    c.body = std::move(body);
    c.run(params_with_username(username));
  }

  std::filesystem::path dir_;
};

} // namespace

// ── Client access ────────────────────────────────────────────────────────────

TEST_F(UserStoreTest, IdentifiedClientCanSetGetEraseAndListEntries) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());

  bool has_store = false;
  std::optional<dynamic> got;
  std::optional<dynamic> missing;
  std::vector<std::string> keys;
  bool erased = false;
  bool erased_again = true;
  run_as(transport, "alice", [&](store_test_client& c) {
    has_store = c.has_user_store();
    c.user_store_set("bdg.desktop.tail", make_value("error|warn")).get();
    c.user_store_set("other", make_value("x")).get();
    got = c.user_store_get("bdg.desktop.tail").get();
    missing = c.user_store_get("nope").get();
    keys = c.user_store_keys().get();
    erased = c.user_store_erase("other").get();
    erased_again = c.user_store_erase("other").get();
  });
  srv.stop();

  EXPECT_TRUE(has_store);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->as<std::string>("text"_key), "error|warn");
  EXPECT_FALSE(missing.has_value());
  EXPECT_EQ(keys, (std::vector<std::string>{"bdg.desktop.tail", "other"}));
  EXPECT_TRUE(erased);
  EXPECT_FALSE(erased_again);
  EXPECT_TRUE(std::filesystem::exists(dir_ / "users" / "alice.bison"));
}

TEST_F(UserStoreTest, InvalidEntryNameIsReportedToTheClient) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());

  bool threw = false;
  run_as(transport, "alice", [&](store_test_client& c) {
    try {
      c.user_store_set("", make_value("x")).get();
    } catch (const std::exception&) {
      threw = true;
    }
  });
  srv.stop();
  EXPECT_TRUE(threw);
}

// ── Persistence and isolation ────────────────────────────────────────────────

TEST_F(UserStoreTest, EntriesPersistAcrossSessionsAndServerRestarts) {
  {
    memory_server_transport transport;
    wish::server srv{transport, std::make_unique<wish::null_renderer>()};
    srv.set_store_dir(dir_);
    srv.start(std::make_shared<wish::local_auth_module>());
    run_as(transport, "alice", [](store_test_client& c) { c.user_store_set("k", make_value("v1")).get(); });
    std::optional<dynamic> next_session;
    run_as(transport, "alice", [&](store_test_client& c) { next_session = c.user_store_get("k").get(); });
    ASSERT_TRUE(next_session.has_value());
    EXPECT_EQ(next_session->as<std::string>("text"_key), "v1");
    srv.stop();
  }
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());
  std::optional<dynamic> after_restart;
  run_as(transport, "alice", [&](store_test_client& c) { after_restart = c.user_store_get("k").get(); });
  srv.stop();
  ASSERT_TRUE(after_restart.has_value());
  EXPECT_EQ(after_restart->as<std::string>("text"_key), "v1");
}

TEST_F(UserStoreTest, OtherIdentityCannotSeeEntries) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());

  run_as(transport, "alice", [](store_test_client& c) { c.user_store_set("secret", make_value("alice")).get(); });
  std::optional<dynamic> bob_get;
  std::vector<std::string> bob_keys{"sentinel"};
  run_as(transport, "bob", [&](store_test_client& c) {
    bob_get = c.user_store_get("secret").get();
    bob_keys = c.user_store_keys().get();
  });
  srv.stop();

  EXPECT_FALSE(bob_get.has_value());
  EXPECT_TRUE(bob_keys.empty());
}

TEST_F(UserStoreTest, ConcurrentSessionsOfOneIdentityShareTheStore) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());

  std::vector<std::string> keys;
  run_as(transport, "alice", [&](store_test_client& outer) {
    outer.user_store_set("from_outer", make_value("1")).get();
    run_as(transport, "alice", [](store_test_client& inner) {
      inner.user_store_set("from_inner", make_value("2")).get();
    });
    keys = outer.user_store_keys().get();
  });
  srv.stop();
  EXPECT_EQ(keys, (std::vector<std::string>{"from_inner", "from_outer"}));
}

// ── Anonymous sessions ───────────────────────────────────────────────────────

TEST_F(UserStoreTest, AnonymousSessionHasNoUserStore) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());

  bool has_store = true;
  bool get_threw_logic_error = false;
  run_as(transport, "", [&](store_test_client& c) {
    has_store = c.has_user_store();
    try {
      c.user_store_get("k").get();
    } catch (const std::logic_error&) {
      get_threw_logic_error = true;
    }
  });
  srv.stop();

  EXPECT_FALSE(has_store);
  EXPECT_TRUE(get_threw_logic_error);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "users"));
}

TEST_F(UserStoreTest, NoAuthModuleMeansNoUserStore) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(); // no auth module: every session is anonymous

  bool has_store = true;
  run_as(transport, "alice", [&](store_test_client& c) { has_store = c.has_user_store(); });
  srv.stop();
  EXPECT_FALSE(has_store);
}

TEST_F(UserStoreTest, UnsafeIdentityGetsNoUserStore) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());

  bool has_store = true;
  run_as(transport, "../evil", [&](store_test_client& c) { has_store = c.has_user_store(); });
  srv.stop();
  EXPECT_FALSE(has_store);
}

// ── Server store ─────────────────────────────────────────────────────────────

TEST_F(UserStoreTest, ServerStoreIsServerSideOnly) {
  memory_server_transport transport;
  recording_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_store_dir(dir_);
  srv.start(std::make_shared<wish::local_auth_module>());
  srv.server_store().set("server.secret", make_value("server-only"));

  std::vector<std::string> client_keys{"sentinel"};
  bool instantiate_threw = false;
  run_as(transport, "alice", [&](store_test_client& c) {
    client_keys = c.user_store_keys().get();
    // No RMI class exposes the server store.
    try {
      c.instantiate("wish"_key, "__WishServerStore"_key).get();
    } catch (const std::exception&) {
      instantiate_threw = true;
    }
  });
  srv.stop();

  EXPECT_TRUE(client_keys.empty());
  EXPECT_TRUE(instantiate_threw);
  EXPECT_TRUE(srv.saw_server_store);
  EXPECT_TRUE(srv.saw_user_store);
  EXPECT_TRUE(std::filesystem::exists(dir_ / "server_store.bison"));

  wish::persistent_store reopened{dir_ / "server_store.bison"};
  EXPECT_TRUE(reopened.contains("server.secret"));
}

// ── Standalone host ──────────────────────────────────────────────────────────

TEST_F(UserStoreTest, StandaloneWithIdentityHasPersistentUserStore) {
  {
    wish::standalone sa{std::make_unique<wish::null_renderer>()};
    sa.set_store_dir(dir_);
    sa.set_user_identity("carol");
    sa.start();
    ASSERT_TRUE(sa.has_user_store());
    sa.user_store_set("k", make_value("standalone")).get();
    sa.stop();
  }
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.set_store_dir(dir_);
  sa.set_user_identity("carol");
  sa.start();
  auto value = sa.user_store_get("k").get();
  auto keys = sa.user_store_keys().get();
  sa.stop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->as<std::string>("text"_key), "standalone");
  EXPECT_EQ(keys, (std::vector<std::string>{"k"}));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "users" / "carol.bison"));
}

TEST_F(UserStoreTest, StandaloneWithoutIdentityIsAnonymous) {
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.set_store_dir(dir_);
  sa.start();
  EXPECT_FALSE(sa.has_user_store());
  EXPECT_THROW(sa.user_store_keys().get(), std::logic_error);
  sa.stop();
}

// ── wish_app_host defaults ───────────────────────────────────────────────────

namespace {
class minimal_host : public wish::wish_app_host {
 public:
  std::future<rmi::proxy::dynamic> instantiate(bdg::bison::key_t, bdg::bison::key_t, dynamic) override {
    throw std::logic_error("unused");
  }
  std::future<void> upload_file(const std::string&, const std::string&, wish::transfer_progress_callback) override {
    throw std::logic_error("unused");
  }
  std::future<std::string> download_file(const std::string&, wish::transfer_progress_callback) override {
    throw std::logic_error("unused");
  }
  void keep_alive(rmi::proxy::dynamic&&) override {}
  void signal_done() override {}
  const std::vector<std::string>& app_args() const override {
    return args_;
  }
  bool read_console_line(std::string&) override {
    return false;
  }

 private:
  std::vector<std::string> args_;
};
} // namespace

TEST(WishAppHostUserStore, DefaultsReportUnavailable) {
  minimal_host host;
  EXPECT_FALSE(host.has_user_store());
  EXPECT_THROW(host.user_store_get("k").get(), std::logic_error);
  EXPECT_THROW(host.user_store_set("k", dynamic{}).get(), std::logic_error);
  EXPECT_THROW(host.user_store_erase("k").get(), std::logic_error);
  EXPECT_THROW(host.user_store_keys().get(), std::logic_error);
}
