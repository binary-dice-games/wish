// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/curl/client/curl_process.hpp"

using bdg::wish::curl::run_curl_cli;

namespace {

// run_curl_cli() is exercised with stub binaries (never `curl`) so these
// pass on any machine, with or without network access -- see the `binary`
// parameter's doc comment in curl_process.hpp.

TEST(CurlProcessTest, CapturesStdoutFromAStubBinary) {
  auto r = run_curl_cli({"hello\tworld"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stdout_text, "hello\tworld");
}

TEST(CurlProcessTest, ReportsNonZeroExitCode) {
  auto r = run_curl_cli({}, "false");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.exit_code, 1);
}

TEST(CurlProcessTest, MissingBinaryReportsSpawnFailure) {
  auto r = run_curl_cli({"--version"}, "definitely-not-a-real-binary-xyzzy");
  EXPECT_EQ(r.exit_code, -1);
  EXPECT_FALSE(r.stderr_text.empty());
}

TEST(CurlProcessTest, EmptyBinaryIsRejected) {
  auto r = run_curl_cli({"--version"}, "");
  EXPECT_EQ(r.exit_code, -1);
}

TEST(CurlProcessTest, ArgsWithSpacesNeedNoEscaping) {
  // No shell in the pipeline, so a space-containing arg (e.g. a header
  // value or JSON body) is one argv entry.
  auto r = run_curl_cli({"%s\n", "a b c"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "a b c\n");
}

} // namespace
