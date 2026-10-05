// MIT License © 2026 Binary Dice Games
/// @file test_nymph_document.cpp
/// @brief nymph's pure text handling: splitting a source into its three
///        parts and parsing the CSV data part.
#include <gtest/gtest.h>

#include "modules/bdg/dev/nymph/server/nymph_csv.hpp"
#include "modules/bdg/dev/nymph/server/nymph_document.hpp"

#include <cmath>
#include <string>

namespace nymph = bdg::wish::nymph;

namespace {

const std::string kExample = "Example of a graph\n"
                             "--\n"
                             "root:\n"
                             "  type: Plot\n"
                             "--\n"
                             "time, value\n"
                             "0, 12\n"
                             "1, 6\n";

int error_line(const std::string& text) {
  try {
    nymph::parse_document(text);
  } catch (const nymph::error& e) {
    return e.line();
  }
  return -1;
}

int csv_error_line(const std::string& text, int first_line = 1) {
  try {
    nymph::parse_csv(text, first_line).numbers(0);
  } catch (const nymph::error& e) {
    return e.line();
  }
  return -1;
}

} // namespace

// ── Document ─────────────────────────────────────────────────────────────────

TEST(NymphDocument, SplitsTheThreeParts) {
  auto doc = nymph::parse_document(kExample);
  EXPECT_EQ(doc.description, "Example of a graph\n");
  EXPECT_EQ(doc.format, "root:\n  type: Plot\n");
  EXPECT_EQ(doc.data, "time, value\n0, 12\n1, 6\n");
  EXPECT_EQ(doc.format_line, 3);
  EXPECT_EQ(doc.data_line, 6);
}

TEST(NymphDocument, ComposeIsTheExactInverse) {
  const std::string cases[] = {
      kExample,
      "a\r\n--\r\nroot: {}\r\n--\r\nx\r\n1\r\n", // CRLF
      "a\n--\nroot: {}\n--\nx\n1",                // no trailing newline
      "--\nroot: {}\n--\nx\n1\n",                 // empty description
      "a\n--\nroot: {}\n--\n",                    // empty data
      "a\n--\nroot: {}\n--",                      // separator is the last line, no newline
      "a\n--  \t\nroot: {}\n-- \nx\n",            // separators with trailing blanks
      "a\n--\nroot: {}\n--\nx\n--\n1\n",          // a "--" line inside the data
  };
  for (const auto& text : cases)
    EXPECT_EQ(nymph::compose_document(nymph::parse_document(text)), text) << text;
}

TEST(NymphDocument, OnlyTheFirstTwoSeparatorsCount) {
  auto doc = nymph::parse_document("d\n--\nf\n--\nx\n--\n1\n");
  EXPECT_EQ(doc.data, "x\n--\n1\n");
}

TEST(NymphDocument, SeparatorMustBeTheWholeLine) {
  EXPECT_EQ(error_line("d\n --\nf\n---\nx\n"), 1); // neither " --" nor "---" is one
}

TEST(NymphDocument, FewerThanTwoSeparatorsIsAnError) {
  EXPECT_EQ(error_line("just text\n"), 1);
  EXPECT_EQ(error_line("d\n--\nroot: {}\n"), 1);
  EXPECT_EQ(error_line(""), 1);
}

TEST(NymphDocument, RejectsOversizedSource) {
  std::string big(nymph::kMaxSourceBytes + 1, 'x');
  EXPECT_EQ(error_line(big), 1);
}

TEST(NymphDocument, ErrorWhatCarriesThePosition) {
  nymph::error e(7, 3, "boom");
  EXPECT_STREQ(e.what(), "7:3: boom");
  EXPECT_EQ(e.message(), "boom");
}

TEST(NymphDocument, MakeDocumentKeepsSeparatorsOnTheirOwnLines) {
  auto doc = nymph::make_document("desc", "root: {}", "x\n1");
  EXPECT_EQ(nymph::compose_document(doc), "desc\n--\nroot: {}\n--\nx\n1");
  EXPECT_EQ(doc.format_line, 3);
  EXPECT_EQ(doc.data_line, 5);
  // What it composes parses back to the same parts and line numbers.
  auto again = nymph::parse_document(nymph::compose_document(doc));
  EXPECT_EQ(again.description, doc.description);
  EXPECT_EQ(again.format, doc.format);
  EXPECT_EQ(again.data, doc.data);
  EXPECT_EQ(again.format_line, doc.format_line);
  EXPECT_EQ(again.data_line, doc.data_line);
}

TEST(NymphDocument, MakeDocumentAllowsEmptyParts) {
  EXPECT_EQ(nymph::compose_document(nymph::make_document("", "root: {}\n", "")), "--\nroot: {}\n--\n");
}

// ── CSV ──────────────────────────────────────────────────────────────────────

TEST(NymphCsv, HeaderAndRowsAreTrimmed) {
  auto t = nymph::parse_csv("time, value\n0, 12\n1, 6\n");
  ASSERT_EQ(t.headers.size(), 2u);
  EXPECT_EQ(t.headers[0], "time");
  EXPECT_EQ(t.headers[1], "value");
  ASSERT_EQ(t.row_count(), 2u);
  EXPECT_EQ(t.columns[1][0], "12");
  EXPECT_EQ(t.columns[1][1], "6");
}

TEST(NymphCsv, QuotedCells) {
  auto t = nymph::parse_csv("name,note\n\"Doe, Jane\",\"said \"\"hi\"\"\"\n \"two\nlines\" , x\n");
  ASSERT_EQ(t.row_count(), 2u);
  EXPECT_EQ(t.columns[0][0], "Doe, Jane");
  EXPECT_EQ(t.columns[1][0], "said \"hi\"");
  EXPECT_EQ(t.columns[0][1], "two\nlines");
  EXPECT_EQ(t.columns[1][1], "x");
}

TEST(NymphCsv, BlankLinesAndCrLfAreTolerated) {
  auto t = nymph::parse_csv("a,b\r\n\r\n1,2\r\n   \r\n3,4");
  ASSERT_EQ(t.row_count(), 2u);
  EXPECT_EQ(t.columns[1][1], "4");
  EXPECT_EQ(t.row_lines[0], 3);
  EXPECT_EQ(t.row_lines[1], 5);
}

TEST(NymphCsv, EmptyTextIsAnEmptyTable) {
  auto t = nymph::parse_csv("");
  EXPECT_TRUE(t.headers.empty());
  EXPECT_EQ(t.row_count(), 0u);
  EXPECT_EQ(nymph::parse_csv("\n  \n").row_count(), 0u);
}

TEST(NymphCsv, RaggedRowIsAnErrorAtItsSourceLine) {
  try {
    nymph::parse_csv("a,b\n1,2\n3\n", /*first_line=*/10);
    FAIL() << "expected an error";
  } catch (const nymph::error& e) {
    EXPECT_EQ(e.line(), 12);
  }
}

TEST(NymphCsv, LineNumbersCountNewlinesInsideQuotes) {
  auto t = nymph::parse_csv("a,b\n\"x\ny\",1\n2,3\n");
  ASSERT_EQ(t.row_count(), 2u);
  EXPECT_EQ(t.row_lines[0], 2);
  EXPECT_EQ(t.row_lines[1], 4);
}

TEST(NymphCsv, HeaderProblemsAreErrors) {
  EXPECT_THROW(nymph::parse_csv("a,a\n1,2\n"), nymph::error);
  EXPECT_THROW(nymph::parse_csv("a,,c\n1,2,3\n"), nymph::error);
  EXPECT_THROW(nymph::parse_csv("a,b\n\"open,2\n"), nymph::error);
  EXPECT_THROW(nymph::parse_csv("a,b\n\"x\"y,2\n"), nymph::error);
}

TEST(NymphCsv, NumbersConvertPerColumn) {
  auto t = nymph::parse_csv("v,label\n1e3,a\n-0.5,b\n +7 ,c\n,d\n");
  auto v = t.numbers(0);
  ASSERT_EQ(v.size(), 4u);
  EXPECT_FLOAT_EQ(v[0], 1000.0f);
  EXPECT_FLOAT_EQ(v[1], -0.5f);
  EXPECT_FLOAT_EQ(v[2], 7.0f);
  EXPECT_TRUE(std::isnan(v[3])); // an empty cell is a gap, not an error
  // The text column is never checked until someone asks for it as numbers.
  EXPECT_THROW(t.numbers(1), nymph::error);
}

TEST(NymphCsv, NonNumberNamesItsLine) {
  EXPECT_EQ(csv_error_line("v\n1\nabc\n3\n", /*first_line=*/20), 22);
  EXPECT_EQ(csv_error_line("v\n1\n2x\n"), 3);
}

TEST(NymphCsv, FindColumnByNameThenPosition) {
  auto t = nymph::parse_csv("time,unit price,3\n1,2,3\n");
  EXPECT_EQ(t.find_column("time"), 0);
  EXPECT_EQ(t.find_column("unit price"), 1);
  EXPECT_EQ(t.find_column("2"), 1);
  EXPECT_EQ(t.find_column("3"), 2); // a header named "3" wins over position 3 (same column here)
  EXPECT_EQ(t.find_column("1"), 0);
  EXPECT_EQ(t.find_column("0"), -1);
  EXPECT_EQ(t.find_column("4"), -1);
  EXPECT_EQ(t.find_column("nope"), -1);
  EXPECT_EQ(t.find_column(""), -1);
}
