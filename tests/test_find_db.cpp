// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/desktop/find/client/find_db.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bdg::wish::find_db;
namespace fs = std::filesystem;

namespace {

// A fixed 2026-10-12 12:00 local time for the date tests.
std::time_t noon(int year, int month, int day) {
  std::tm tm_buf{};
  tm_buf.tm_year = year - 1900;
  tm_buf.tm_mon = month - 1;
  tm_buf.tm_mday = day;
  tm_buf.tm_hour = 12;
  tm_buf.tm_isdst = -1;
  return std::mktime(&tm_buf);
}

const std::time_t kNow = noon(2026, 10, 12);

// /home/u
//   Docs/          (dir)
//     report.PDF   2000   2026-10-12
//     notes.txt    100    2026-10-11
//   src/           (dir)
//     main.cpp     5000   2026-01-15
//     Makefile     300    2025-06-01
//   photo.jpg      3 MB   2026-10-10
//   .bashrc        50     2024-01-01
database make_db() {
  database db;
  const auto root = db.add_root("/home/u", noon(2026, 1, 1));
  const auto docs = db.add(root, "Docs", true, 0, noon(2026, 10, 12));
  db.add(docs, "report.PDF", false, 2000, noon(2026, 10, 12));
  db.add(docs, "notes.txt", false, 100, noon(2026, 10, 11));
  const auto src = db.add(root, "src", true, 0, noon(2026, 1, 15));
  db.add(src, "main.cpp", false, 5000, noon(2026, 1, 15));
  db.add(src, "Makefile", false, 300, noon(2025, 6, 1));
  db.add(root, "photo.jpg", false, 3u * 1024 * 1024, noon(2026, 10, 10));
  db.add(root, ".bashrc", false, 50, noon(2024, 1, 1));
  db.finalize();
  return db;
}

std::vector<std::string> names(const database& db, const search_result& r) {
  std::vector<std::string> out;
  for (auto i : r.rows)
    out.push_back(db.at(i).name);
  return out;
}

std::vector<std::string> find(const database& db, const std::string& query, search_options o = {}) {
  o.query = query;
  o.now = kNow;
  return names(db, search(db, o));
}

std::vector<std::string> sorted(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  return v;
}

} // namespace

// ── Database ──────────────────────────────────────────────────────────────────

TEST(FindDbTest, PathsCountsAndFolderSizes) {
  const database db = make_db();
  EXPECT_EQ(db.size(), 9u);
  EXPECT_EQ(db.file_count(), 6u);
  EXPECT_EQ(db.folder_count(), 3u);

  // Entry 2 is report.PDF in Docs.
  EXPECT_EQ(db.at(2).name, "report.PDF");
  EXPECT_EQ(db.full_path(2), "/home/u/Docs/report.PDF");
  EXPECT_EQ(db.parent_path(2), "/home/u/Docs");
  EXPECT_EQ(db.full_path(0), "/home/u");
  EXPECT_EQ(db.parent_path(0), "/home");
  EXPECT_EQ(db.at(0).name, "u");

  EXPECT_EQ(db.at(1).size, 2100u); // Docs
  EXPECT_EQ(db.at(0).size, 2100u + 5300u + 3u * 1024 * 1024 + 50u);
}

TEST(FindDbTest, EmptyQueryListsEverythingFoldersFirstByName) {
  const database db = make_db();
  EXPECT_EQ(
      find(db, ""),
      (std::vector<std::string>{
          "Docs", "src", "u", ".bashrc", "main.cpp", "Makefile", "notes.txt", "photo.jpg", "report.PDF"}));
}

// ── Query syntax ──────────────────────────────────────────────────────────────

TEST(FindDbTest, SubstringIsCaseInsensitiveForLowerCaseQueries) {
  const database db = make_db();
  EXPECT_EQ(find(db, "make"), (std::vector<std::string>{"Makefile"}));
  EXPECT_EQ(find(db, "pdf"), (std::vector<std::string>{"report.PDF"}));
}

TEST(FindDbTest, UpperCaseQueryMatchesCaseAutomatically) {
  const database db = make_db();
  EXPECT_TRUE(find(db, "Pdf").empty());
  EXPECT_EQ(find(db, "PDF"), (std::vector<std::string>{"report.PDF"}));

  search_options o;
  o.auto_match_case = false;
  EXPECT_EQ(find(db, "Pdf", o), (std::vector<std::string>{"report.PDF"}));

  o.match_case = true;
  EXPECT_TRUE(find(db, "make", o).empty());
}

TEST(FindDbTest, TermsAreAndedOrBindsTighterAndNotNegates) {
  const database db = make_db();
  EXPECT_EQ(find(db, "m a"), (std::vector<std::string>{"main.cpp", "Makefile"}));
  EXPECT_EQ(sorted(find(db, "main OR photo")), (std::vector<std::string>{"main.cpp", "photo.jpg"}));
  EXPECT_EQ(sorted(find(db, "o | main")), sorted(find(db, "o OR main")));
  // "t" AND ("pdf" OR "jpg")
  EXPECT_EQ(sorted(find(db, "t pdf OR jpg")), (std::vector<std::string>{"photo.jpg", "report.PDF"}));
  EXPECT_EQ(find(db, "file: !t"), (std::vector<std::string>{".bashrc", "main.cpp", "Makefile"}));
  EXPECT_EQ(find(db, "file: NOT t"), (std::vector<std::string>{".bashrc", "main.cpp", "Makefile"}));
}

TEST(FindDbTest, WildcardsMatchTheWholeName) {
  const database db = make_db();
  EXPECT_EQ(find(db, "*.cpp"), (std::vector<std::string>{"main.cpp"}));
  EXPECT_EQ(find(db, "m*"), (std::vector<std::string>{"main.cpp", "Makefile"}));
  EXPECT_EQ(find(db, "?ain.*"), (std::vector<std::string>{"main.cpp"}));
  EXPECT_TRUE(find(db, "ain*").empty());
}

TEST(FindDbTest, QuotesMakeOneLiteralTerm) {
  database db;
  const auto root = db.add_root("/r");
  db.add(root, "my notes.txt", false, 1, 0);
  db.add(root, "my.txt", false, 1, 0);
  db.add(root, "notes.txt", false, 1, 0);
  db.finalize();
  EXPECT_EQ(find(db, "\"my notes\""), (std::vector<std::string>{"my notes.txt"}));
  EXPECT_EQ(find(db, "my notes"), (std::vector<std::string>{"my notes.txt"}));
  EXPECT_EQ(find(db, "\"OR\""), (std::vector<std::string>{}));
}

TEST(FindDbTest, PathSearchExplicitAndAutomatic) {
  const database db = make_db();
  // "src/" contains a separator: matched against the full path.
  EXPECT_EQ(find(db, "src/"), (std::vector<std::string>{"main.cpp", "Makefile"}));
  EXPECT_EQ(find(db, "path:docs file:"), (std::vector<std::string>{"notes.txt", "report.PDF"}));

  search_options o;
  o.search_in_path = true;
  EXPECT_EQ(find(db, "docs", o), (std::vector<std::string>{"Docs", "notes.txt", "report.PDF"}));
  o.search_in_path = false;
  o.auto_search_in_path = false;
  EXPECT_TRUE(find(db, "src/", o).empty());
}

TEST(FindDbTest, Functions) {
  const database db = make_db();
  EXPECT_EQ(find(db, "ext:jpg;pdf"), (std::vector<std::string>{"photo.jpg", "report.PDF"}));
  EXPECT_EQ(find(db, "ext:"), (std::vector<std::string>{".bashrc", "Makefile"}));
  EXPECT_EQ(find(db, "folder:"), (std::vector<std::string>{"Docs", "src", "u"}));
  EXPECT_EQ(find(db, "folder:s"), (std::vector<std::string>{"Docs", "src"}));
  EXPECT_EQ(find(db, "size:>1mb"), (std::vector<std::string>{"photo.jpg"}));
  EXPECT_EQ(find(db, "size:<=100"), (std::vector<std::string>{".bashrc", "notes.txt"}));
  EXPECT_EQ(find(db, "size:200..2000"), (std::vector<std::string>{"Makefile", "report.PDF"}));
  EXPECT_EQ(find(db, "exact:main.cpp"), (std::vector<std::string>{"main.cpp"}));
  EXPECT_TRUE(find(db, "exact:main").empty());
  EXPECT_EQ(find(db, "case:Make"), (std::vector<std::string>{"Makefile"}));
  EXPECT_EQ(find(db, "nocase:DOCS"), (std::vector<std::string>{"Docs"}));
  EXPECT_EQ(find(db, "regex:^m.*p$"), (std::vector<std::string>{"main.cpp"}));
  // Not a known function: plain text.
  EXPECT_TRUE(find(db, "c:foo").empty());
}

TEST(FindDbTest, DateModified) {
  const database db = make_db();
  EXPECT_EQ(find(db, "dm:today"), (std::vector<std::string>{"Docs", "report.PDF"}));
  EXPECT_EQ(find(db, "dm:yesterday"), (std::vector<std::string>{"notes.txt"}));
  EXPECT_EQ(find(db, "dm:thisweek file:"), (std::vector<std::string>{"notes.txt", "photo.jpg", "report.PDF"}));
  EXPECT_EQ(find(db, "dm:2026-01-15"), (std::vector<std::string>{"src", "main.cpp"}));
  EXPECT_EQ(find(db, "dm:<2026-01-01"), (std::vector<std::string>{".bashrc", "Makefile"}));
  EXPECT_EQ(find(db, "dm:2025 OR dm:2024"), (std::vector<std::string>{".bashrc", "Makefile"}));
  EXPECT_EQ(find(db, "dm:2026-10-10..2026-10-11"), (std::vector<std::string>{"notes.txt", "photo.jpg"}));
}

TEST(FindDbTest, InvalidQueriesReportAnError) {
  const database db = make_db();
  search_options o;
  o.query = "size:huge";
  auto r = search(db, o);
  EXPECT_FALSE(r.error.empty());
  EXPECT_EQ(r.total, 0u);

  o.query = "(";
  o.regex = true;
  r = search(db, o);
  EXPECT_FALSE(r.error.empty());
}

TEST(FindDbTest, RegexModeTreatsTheWholeQueryAsOneExpression) {
  const database db = make_db();
  search_options o;
  o.regex = true;
  EXPECT_EQ(find(db, "^(main|photo)\\.", o), (std::vector<std::string>{"main.cpp", "photo.jpg"}));
  EXPECT_EQ(find(db, "pdf$", o), (std::vector<std::string>{"report.PDF"}));
}

// ── Filters, sorting, limits ──────────────────────────────────────────────────

TEST(FindDbTest, FilterDropDown) {
  const database db = make_db();
  search_options o;
  o.filter = filter_kind::folders;
  EXPECT_EQ(find(db, "", o), (std::vector<std::string>{"Docs", "src", "u"}));
  o.filter = filter_kind::files;
  EXPECT_EQ(find(db, "", o).size(), 6u);
  o.filter = filter_kind::pictures;
  EXPECT_EQ(find(db, "", o), (std::vector<std::string>{"photo.jpg"}));
  o.filter = filter_kind::documents;
  EXPECT_EQ(find(db, "", o), (std::vector<std::string>{"main.cpp", "notes.txt", "report.PDF"}));
}

TEST(FindDbTest, SortsByEveryColumnBothWays) {
  const database db = make_db();
  search_options o;
  o.filter = filter_kind::files;

  o.sort = sort_column::size;
  o.ascending = false;
  EXPECT_EQ(
      find(db, "", o),
      (std::vector<std::string>{"photo.jpg", "main.cpp", "report.PDF", "Makefile", "notes.txt", ".bashrc"}));

  o.sort = sort_column::modified;
  o.ascending = true;
  EXPECT_EQ(
      find(db, "", o),
      (std::vector<std::string>{".bashrc", "Makefile", "main.cpp", "photo.jpg", "notes.txt", "report.PDF"}));

  // Path: /home/u, then /home/u/Docs, then /home/u/src; by name inside.
  o.sort = sort_column::path;
  EXPECT_EQ(
      find(db, "", o),
      (std::vector<std::string>{".bashrc", "photo.jpg", "notes.txt", "report.PDF", "main.cpp", "Makefile"}));

  o.sort = sort_column::name;
  o.ascending = false;
  EXPECT_EQ(
      find(db, "", o),
      (std::vector<std::string>{"report.PDF", "photo.jpg", "notes.txt", "Makefile", "main.cpp", ".bashrc"}));

  // Type: documents before pictures, plain files last.
  o.sort = sort_column::type;
  o.ascending = true;
  EXPECT_EQ(
      find(db, "", o),
      (std::vector<std::string>{"main.cpp", "report.PDF", "notes.txt", "photo.jpg", ".bashrc", "Makefile"}));
}

TEST(FindDbTest, MaxRowsLimitsRowsNotTheTotal) {
  const database db = make_db();
  search_options o;
  o.max_rows = 2;
  o.sort = sort_column::size;
  o.ascending = false;
  o.filter = filter_kind::files;
  const auto r = search(db, o);
  EXPECT_EQ(r.total, 6u);
  EXPECT_EQ(names(db, r), (std::vector<std::string>{"photo.jpg", "main.cpp"}));
}

TEST(FindDbTest, CancelledSearchStops) {
  database db;
  const auto root = db.add_root("/r");
  for (int i = 0; i < 70000; ++i)
    db.add(root, "f" + std::to_string(i), false, 1, 0);
  db.finalize();
  const auto r = search(db, {}, [] { return true; });
  EXPECT_TRUE(r.cancelled);
  EXPECT_TRUE(r.rows.empty());
}

// ── Formatting ────────────────────────────────────────────────────────────────

TEST(FindDbTest, TypeLabelsAndFormatting) {
  entry e;
  e.name = "a.PNG";
  EXPECT_EQ(type_label(e), "$$FIND_TYPE_IMAGE\x1fPNG");
  e.name = "Makefile";
  EXPECT_EQ(type_label(e), "$$FIND_TYPE_FILE");
  e.name = "x.weird";
  EXPECT_EQ(type_label(e), "$$FIND_TYPE_EXT_FILE\x1fWEIRD");
  e.is_dir = true;
  EXPECT_EQ(type_label(e), "$$FIND_TYPE_FOLDER");

  EXPECT_EQ(extension_of(".bashrc"), "");
  EXPECT_EQ(extension_of("a.tar.GZ"), "gz");
  EXPECT_EQ(format_bytes(1536), "1.5 KB");
  EXPECT_EQ(format_count(1234567), "1,234,567");
  EXPECT_EQ(format_time(0), "");
}

// ── Indexing a real folder ────────────────────────────────────────────────────

TEST(FindDbTest, BuildIndexesAFolderWithExclusions) {
  const fs::path dir = fs::temp_directory_path() /
      ("wish_find_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
  fs::remove_all(dir);
  fs::create_directories(dir / "sub" / "node_modules");
  fs::create_directories(dir / ".hidden");
  std::ofstream(dir / "a.txt") << "hello";
  std::ofstream(dir / "sub" / "b.md") << "x";
  std::ofstream(dir / "sub" / "node_modules" / "c.js") << "x";
  std::ofstream(dir / ".hidden" / "d.txt") << "x";

  index_options opts;
  opts.locations = {dir.string()};
  opts.excludes = {"node_modules"};
  opts.exclude_hidden = true;
  database db;
  index_progress totals;
  ASSERT_TRUE(database::build(opts, db, {}, &totals));
  EXPECT_EQ(sorted(find(db, "")), (std::vector<std::string>{"a.txt", "b.md", "sub", dir.filename().string()}));
  EXPECT_EQ(totals.files, 2u);
  EXPECT_EQ(totals.folders, 2u);

  search_options o;
  o.query = "b.md";
  const auto r = search(db, o);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(db.full_path(r.rows[0]), (dir / "sub" / "b.md").string());
  EXPECT_EQ(db.at(r.rows[0]).size, 1u);

  // Cancelling from the progress callback.
  EXPECT_FALSE(database::build(opts, db, [](const index_progress&) { return false; }));
  fs::remove_all(dir);
}

TEST(FindDbTest, MissingLocationCountsAsAnError) {
  index_options opts;
  opts.locations = {"/definitely/not/here/wish_find"};
  database db;
  index_progress totals;
  ASSERT_TRUE(database::build(opts, db, {}, &totals));
  EXPECT_EQ(db.size(), 0u);
  EXPECT_EQ(totals.errors, 1u);
}
