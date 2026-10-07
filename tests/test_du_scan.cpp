// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/desktop/du/client/du_scan.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace du_scan = bdg::wish::du_scan;

namespace {

void write_file(const fs::path& path, size_t bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << std::string(bytes, 'x');
}

// A small tree, unique per test (ctest runs each test case as its own
// process, in parallel):
//   big.bin 5000 | docs/{a.txt 300, b.txt 200, deep/c.md 100} | small.log 10
//   | empty/
class DuScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
        (std::string{"wish_du_scan_"} + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(root_);
    write_file(root_ / "big.bin", 5000);
    write_file(root_ / "docs" / "a.txt", 300);
    write_file(root_ / "docs" / "b.txt", 200);
    write_file(root_ / "docs" / "deep" / "c.md", 100);
    write_file(root_ / "small.log", 10);
    fs::create_directories(root_ / "empty");
  }

  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
};

} // namespace

TEST_F(DuScanTest, TotalsAndLargestFirstOrder) {
  du_scan::entry tree;
  du_scan::progress totals;
  ASSERT_TRUE(du_scan::scan(root_, tree, {}, &totals));

  EXPECT_EQ(tree.name, root_.string());
  EXPECT_TRUE(tree.is_dir);
  EXPECT_EQ(tree.size, 5610u);
  EXPECT_EQ(tree.files, 5u);
  EXPECT_EQ(tree.dirs, 3u); // docs, docs/deep, empty
  EXPECT_EQ(totals.items, 8u);
  EXPECT_EQ(totals.bytes, 5610u);
  EXPECT_EQ(totals.errors, 0u);

  ASSERT_EQ(tree.children.size(), 4u);
  EXPECT_EQ(tree.children[0].name, "big.bin");
  EXPECT_EQ(tree.children[1].name, "docs");
  EXPECT_EQ(tree.children[2].name, "small.log");
  EXPECT_EQ(tree.children[3].name, "empty");

  const du_scan::entry& docs = tree.children[1];
  EXPECT_TRUE(docs.is_dir);
  EXPECT_EQ(docs.size, 600u);
  EXPECT_EQ(docs.files, 3u);
  EXPECT_EQ(docs.dirs, 1u);
  EXPECT_FALSE(tree.children[0].is_dir);
  EXPECT_EQ(tree.children[0].files, 0u);
}

TEST_F(DuScanTest, FindResolvesRelativePaths) {
  du_scan::entry tree;
  ASSERT_TRUE(du_scan::scan(root_, tree));

  EXPECT_EQ(du_scan::find(tree, ""), &tree);
  const du_scan::entry* deep = du_scan::find(tree, "docs/deep");
  ASSERT_NE(deep, nullptr);
  EXPECT_EQ(deep->size, 100u);
  const du_scan::entry* file = du_scan::find(tree, "docs/deep/c.md");
  ASSERT_NE(file, nullptr);
  EXPECT_FALSE(file->is_dir);
  EXPECT_EQ(du_scan::find(tree, "docs/missing"), nullptr);
  EXPECT_EQ(du_scan::find(tree, "big.bin/x"), nullptr);
}

TEST_F(DuScanTest, ProgressCallbackCanCancel) {
  du_scan::entry tree;
  int calls = 0;
  const bool completed = du_scan::scan(root_, tree, [&](const du_scan::progress&) { return ++calls < 2; });
  EXPECT_FALSE(completed);
  EXPECT_EQ(calls, 2);
}

TEST_F(DuScanTest, ScanningAFileThrows) {
  du_scan::entry tree;
  EXPECT_THROW(du_scan::scan(root_ / "big.bin", tree), std::runtime_error);
  EXPECT_THROW(du_scan::scan(root_ / "nope", tree), std::runtime_error);
}

#if !defined(_WIN32)
TEST_F(DuScanTest, SymlinksAreNotFollowed) {
  fs::create_directory_symlink(root_ / "docs", root_ / "docs_link");
  du_scan::entry tree;
  ASSERT_TRUE(du_scan::scan(root_, tree));
  EXPECT_EQ(tree.size, 5610u);
  const du_scan::entry* link = du_scan::find(tree, "docs_link");
  ASSERT_NE(link, nullptr);
  EXPECT_FALSE(link->is_dir);
  EXPECT_EQ(link->size, 0u);
}
#endif

// ── Treemap flattening ────────────────────────────────────────────────────────

TEST_F(DuScanTest, TreemapListsParentsBeforeChildren) {
  du_scan::entry tree;
  ASSERT_TRUE(du_scan::scan(root_, tree));
  const du_scan::treemap map = du_scan::build_treemap(tree, 6000, 0.0);

  // Root + big.bin, docs{a, b, deep{c}}, small.log; the empty dir has no area.
  ASSERT_EQ(map.parents.size(), 8u);
  EXPECT_EQ(map.sizes.size(), 8u);
  EXPECT_EQ(map.colors.size(), 8u);
  EXPECT_EQ(map.kinds.size(), 8u);
  EXPECT_EQ(map.labels.size(), 8u);
  EXPECT_EQ(map.parents[0], -1);
  EXPECT_EQ(map.labels[0], root_.filename().string());
  EXPECT_EQ(map.bytes[0], 5610u);
  for (size_t i = 1; i < map.parents.size(); ++i) {
    EXPECT_GE(map.parents[i], 0);
    EXPECT_LT(static_cast<size_t>(map.parents[i]), i);
  }

  EXPECT_EQ(map.labels[1], "big.bin");
  EXPECT_EQ(map.kinds[1], static_cast<int32_t>(du_scan::node_kind::file));
  EXPECT_EQ(map.labels[2], "docs");
  EXPECT_EQ(map.kinds[2], static_cast<int32_t>(du_scan::node_kind::dir));
  EXPECT_EQ(map.labels[3], "a.txt");
  EXPECT_EQ(map.parents[3], 2);
  EXPECT_FLOAT_EQ(map.sizes[3], 300.0f);
  // Same extension, same color.
  EXPECT_EQ(map.colors[3], map.colors[4]);
}

TEST_F(DuScanTest, TreemapMergesSmallItems) {
  du_scan::entry tree;
  ASSERT_TRUE(du_scan::scan(root_, tree));
  // Anything under 5% of 5610 bytes (280) is merged per directory.
  const du_scan::treemap map = du_scan::build_treemap(tree, 6000, 0.05);

  // Root, big.bin, docs{a.txt, (2 smaller items)}, small.log (a lone small
  // file keeps its own name).
  ASSERT_EQ(map.labels.size(), 6u);
  EXPECT_EQ(map.labels[3], "a.txt");
  EXPECT_EQ(map.labels[4], "(2 smaller items)");
  EXPECT_EQ(map.kinds[4], static_cast<int32_t>(du_scan::node_kind::rest));
  EXPECT_EQ(map.bytes[4], 300u); // b.txt + deep/
  EXPECT_EQ(map.labels[5], "small.log");
  EXPECT_EQ(map.kinds[5], static_cast<int32_t>(du_scan::node_kind::file));
}

TEST_F(DuScanTest, TreemapHonorsNodeBudget) {
  du_scan::entry tree;
  ASSERT_TRUE(du_scan::scan(root_, tree));
  const du_scan::treemap map = du_scan::build_treemap(tree, 3, 0.0);
  EXPECT_LE(map.parents.size(), 3u);
  // Nothing is lost: the top-level nodes still add up to the folder.
  std::uint64_t sum = 0;
  for (size_t i = 1; i < map.parents.size(); ++i)
    if (map.parents[i] == 0)
      sum += map.bytes[i];
  EXPECT_EQ(sum, 5610u);
}

// ── Formatting ────────────────────────────────────────────────────────────────

TEST(DuScanFormat, Bytes) {
  EXPECT_EQ(du_scan::format_bytes(0), "0 B");
  EXPECT_EQ(du_scan::format_bytes(1023), "1023 B");
  EXPECT_EQ(du_scan::format_bytes(1536), "1.5 KB");
  EXPECT_EQ(du_scan::format_bytes(5ull * 1024 * 1024 * 1024), "5.0 GB");
}

TEST(DuScanFormat, Count) {
  EXPECT_EQ(du_scan::format_count(0), "0");
  EXPECT_EQ(du_scan::format_count(999), "999");
  EXPECT_EQ(du_scan::format_count(1000), "1,000");
  EXPECT_EQ(du_scan::format_count(1234567), "1,234,567");
}

TEST(DuScanFormat, ColorsGroupByExtensionCaseInsensitively) {
  EXPECT_EQ(du_scan::color_for("a.JPG"), du_scan::color_for("b.png"));
  EXPECT_NE(du_scan::color_for("a.jpg"), du_scan::color_for("a.zip"));
  EXPECT_EQ(du_scan::color_for("x.weirdext"), du_scan::color_for("y.weirdext"));
  // A dotfile has no extension.
  EXPECT_EQ(du_scan::color_for(".bashrc"), du_scan::color_for("Makefile"));
  // Every color is opaque and non-zero (0 means "pick one" to Treemap).
  EXPECT_EQ(du_scan::color_for("x.weirdext") & 0xFF, 0xFF);
}
