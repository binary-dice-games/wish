// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/sq/client/sq_query_guard.hpp"
#include "modules/bdg/dev/sq/client/sq_result_parser.hpp"

using namespace bdg::wish::sq;

namespace {

// ── parse_jsonl_result ─────────────────────────────────────────────────────

TEST(SqResultParserTest, ParsesColumnsInOrderAndTypedCells) {
  const auto r = parse_jsonl_result(
      "{\"id\": 1, \"title\": \"Kind of Blue\", \"price\": 9.5, \"ok\": true, \"n\": null}\n"
      "{\"id\": 2, \"title\": \"A Love Supreme\", \"price\": 11, \"ok\": false, \"n\": null}\n",
      100);
  ASSERT_EQ(r.columns.size(), 5u);
  EXPECT_EQ(r.columns[0], "id");
  EXPECT_EQ(r.columns[4], "n");
  ASSERT_EQ(r.rows.size(), 2u);
  EXPECT_EQ(r.rows[0][1].text, "Kind of Blue");
  EXPECT_EQ(r.rows[0][2].text, "9.5");
  EXPECT_EQ(r.rows[1][3].text, "false");
  EXPECT_TRUE(r.rows[0][4].is_null);
  EXPECT_FALSE(r.truncated);
}

TEST(SqResultParserTest, NullIsDistinctFromEmptyString) {
  const auto r = parse_jsonl_result("{\"a\": \"\", \"b\": null}\n", 10);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_FALSE(r.rows[0][0].is_null);
  EXPECT_TRUE(r.rows[0][1].is_null);
}

TEST(SqResultParserTest, UnescapesStringsIncludingUnicodeAndSurrogatePairs) {
  const auto r = parse_jsonl_result(
      "{\"s\": \"x\\ny\\\"z\\\\ \\u00e9 \\ud83d\\ude00 {not: [nested]}\"}\n", 10);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0][0].text, "x\ny\"z\\ \xC3\xA9 \xF0\x9F\x98\x80 {not: [nested]}");
}

TEST(SqResultParserTest, NestedValuesKeepTheirRawJson) {
  const auto r = parse_jsonl_result("{\"j\": {\"a\": [1, \"}\"]}, \"k\": 2}\n", 10);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0][0].text, "{\"a\": [1, \"}\"]}");
  EXPECT_EQ(r.rows[0][1].text, "2");
}

TEST(SqResultParserTest, DuplicateColumnNamesAreKeptAsSeparateColumns) {
  const auto r = parse_jsonl_result("{\"id\": 1, \"id\": 2}\n", 10);
  ASSERT_EQ(r.columns.size(), 2u);
  EXPECT_EQ(r.rows[0][0].text, "1");
  EXPECT_EQ(r.rows[0][1].text, "2");
}

TEST(SqResultParserTest, MaxRowsTruncatesButCountsEverything) {
  std::string text;
  for (int i = 0; i < 10; ++i)
    text += "{\"i\": " + std::to_string(i) + "}\n";
  const auto r = parse_jsonl_result(text, 3);
  EXPECT_EQ(r.rows.size(), 3u);
  EXPECT_EQ(r.total_rows, 10u);
  EXPECT_TRUE(r.truncated);
}

TEST(SqResultParserTest, EmptyAndMalformedInputDegradeGracefully) {
  EXPECT_TRUE(parse_jsonl_result("", 10).columns.empty());
  const auto r = parse_jsonl_result("garbage\n{\"a\": \n{\"a\": 1}\n\n", 10);
  EXPECT_EQ(r.malformed_lines, 2u);
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0][0].text, "1");
}

// ── mask_location ──────────────────────────────────────────────────────────

TEST(SqMaskLocationTest, MasksThePasswordOnly) {
  EXPECT_EQ(mask_location("postgres://alice:hunter2@db:5432/x?sslmode=disable"), "postgres://alice:xxxxx@db:5432/x?sslmode=disable");
}

TEST(SqMaskLocationTest, LeavesLocationsWithoutAPasswordAlone) {
  EXPECT_EQ(mask_location("postgres://alice@db/x"), "postgres://alice@db/x");
  EXPECT_EQ(mask_location("/tmp/data.db"), "/tmp/data.db");
  EXPECT_EQ(mask_location("sqlite3:///tmp/a:b@c.db"), "sqlite3:///tmp/a:b@c.db");
  EXPECT_EQ(mask_location("select 1"), "select 1");
}

// ── check_read_only_sql ────────────────────────────────────────────────────

TEST(SqQueryGuardTest, AcceptsReadOnlyStatements) {
  for (const char* sql : {
           "select * from album",
           "  SELECT 1;",
           "with x as (select 1) select * from x",
           "values (1), (2)",
           "show tables",
           "describe album",
           "explain select * from album",
           "-- leading comment\nselect 1",
           "select replace(title, 'a', 'b'), insert('abc', 1, 1, 'x') from album",
           "select 'delete from x; drop table y' as s",
           "select \"update\" from t",
           "select $$insert into$$",
       })
    EXPECT_FALSE(check_read_only_sql(sql).has_value()) << sql;
}

TEST(SqQueryGuardTest, RejectsStatementsThatChangeData) {
  for (const char* sql : {
           "insert into t values (1)",
           "update t set a = 1",
           "delete from t",
           "drop table t",
           "create table t (a int)",
           "alter table t add b int",
           "truncate table t",
           "replace into t values (1)",
           "pragma writable_schema = 1",
           "attach database 'x.db' as x",
           "with d as (delete from t returning *) select * from d",
           "select * into copy_of_t from t",
           "explain analyze delete from t",
           "select * from t for update",
       })
    EXPECT_TRUE(check_read_only_sql(sql).has_value()) << sql;
}

TEST(SqQueryGuardTest, RejectsMultipleStatementsAndEmptyInput) {
  EXPECT_TRUE(check_read_only_sql("select 1; select 2").has_value());
  EXPECT_TRUE(check_read_only_sql("select 1; drop table t").has_value());
  EXPECT_TRUE(check_read_only_sql("").has_value());
  EXPECT_TRUE(check_read_only_sql(" -- only a comment").has_value());
  EXPECT_TRUE(check_read_only_sql(";").has_value());
}

TEST(SqQueryGuardTest, CommentsCannotHideForbiddenKeywords) {
  EXPECT_TRUE(check_read_only_sql("select 1 /* x */ ; /* y */ delete from t").has_value());
  EXPECT_TRUE(check_read_only_sql("select 1\n-- ok\n; drop table t").has_value());
  EXPECT_TRUE(check_read_only_sql("select '\\'; drop table t; --'").has_value());
}

} // namespace
