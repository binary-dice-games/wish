// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/curl/client/curl_response_parser.hpp"

using bdg::wish::curl::parse_curl_output;

namespace {

// Regression test for the bug found end-to-end (automation module against a
// live httpbin.org GET, see DESIGN.md "Command construction & response
// parsing"): a JSON body that ends in its own trailing "\n", combined with
// the -w format string's leading "\n", produces a second blank-line
// boundary right before the sentinel. Parsing that boundary first (instead
// of stripping the sentinel first) silently drops the real body.
TEST(CurlResponseParserTest, BodyEndingInNewlineIsNotDroppedBeforeSentinel) {
  const std::string raw =
      "HTTP/2 200\r\n"
      "date: Mon, 21 Sep 2026 17:47:36 GMT\r\n"
      "content-type: application/json\r\n"
      "content-length: 282\r\n"
      "\r\n"
      "{\n"
      "  \"args\": {\n"
      "    \"probe\": \"1\"\n"
      "  }\n"
      "}\n"
      "\n" // the -w format's own leading "\n"
      "__WISH_CURL_META__\t200\t0.643103\t282\n";

  auto pr = parse_curl_output(raw);
  EXPECT_EQ(pr.status_code, 200);
  EXPECT_FLOAT_EQ(pr.time_ms, 643.103f);
  EXPECT_FLOAT_EQ(pr.size_bytes, 282.0f);
  EXPECT_EQ(pr.body, "{\n  \"args\": {\n    \"probe\": \"1\"\n  }\n}\n");
  ASSERT_EQ(pr.headers.size(), 3u);
  EXPECT_EQ(pr.headers[0].key, "date");
  EXPECT_EQ(pr.headers[2].key, "content-length");
  EXPECT_EQ(pr.headers[2].value, "282");
}

TEST(CurlResponseParserTest, BodyWithNoTrailingNewlineIsPreservedExactly) {
  const std::string raw =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "hello"
      "\n" // the -w format's own leading "\n" -- body itself has none
      "__WISH_CURL_META__\t200\t0.01\t5\n";

  auto pr = parse_curl_output(raw);
  EXPECT_EQ(pr.status_code, 200);
  EXPECT_EQ(pr.status_text, "OK");
  EXPECT_EQ(pr.body, "hello");
}

TEST(CurlResponseParserTest, FollowsRedirectsToTheFinalHopOnly) {
  const std::string raw =
      "HTTP/1.1 301 Moved Permanently\r\n"
      "Location: https://example.com/new\r\n"
      "\r\n"
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "final body\n"
      "__WISH_CURL_META__\t200\t0.02\t10\n";

  auto pr = parse_curl_output(raw);
  EXPECT_EQ(pr.status_code, 200);
  EXPECT_EQ(pr.status_text, "OK");
  EXPECT_EQ(pr.body, "final body");
  ASSERT_EQ(pr.headers.size(), 1u);
  EXPECT_EQ(pr.headers[0].key, "Content-Type");
  EXPECT_EQ(pr.headers[0].value, "text/plain");
}

TEST(CurlResponseParserTest, EmptyInputDoesNotCrash) {
  auto pr = parse_curl_output("");
  EXPECT_EQ(pr.status_code, 0);
  EXPECT_TRUE(pr.body.empty());
  EXPECT_TRUE(pr.headers.empty());
}

TEST(CurlResponseParserTest, MissingSentinelStillParsesWhateverWasCaptured) {
  // A connection-level failure (non-zero exit) never reaches this parser in
  // practice -- curl_source checks r.exit_code first -- but the parser
  // itself should still degrade gracefully on truncated/sentinel-less
  // output rather than crash.
  const std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\npartial";
  auto pr = parse_curl_output(raw);
  EXPECT_EQ(pr.status_code, 0);
  EXPECT_EQ(pr.body, "partial");
}

} // namespace
