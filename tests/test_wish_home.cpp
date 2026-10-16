// MIT License © 2026 Binary Dice Games
//
// Unit tests for the wish home layout (src/context/wish_home.hpp), identity
// normalization, the OS user default identity and its client-side
// application (see src/context/DESIGN.md).
#include <client/client.hpp>
#include <context/os_user.hpp>
#include <context/wish_home.hpp>

#include "src/rmi/shared/ids.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace wish = bdg::wish;

namespace {

std::filesystem::path test_dir() {
  return std::filesystem::temp_directory_path() /
      ("wish_home_test_" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()} + "_" +
       std::to_string(static_cast<uint32_t>(rmi::shared::generate_id().id)));
}

} // namespace

// ── normalize_identity ───────────────────────────────────────────────────────

TEST(NormalizeIdentity, LowercasesAndMapsSpaces) {
  EXPECT_EQ(wish::normalize_identity("Alice"), "alice");
  EXPECT_EQ(wish::normalize_identity("John Smith"), "john_smith");
  EXPECT_EQ(wish::normalize_identity("dev-1.test_x"), "dev-1.test_x");
}

TEST(NormalizeIdentity, RejectsWhatCannotNameADirectory) {
  EXPECT_EQ(wish::normalize_identity(""), "");
  EXPECT_EQ(wish::normalize_identity("../evil"), "");
  EXPECT_EQ(wish::normalize_identity("a/b"), "");
  EXPECT_EQ(wish::normalize_identity("a\\b"), "");
  EXPECT_EQ(wish::normalize_identity("a..b"), "");
  EXPECT_EQ(wish::normalize_identity(".hidden"), "");
  EXPECT_EQ(wish::normalize_identity("c:drive"), "");
  EXPECT_EQ(wish::normalize_identity("jos\xc3\xa9"), ""); // non-ASCII
  EXPECT_EQ(wish::normalize_identity(std::string(65, 'a')), "");
  EXPECT_EQ(wish::normalize_identity(std::string(64, 'a')), std::string(64, 'a'));
}

// ── wish_home ────────────────────────────────────────────────────────────────

TEST(WishHome, DerivesTheDocumentedLayout) {
  const std::filesystem::path root{"/srv/wishhome"};
  wish::wish_home home{root};
  EXPECT_EQ(home.root(), root);
  EXPECT_EQ(home.server_store_file(), root / "server" / "store.bison");
  EXPECT_EQ(home.user_dir("Alice"), root / "users" / "alice");
  EXPECT_EQ(home.user_store_file("alice"), root / "users" / "alice" / "store.bison");
  EXPECT_EQ(home.user_sandbox_dir("alice"), root / "users" / "alice" / "sandbox");
  EXPECT_EQ(home.user_desktop_config("alice"), root / "users" / "alice" / "desktop" / "desktop.yaml");
  EXPECT_EQ(home.logs_dir(), root / "logs");
  EXPECT_EQ(home.screenshots_dir(), root / "screenshots");
  EXPECT_EQ(home.traces_dir(), root / "traces");
}

TEST(WishHome, InvalidIdentityHasNoProfile) {
  wish::wish_home home{"/srv/wishhome"};
  EXPECT_TRUE(home.user_dir("../evil").empty());
  EXPECT_TRUE(home.user_store_file("").empty());
  EXPECT_TRUE(home.user_sandbox_dir("a/b").empty());
  EXPECT_TRUE(home.user_desktop_config(".x").empty());
}

TEST(WishHome, DefaultRootComesFromWishHomeEnv) {
  // tests/wish_test_main.cpp sets WISH_HOME for every test process.
  const char* env = std::getenv("WISH_HOME");
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(wish::wish_home::default_root(), std::filesystem::path{env});
  EXPECT_EQ(wish::wish_home{}.root(), std::filesystem::path{env});
  EXPECT_EQ(wish::wish_home{std::filesystem::path{}}.root(), std::filesystem::path{env});
}

TEST(WishHome, ListIdentitiesReturnsNormalizedProfileDirectories) {
  const auto root = test_dir();
  std::filesystem::create_directories(root / "users" / "bob");
  std::filesystem::create_directories(root / "users" / "alice");
  std::filesystem::create_directories(root / "users" / "Not Normalized");
  std::filesystem::create_directories(root / "users");
  { std::ofstream(root / "users" / "carol.bison") << ""; }
  EXPECT_EQ(wish::wish_home{root}.list_identities(), (std::vector<std::string>{"alice", "bob"}));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  EXPECT_TRUE(wish::wish_home{root}.list_identities().empty());
}

// ── Default identity ─────────────────────────────────────────────────────────

TEST(OsUser, NameHasNoDomainPart) {
  const std::string user = wish::os_user_name();
  EXPECT_EQ(user.find('\\'), std::string::npos);
  EXPECT_EQ(user.find('@'), std::string::npos);
}

TEST(DefaultIdentity, AddsTheOsUserOnlyWhenNoneIsGiven) {
  const std::string user = wish::os_user_name();
  if (user.empty())
    GTEST_SKIP() << "the OS user name is unknown";

  dynamic none;
  wish::client::apply_default_identity(none);
  EXPECT_EQ(none.as<std::string>("username"_key), user);

  dynamic named;
  named["username"_key] = std::string{"alice"};
  wish::client::apply_default_identity(named);
  EXPECT_EQ(named.as<std::string>("username"_key), "alice");

  dynamic anonymous;
  anonymous["anonymous"_key] = true;
  wish::client::apply_default_identity(anonymous);
  EXPECT_EQ(anonymous.findField("username"_key), nullptr);
}
