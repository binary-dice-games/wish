// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <context.hpp>

#include <filesystem>

using bdg::wish::context;
using namespace bdg::bison;

// ── Constructor / destructor ──────────────────────────────────────────────────

TEST(ContextTest, ConstructorCreatesResourceDir) {
  context s{"s1"_key};
  EXPECT_TRUE(std::filesystem::exists(s.resource_dir));
  EXPECT_TRUE(std::filesystem::is_directory(s.resource_dir));
}

TEST(ContextTest, DestructorRemovesResourceDir) {
  std::filesystem::path saved;
  {
    context s{"s2"_key};
    saved = s.resource_dir;
    ASSERT_TRUE(std::filesystem::exists(saved));
  }
  EXPECT_FALSE(std::filesystem::exists(saved));
}

// ── ID and path uniqueness ────────────────────────────────────────────────────

TEST(ContextTest, TwoSessionsHaveDifferentIdsAndPaths) {
  context a{"sess_a"_key};
  context b{"sess_b"_key};
  EXPECT_NE(a.session_id.id, b.session_id.id);
  EXPECT_NE(a.resource_dir, b.resource_dir);
}

// ── dirty flag ────────────────────────────────────────────────────────────────

TEST(ContextTest, DirtyDefaultsTrue) {
  // A freshly created session must render at least once, so the render loop
  // does not treat it as idle before any RMI dispatch has run.
  context s{"s3"_key};
  EXPECT_TRUE(s.dirty.load());
}

TEST(ContextTest, DirtyCanBeSetAtomically) {
  context s{"s4"_key};
  s.dirty.store(true);
  EXPECT_TRUE(s.dirty.load());
}

// ── embedded resources ────────────────────────────────────────────────────────

TEST(ContextTest, ResourceDirContainsEmbeddedAssets) {
  context s{"s5"_key};
  auto icon = s.resource_dir / "res" / "icons/folder.png";
  auto font = s.resource_dir / "res" / "fonts/default.ttf";
  ASSERT_TRUE(std::filesystem::exists(icon));
  ASSERT_TRUE(std::filesystem::exists(font));
  EXPECT_GT(std::filesystem::file_size(icon), 0U);
  EXPECT_GT(std::filesystem::file_size(font), 0U);
}

TEST(ContextTest, ResourceDirEmbeddedFilesAreReadOnly) {
  context s{"s6"_key};
  auto icon = s.resource_dir / "res" / "icons/folder.png";
  ASSERT_TRUE(std::filesystem::exists(icon));
  EXPECT_EQ(std::filesystem::status(icon).permissions() & std::filesystem::perms::owner_write,
            std::filesystem::perms::none);
}

TEST(ContextTest, EmbeddedCrc32sPopulatedWithResPrefixedKeys) {
  context s{"s7"_key};
  auto it = s.embedded_crc32s.find("res/icons/folder.png");
  ASSERT_NE(it, s.embedded_crc32s.end());
  EXPECT_NE(it->second, 0U);
}
