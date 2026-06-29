// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/resource_store.hpp>

using bdg::wish::resource_store::find;
using bdg::wish::resource_store::is_resource_path;
using bdg::wish::resource_store::strip_scheme;

// ── is_resource_path ──────────────────────────────────────────────────────────

TEST(ResourceStore, IsResourcePathReturnsTrueForResScheme) {
  EXPECT_TRUE(is_resource_path("res://icons/folder.png"));
  EXPECT_TRUE(is_resource_path("res://"));
}

TEST(ResourceStore, IsResourcePathReturnsFalseForBareAndFilePaths) {
  EXPECT_FALSE(is_resource_path("icons/folder.png"));
  EXPECT_FALSE(is_resource_path("file://icons/folder.png"));
  EXPECT_FALSE(is_resource_path(""));
  EXPECT_FALSE(is_resource_path("/absolute/path.png"));
}

// ── strip_scheme ──────────────────────────────────────────────────────────────

TEST(ResourceStore, StripSchemeRemovesResPrefix) {
  EXPECT_EQ(strip_scheme("res://icons/folder.png"), "icons/folder.png");
  EXPECT_EQ(strip_scheme("res://fonts/default.ttf"), "fonts/default.ttf");
  EXPECT_EQ(strip_scheme("res://"), "");
}

TEST(ResourceStore, StripSchemeLeavesBarePathUnchanged) {
  EXPECT_EQ(strip_scheme("icons/folder.png"), "icons/folder.png");
  EXPECT_EQ(strip_scheme(""), "");
  EXPECT_EQ(strip_scheme("file://foo.png"), "file://foo.png");
}

// ── find — miss ───────────────────────────────────────────────────────────────

TEST(ResourceStore, FindReturnsNulloptForUnknownPath) {
  EXPECT_EQ(find("no/such/file.png"), std::nullopt);
  EXPECT_EQ(find(""), std::nullopt);
  EXPECT_EQ(find("res://icons/folder.png"), std::nullopt); // prefix not stripped
}

// ── find — hit ────────────────────────────────────────────────────────────────

TEST(ResourceStore, FindReturnsDataForKnownIcon) {
  auto result = find("icons/folder.png");
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->data.empty());
}

TEST(ResourceStore, FindReturnsDataForKnownFont) {
  auto result = find("fonts/default.ttf");
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->data.empty());
}

TEST(ResourceStore, FindCoversAllPlannedIcons) {
  for (const char* path : {
           "icons/file.png",
           "icons/folder.png",
           "icons/audio.png",
           "icons/image.png",
           "icons/code.png",
           "icons/document.png",
       }) {
    auto result = find(path);
    EXPECT_TRUE(result.has_value()) << "missing: " << path;
    if (result)
      EXPECT_FALSE(result->data.empty()) << "zero-size: " << path;
  }
}

TEST(ResourceStore, FindCoversAllPlannedFonts) {
  for (const char* path : {"fonts/default.ttf", "fonts/mono.ttf"}) {
    auto result = find(path);
    EXPECT_TRUE(result.has_value()) << "missing: " << path;
    if (result)
      EXPECT_FALSE(result->data.empty()) << "zero-size: " << path;
  }
}

// ── strip + find round-trip ───────────────────────────────────────────────────

TEST(ResourceStore, StripThenFindResolvesResUri) {
  std::string_view uri = "res://icons/folder.png";
  auto result = find(strip_scheme(uri));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->data.empty());
}

// ── data pointer stability ────────────────────────────────────────────────────

TEST(ResourceStore, TwoFindCallsReturnSamePointer) {
  auto a = find("icons/file.png");
  auto b = find("icons/file.png");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a->data.data(), b->data.data());
  EXPECT_EQ(a->data.size(), b->data.size());
}
