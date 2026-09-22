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

// Regression test for a second, distinct bug found end-to-end (automation
// module against a live GET https://www.google.com): a large HTML body can
// easily contain its own "\n\n" sequence (routine in minified HTML/JS) --
// scanning the *whole* text for every blank-line occurrence and picking the
// last one misreads that body-internal blank line as an extra `-L` redirect
// hop, truncating the real body to just whatever followed it and misparsing
// a large chunk of the actual body as "headers". The fix only ever treats a
// blank line as a boundary when reached by walking forward from a position
// that itself starts with "HTTP/".
TEST(CurlResponseParserTest, BodyContainingItsOwnBlankLineIsNotTruncated) {
  const std::string raw =
      "HTTP/2 200\r\n"
      "content-type: text/html; charset=ISO-8859-1\r\n"
      "server: gws\r\n"
      "\r\n"
      "<!doctype html><html><head></head><body>\n"
      "\n" // a blank line occurring naturally inside the HTML body
      "<div>content after the embedded blank line</div>\n"
      "</body></html>"
      "\n" // the -w format's own leading "\n" -- body itself has none of its own
      "__WISH_CURL_META__\t200\t0.53\t84819\n";

  auto pr = parse_curl_output(raw);
  EXPECT_EQ(pr.status_code, 200);
  EXPECT_FLOAT_EQ(pr.size_bytes, 84819.0f);
  EXPECT_EQ(
      pr.body,
      "<!doctype html><html><head></head><body>\n"
      "\n"
      "<div>content after the embedded blank line</div>\n"
      "</body></html>");
  ASSERT_EQ(pr.headers.size(), 2u);
  EXPECT_EQ(pr.headers[0].key, "content-type");
  EXPECT_EQ(pr.headers[1].key, "server");
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
