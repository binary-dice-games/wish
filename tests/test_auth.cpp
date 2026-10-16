// MIT License © 2025 Binary Dice Games
//
// End-to-end tests for the optional auth module hook and persistent sandbox
// directories described in src/auth/DESIGN.md and src/context/DESIGN.md:
// local_auth_module identity extraction, the client's OS-user default,
// identity normalization, set_persistent_sandboxes() gating, and the
// path-escape guard in server::on_authenticated.
#include <gtest/gtest.h>

#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <context/os_user.hpp>
#include <context/wish_home.hpp>
#include <server/server.hpp>

#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

namespace {

// Connects with a given set of connect params, optionally uploads one file,
// optionally attempts to download one file, then disconnects. Outcomes are
// exposed as public members so the TEST body can assert after run() returns.
class auth_test_client : public wish::client {
 public:
  using wish::client::client;

  dynamic connect_params;
  std::string upload_name;
  std::string upload_data;
  std::string download_name;

  bool download_ok{false};
  std::string downloaded;
  std::string download_error;

  /// Runs at the end of on_session(), while still connected.
  std::function<void(auth_test_client&)> while_connected;

 protected:
  void on_session() override {
    if (!upload_name.empty())
      upload_file(upload_name, upload_data).get();
    if (!download_name.empty()) {
      try {
        downloaded = download_file(download_name).get();
        download_ok = true;
      } catch (const std::exception& e) {
        download_error = e.what();
      }
    }
    if (while_connected)
      while_connected(*this);
  }
};

dynamic params_with_username(const std::string& username) {
  dynamic p;
  if (!username.empty())
    p["username"_key] = username;
  return p;
}

dynamic anonymous_params() {
  dynamic p;
  p["anonymous"_key] = true;
  return p;
}

} // namespace

class AuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
        ("wish_auth_test_" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()} + "_" +
         std::to_string(static_cast<uint32_t>(bdg::bison::rmi::shared::generate_id().id)));
    std::filesystem::create_directories(root_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
};

TEST_F(AuthTest, NamelessClientConnectsAsTheOsUser) {
  const std::string user = wish::normalize_identity(wish::os_user_name());
  if (user.empty())
    GTEST_SKIP() << "the OS user name is unknown or not a valid identity";
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "keep.txt";
    c.upload_data = "kept";
    c.run(dynamic{}); // no username: wish::client::run() sends the OS user
  }
  EXPECT_TRUE(std::filesystem::exists(root_ / "users" / user / "sandbox" / "keep.txt"));
  srv.stop();
}

TEST_F(AuthTest, IdentityIsNormalizedToOneProfile) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "note.txt";
    c.upload_data = "hi";
    c.run(params_with_username("Alice"));
  }
  EXPECT_TRUE(std::filesystem::exists(root_ / "users" / "alice" / "sandbox" / "note.txt"));
  {
    auth_test_client c{transport.connect()};
    c.download_name = "note.txt";
    c.run(params_with_username("alice"));
    EXPECT_TRUE(c.download_ok) << c.download_error;
  }
  srv.stop();
}

TEST_F(AuthTest, PersistentSandboxesDefaultOffOnlyForNetworkTransports) {
  memory_server_transport memory;
  wish::server local_srv{memory, std::make_unique<wish::null_renderer>()};
  EXPECT_TRUE(local_srv.persistent_sandboxes());

  socket_server_transport tcp{"0.0.0.0", 0};
  wish::server network_srv{tcp, std::make_unique<wish::null_renderer>()};
  EXPECT_FALSE(network_srv.persistent_sandboxes());
  network_srv.set_persistent_sandboxes(true);
  EXPECT_TRUE(network_srv.persistent_sandboxes());

  EXPECT_TRUE(wish::is_loopback_host("127.0.0.1"));
  EXPECT_TRUE(wish::is_loopback_host("localhost"));
  EXPECT_TRUE(wish::is_loopback_host("::1"));
  EXPECT_FALSE(wish::is_loopback_host("0.0.0.0"));
  EXPECT_FALSE(wish::is_loopback_host("192.168.1.2"));
}

TEST_F(AuthTest, UploadPersistsAcrossReconnectWithSameIdentity) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "note.txt";
    c.upload_data = "hello alice";
    c.run(params_with_username("alice"));
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "note.txt";
    c.run(params_with_username("alice"));
    EXPECT_TRUE(c.download_ok) << c.download_error;
    EXPECT_EQ(c.downloaded, "hello alice");
  }

  srv.stop();
}

TEST_F(AuthTest, DifferentIdentityDoesNotSeeAnotherIdentitysFiles) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "secret.txt";
    c.upload_data = "alice-only";
    c.run(params_with_username("alice"));
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "secret.txt";
    c.run(params_with_username("bob"));
    EXPECT_FALSE(c.download_ok) << "bob should not see alice's file";
  }

  srv.stop();
}

TEST_F(AuthTest, NoIdentityFallsBackToNonPersistentTempDir) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    // anonymous: no "username" field at all -- identity stays empty.
    c.upload_name = "temp.txt";
    c.upload_data = "temp-data";
    c.run(anonymous_params());
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "temp.txt";
    c.run(anonymous_params());
    EXPECT_FALSE(c.download_ok) << "no-identity sessions must not persist across reconnects";
  }
  EXPECT_FALSE(std::filesystem::exists(root_ / "users")) << "anonymous sessions write nothing under the home";

  srv.stop();
}

TEST_F(AuthTest, PathEscapingIdentityIsRejectedAndDoesNotPersist) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "escape.txt";
    c.upload_data = "should-not-persist";
    c.run(params_with_username("../evil"));
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "escape.txt";
    c.run(params_with_username("../evil"));
    EXPECT_FALSE(c.download_ok) << "a path-escaping identity must not get a persistent directory";
  }
  // The rejected identity must never have created a directory that escapes root_.
  EXPECT_FALSE(std::filesystem::exists(root_.parent_path() / "evil"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "evil"));
  EXPECT_FALSE(std::filesystem::exists(root_ / "users"));

  srv.stop();
}

TEST_F(AuthTest, PersistentSandboxesOffKeepsATempSandbox) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.set_persistent_sandboxes(false);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "note.txt";
    c.upload_data = "hello";
    c.run(params_with_username("alice"));
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "note.txt";
    c.run(params_with_username("alice"));
    EXPECT_FALSE(c.download_ok) << "persistence must stay off with set_persistent_sandboxes(false)";
  }

  srv.stop();
}

TEST_F(AuthTest, NoAuthModuleBehavesUnchanged) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(); // no auth_module argument

  {
    auth_test_client c{transport.connect()};
    c.upload_name = "note.txt";
    c.upload_data = "hello";
    c.run(params_with_username("alice"));
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "note.txt";
    c.run(params_with_username("alice"));
    EXPECT_FALSE(c.download_ok) << "persistence must stay off without an auth module";
  }

  srv.stop();
}

// ── Per-tool temp dirs in a persistent sandbox ──────────────────────────────

TEST_F(AuthTest, ToolTempDirIsPrivatePerSessionAndRemovedAtSessionEnd) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_home(root_);
  srv.start(std::make_shared<wish::local_auth_module>());
  const auto sandbox = root_ / "users" / "alice" / "sandbox";

  std::string first_dir;
  {
    auth_test_client c{transport.connect()};
    c.upload_name = "shared.txt"; // a file at the shared root
    c.upload_data = "shared";
    c.while_connected = [&](auth_test_client& self) {
      first_dir = self.create_temp_dir("bdg/desktop/nano").get();
      self.upload_file(first_dir + "/0/notes.txt", "scratch").get();
      // A second session of the same identity, connected at the same time,
      // gets its own directory.
      auth_test_client other{transport.connect()};
      std::string other_dir;
      other.while_connected = [&](auth_test_client& o) { other_dir = o.create_temp_dir("bdg/desktop/nano").get(); };
      other.run(params_with_username("alice"));
      EXPECT_NE(other_dir, first_dir);
      EXPECT_EQ(other_dir.rfind("private/apps/bdg.desktop.nano/tmp/", 0), 0u) << other_dir;
      // The other session ending does not remove this session's directory.
      EXPECT_TRUE(std::filesystem::exists(sandbox / first_dir / "0" / "notes.txt"));
    };
    c.run(params_with_username("alice"));
  }
  EXPECT_EQ(first_dir.rfind("private/apps/bdg.desktop.nano/tmp/", 0), 0u) << first_dir;

  // Session teardown runs on the server after the client disconnects.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::filesystem::exists(sandbox / first_dir) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(std::filesystem::exists(sandbox / first_dir));
  EXPECT_TRUE(std::filesystem::exists(sandbox / "shared.txt"));
  srv.stop();
}

TEST_F(AuthTest, CreateTempDirRejectsMalformedAppName) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  bool threw = false;
  {
    auth_test_client c{transport.connect()};
    c.while_connected = [&](auth_test_client& self) {
      try {
        self.create_temp_dir("../escape").get();
      } catch (const std::exception&) {
        threw = true;
      }
    };
    c.run(dynamic{});
  }
  EXPECT_TRUE(threw);
  srv.stop();
}
