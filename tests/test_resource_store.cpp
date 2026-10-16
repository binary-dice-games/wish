// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <resources/resource_store.hpp>

#include <miniz.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

using bdg::wish::resource_store::extract_to;

namespace {

std::filesystem::path make_temp_dir(const char* suffix) {
  static std::atomic<uint32_t> counter{0};
  auto dir = std::filesystem::temp_directory_path() /
      ("wish_test_resource_store_" + std::string(suffix) + "_" + std::to_string(counter++));
  std::filesystem::remove_all(dir);
  return dir;
}

bool is_owner_writable(const std::filesystem::path& p) {
  return (std::filesystem::status(p).permissions() & std::filesystem::perms::owner_write) !=
      std::filesystem::perms::none;
}

const char* kExpectedFiles[] = {
    "icons/file.png",     "icons/folder.png", "icons/audio.png", "icons/image.png",
    "icons/document.png", "icons/code.png",   "fonts/default.ttf", "fonts/mono.ttf",
};

// A sample of the general-purpose Material Symbols UI icons (see
// resources/README.md for the full list).
const char* kExpectedUiIcons[] = {
    "icons/settings.png",
    "icons/search.png",
    "icons/add.png",
    "icons/save.png",
    "icons/refresh.png",
    "icons/copy.png",
    "icons/play.png",
    "icons/stop.png",
    "icons/terminal.png",
    "icons/database.png",
    "icons/zip.png",
    "icons/tree.png",
    "icons/sync.png",
    "icons/dark_mode.png",
    "icons/key.png",
    "icons/tag.png",
};

} // namespace

// ── extract_to — happy path ───────────────────────────────────────────────────

TEST(ResourceStore, ExtractToCreatesExpectedFiles) {
  auto dir = make_temp_dir("expected");
  ASSERT_TRUE(extract_to(dir));

  for (const char* rel : kExpectedFiles) {
    auto path = dir / rel;
    EXPECT_TRUE(std::filesystem::exists(path)) << "missing: " << rel;
    EXPECT_GT(std::filesystem::file_size(path), 0U) << "zero-size: " << rel;
  }

  std::filesystem::remove_all(dir);
}

TEST(ResourceStore, ExtractToIncludesUiIcons) {
  auto dir = make_temp_dir("ui_icons");
  ASSERT_TRUE(extract_to(dir));

  for (const char* rel : kExpectedUiIcons) {
    auto path = dir / rel;
    EXPECT_TRUE(std::filesystem::exists(path)) << "missing: " << rel;
    EXPECT_GT(std::filesystem::file_size(path), 0U) << "zero-size: " << rel;
  }

  std::filesystem::remove_all(dir);
}

TEST(ResourceStore, ExtractToCreatesDestinationDirectoryIfMissing) {
  auto dir = make_temp_dir("missing_dir");
  ASSERT_FALSE(std::filesystem::exists(dir));

  ASSERT_TRUE(extract_to(dir));
  EXPECT_TRUE(std::filesystem::exists(dir));
  EXPECT_TRUE(std::filesystem::exists(dir / "icons/folder.png"));

  std::filesystem::remove_all(dir);
}

TEST(ResourceStore, ExtractToMarksFilesReadOnly) {
  auto dir = make_temp_dir("readonly");
  ASSERT_TRUE(extract_to(dir));

  for (const char* rel : kExpectedFiles) {
    EXPECT_FALSE(is_owner_writable(dir / rel)) << "writable: " << rel;
  }

  std::filesystem::remove_all(dir);
}

// ── extract_to — repeated call ────────────────────────────────────────────────

TEST(ResourceStore, ExtractToAgainKeepsCurrentFilesAndReportsTheirCrc32s) {
  // A persistent sandbox is re-populated on every connect.
  auto dir = make_temp_dir("repeat");
  std::unordered_map<std::string, uint32_t> first;
  ASSERT_TRUE(extract_to(dir, &first));

  std::unordered_map<std::string, uint32_t> second;
  EXPECT_TRUE(extract_to(dir, &second));
  EXPECT_EQ(second, first);
  for (const char* rel : kExpectedFiles) {
    auto path = dir / rel;
    EXPECT_GT(std::filesystem::file_size(path), 0U) << "zero-size after re-extract: " << rel;
    EXPECT_FALSE(is_owner_writable(path)) << "writable after re-extract: " << rel;
  }

  std::filesystem::remove_all(dir);
}

TEST(ResourceStore, ExtractToAgainReplacesAStaleReadOnlyFile) {
  auto dir = make_temp_dir("stale");
  std::unordered_map<std::string, uint32_t> crc32s;
  ASSERT_TRUE(extract_to(dir, &crc32s));
  const auto path = dir / "icons/file.png";
  const auto size = std::filesystem::file_size(path);
  std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
  { std::ofstream(path, std::ios::binary | std::ios::trunc) << "stale"; }
  std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);

  EXPECT_TRUE(extract_to(dir));
  EXPECT_EQ(std::filesystem::file_size(path), size);
  EXPECT_FALSE(is_owner_writable(path));
  EXPECT_FALSE(std::filesystem::exists(dir / "icons/file.png.extracting"));

  std::filesystem::remove_all(dir);
}

// ── extract_to — CRC32 out-param ──────────────────────────────────────────────
//
// Surfaces miniz's own per-file CRC-32 (computed while unpacking the zip) so
// callers can use it as a content-version number without recomputing it --
// see web_renderer::get_or_load_texture()'s reuse of context::embedded_crc32s.

TEST(ResourceStore, ExtractToPopulatesCrc32MapMatchingFileContent) {
  auto dir = make_temp_dir("crc32");
  std::unordered_map<std::string, uint32_t> crc32s;
  ASSERT_TRUE(extract_to(dir, &crc32s));

  for (const char* rel : kExpectedFiles) {
    auto it = crc32s.find(rel);
    ASSERT_NE(it, crc32s.end()) << "missing crc32 entry: " << rel;

    std::ifstream file(dir / rel, std::ios::binary);
    ASSERT_TRUE(file) << rel;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    uint32_t expected = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()));
    EXPECT_EQ(it->second, expected) << "crc32 mismatch: " << rel;
  }

  std::filesystem::remove_all(dir);
}
