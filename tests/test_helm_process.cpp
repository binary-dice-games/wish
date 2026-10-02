// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/helm/client/helm_process.hpp"
#include "modules/bdg/dev/helm/client/helm_table_parser.hpp"

using bdg::wish::helm::error_summary;
using bdg::wish::helm::is_safe_arg;
using bdg::wish::helm::is_valid_release_name;
using bdg::wish::helm::parse_table;
using bdg::wish::helm::run_helm_cli;
using bdg::wish::helm::short_timestamp;

namespace {

// ── run_helm_cli() ──────────────────────────────────────────────────────────
//
// Exercised with stub binaries (never `helm`) so these pass on any machine,
// with or without helm installed -- see the `binary` parameter's doc comment
// in helm_process.hpp.

TEST(HelmProcessTest, CapturesStdoutFromAStubBinary) {
  auto r = run_helm_cli({"web \tprod"}, "printf");
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stdout_text, "web \tprod");
}

TEST(HelmProcessTest, ReportsNonZeroExitCode) {
  auto r = run_helm_cli({}, "false");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.exit_code, 1);
}

TEST(HelmProcessTest, MissingBinaryReportsSpawnFailure) {
  auto r = run_helm_cli({"version"}, "definitely-not-a-real-binary-xyzzy");
  EXPECT_EQ(r.exit_code, -1);
  EXPECT_FALSE(r.stderr_text.empty());
}

TEST(HelmProcessTest, EmptyBinaryIsRejected) {
  auto r = run_helm_cli({"version"}, "");
  EXPECT_EQ(r.exit_code, -1);
}

// ── helm table output parsing ───────────────────────────────────────────────

TEST(HelmTableParserTest, ParsesHelmListSkippingHeaderAndTrimmingPadding) {
  const std::string out =
      "NAME\tNAMESPACE\tREVISION\tUPDATED                                \tSTATUS  \tCHART       \tAPP VERSION\n"
      "web \tprod     \t3       \t2026-01-15 10:23:45.123456789 +0000 UTC\tdeployed\tnginx-15.1.0\t1.25.1     \n"
      "db  \tstaging  \t1       \t2026-01-16 08:00:00.5 +0000 UTC        \tfailed  \tpg-12.0.0   \t15.3       \n";
  auto rows = parse_table(out, "NAME", 7);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], (std::vector<std::string>{
                         "web", "prod", "3", "2026-01-15 10:23:45.123456789 +0000 UTC", "deployed", "nginx-15.1.0",
                         "1.25.1"}));
  EXPECT_EQ(rows[1][0], "db");
  EXPECT_EQ(rows[1][4], "failed");
}

TEST(HelmTableParserTest, KeepsSpacesInsideCellsAndPadsShortRows) {
  auto rows = parse_table(
      "REVISION\tUPDATED                 \tSTATUS  \tCHART\tAPP VERSION\tDESCRIPTION\n"
      "1       \tMon Jan 12 10:00:00 2026\tdeployed\tnginx-15.0.0\n",
      "REVISION", 6);
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows[0].size(), 6u);
  EXPECT_EQ(rows[0][1], "Mon Jan 12 10:00:00 2026");
  EXPECT_EQ(rows[0][5], "");
}

TEST(HelmTableParserTest, SkipsBlankLinesAndPlainMessages) {
  EXPECT_TRUE(parse_table("No results found\n", "NAME", 4).empty());
  EXPECT_TRUE(parse_table("", "NAME", 4).empty());
  EXPECT_TRUE(parse_table("NAME\tURL\n\n", "NAME", 2).empty());
}

TEST(HelmTableParserTest, OnlyALeadingRowIsTreatedAsTheHeader) {
  // A repository really named "NAME" must survive.
  auto rows = parse_table("NAME\tURL\nNAME\thttps://example.com\r\n", "NAME", 2);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0][1], "https://example.com");
}

TEST(HelmTableParserTest, ShortTimestampDropsFractionAndZone) {
  EXPECT_EQ(short_timestamp("2026-01-15 10:23:45.123456789 +0000 UTC"), "2026-01-15 10:23:45");
  EXPECT_EQ(short_timestamp("Mon Jan 12 10:00:00 2026"), "Mon Jan 12 10:00:00 2026");
  EXPECT_EQ(short_timestamp(""), "");
}

TEST(HelmTableParserTest, FlagShapedOrEmptyValuesAreNotSafeArgs) {
  EXPECT_TRUE(is_safe_arg("web"));
  EXPECT_TRUE(is_safe_arg("bitnami/nginx"));
  EXPECT_TRUE(is_safe_arg("https://charts.example.com"));
  EXPECT_FALSE(is_safe_arg(""));
  EXPECT_FALSE(is_safe_arg("-n"));
  EXPECT_FALSE(is_safe_arg("--post-renderer=/tmp/x"));
}

TEST(HelmTableParserTest, ErrorSummarySkipsWarningsBeforeHelmsErrorLine) {
  const std::string err =
      "I1002 10:07:02.046785 116395 warnings.go:107] \"Warning: metadata.name: a DNS label is recommended\"\n"
      "Error: INSTALLATION FAILED: Service \"v1.0-podinfo\" is invalid\n";
  EXPECT_EQ(error_summary(err), "Error: INSTALLATION FAILED: Service \"v1.0-podinfo\" is invalid");
  EXPECT_EQ(error_summary("Error: no repositories to show\n"), "Error: no repositories to show");
  EXPECT_EQ(error_summary("exec: not found\n"), "exec: not found");
}

TEST(HelmTableParserTest, ReleaseNamesMustBeDnsLabels) {
  EXPECT_TRUE(is_valid_release_name("my-app"));
  EXPECT_TRUE(is_valid_release_name("demo2"));
  EXPECT_FALSE(is_valid_release_name("v1.0"));
  EXPECT_FALSE(is_valid_release_name("MyApp"));
  EXPECT_FALSE(is_valid_release_name("-app"));
  EXPECT_FALSE(is_valid_release_name("app-"));
  EXPECT_FALSE(is_valid_release_name(""));
  EXPECT_FALSE(is_valid_release_name(std::string(54, 'a')));
}

} // namespace
