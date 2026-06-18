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

// ── Move semantics ────────────────────────────────────────────────────────────

TEST(SessionTest, MoveTransfersResourceDirOwnership) {
  std::filesystem::path saved;
  {
    session a{"s5"_key};
    saved = a.resource_dir;
    ASSERT_TRUE(std::filesystem::exists(saved));

    session b = std::move(a);

    // Moved-from: resource_dir is empty → its destructor will not delete.
    EXPECT_TRUE(a.resource_dir.empty());
    // New owner: resource_dir still points to the live directory.
    EXPECT_EQ(b.resource_dir, saved);
    EXPECT_TRUE(std::filesystem::exists(saved));
  }  // b goes out of scope here and deletes saved.
  EXPECT_FALSE(std::filesystem::exists(saved));
}

TEST(SessionTest, MoveAssignmentTransfersOwnership) {
  std::filesystem::path saved;
  std::filesystem::path b_original;

  {
    session b{"s6"_key};
    b_original = b.resource_dir;

    session a{"s7"_key};
    saved = a.resource_dir;
    ASSERT_TRUE(std::filesystem::exists(saved));

    b = std::move(a);

    // The old b directory should have been deleted by move-assign.
    EXPECT_FALSE(std::filesystem::exists(b_original));
    // a is now empty — its destructor will not delete the directory.
    EXPECT_TRUE(a.resource_dir.empty());
    // b now owns saved.
    EXPECT_EQ(b.resource_dir, saved);
  }  // b goes out of scope here → deletes saved.
  EXPECT_FALSE(std::filesystem::exists(saved));
}
