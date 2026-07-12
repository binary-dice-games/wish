// MIT License © 2025 Binary Dice Games
//
// End-to-end tests for the optional auth module hook and persistent sandbox
// directories described in src/auth/DESIGN.md: local_auth_module identity
// extraction, set_persistent_sandbox_root gating, and the path-escape guard
// in server::on_authenticated.
#include <gtest/gtest.h>

#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <server/server.hpp>

#include "src/rmi/rmi.hpp"

#include <filesystem>
#include <memory>
#include <string>

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
  }
};

dynamic params_with_username(const std::string& username) {
  dynamic p;
  if (!username.empty())
    p["username"_key] = username;
  return p;
}

} // namespace

class AuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
        ("wish_auth_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(root_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
};

TEST_F(AuthTest, UploadPersistsAcrossReconnectWithSameIdentity) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_persistent_sandbox_root(root_);
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
  srv.set_persistent_sandbox_root(root_);
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
  srv.set_persistent_sandbox_root(root_);
  srv.start(std::make_shared<wish::local_auth_module>());

  {
    auth_test_client c{transport.connect()};
    // No "username" field at all -- local_auth_module leaves identity empty.
    c.upload_name = "temp.txt";
    c.upload_data = "temp-data";
    c.run(dynamic{});
  }
  {
    auth_test_client c{transport.connect()};
    c.download_name = "temp.txt";
    c.run(dynamic{});
    EXPECT_FALSE(c.download_ok) << "no-identity sessions must not persist across reconnects";
  }

  srv.stop();
}

TEST_F(AuthTest, PathEscapingIdentityIsRejectedAndDoesNotPersist) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_persistent_sandbox_root(root_);
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

  srv.stop();
}

TEST_F(AuthTest, NoPersistentRootConfiguredBehavesUnchangedEvenWithAuthModule) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  // set_persistent_sandbox_root() intentionally not called.
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
    EXPECT_FALSE(c.download_ok) << "persistence must stay off without set_persistent_sandbox_root()";
  }

  srv.stop();
}

TEST_F(AuthTest, NoAuthModuleBehavesUnchanged) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.set_persistent_sandbox_root(root_);
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
