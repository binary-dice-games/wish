// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/session.hpp>

#include <filesystem>

using bdg::wish::session;
using namespace bdg::bison;

// ── Constructor / destructor ──────────────────────────────────────────────────

TEST(SessionTest, ConstructorCreatesResourceDir) {
  session s{"s1"_key};
  EXPECT_TRUE(std::filesystem::exists(s.resource_dir));
  EXPECT_TRUE(std::filesystem::is_directory(s.resource_dir));
}

TEST(SessionTest, DestructorRemovesResourceDir) {
  std::filesystem::path saved;
  {
    session s{"s2"_key};
    saved = s.resource_dir;
    ASSERT_TRUE(std::filesystem::exists(saved));
  }
  EXPECT_FALSE(std::filesystem::exists(saved));
}

// ── ID and path uniqueness ────────────────────────────────────────────────────

TEST(SessionTest, TwoSessionsHaveDifferentIdsAndPaths) {
  session a{"sess_a"_key};
  session b{"sess_b"_key};
  EXPECT_NE(a.id.id, b.id.id);
  EXPECT_NE(a.resource_dir, b.resource_dir);
}

// ── dirty flag ────────────────────────────────────────────────────────────────

TEST(SessionTest, DirtyDefaultsFalse) {
  session s{"s3"_key};
  EXPECT_FALSE(s.dirty.load());
}

TEST(SessionTest, DirtyCanBeSetAtomically) {
  session s{"s4"_key};
  s.dirty.store(true);
  EXPECT_TRUE(s.dirty.load());
}

