// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/dev/pip/client/pip_parsers.hpp"

using bdg::wish::pip::error_summary;
using bdg::wish::pip::is_safe_arg;
using bdg::wish::pip::is_valid_package_name;
using bdg::wish::pip::parse_index_versions;
using bdg::wish::pip::parse_json_objects;
using bdg::wish::pip::split_requirements;

namespace {

// ── `pip list --format=json` parsing ────────────────────────────────────────

TEST(PipParsersTest, ParsesPipListJson) {
  const std::string out =
      R"([{"name": "attrs", "version": "25.4.0"}, )"
      R"({"name": "bison-abi", "version": "1.0.0", "editable_project_location": "/home/me/my src/python"},)"
      "\n"
      R"( {"name": "certifi", "version": "2026.1.4", "latest_version": "2026.7.22", "latest_filetype": "wheel"}])"
      "\n";
  auto rows = parse_json_objects(out);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].at("name"), "attrs");
  EXPECT_EQ(rows[0].at("version"), "25.4.0");
  EXPECT_EQ(rows[0].count("latest_version"), 0u);
  EXPECT_EQ(rows[1].at("editable_project_location"), "/home/me/my src/python");
  EXPECT_EQ(rows[2].at("latest_version"), "2026.7.22");
}

TEST(PipParsersTest, UnescapesStringsAndKeepsScalarsAndSkipsNestedValues) {
  auto rows = parse_json_objects(
      R"([{"path": "C:\\Users\\me\\\"x\"", "tab": "a\tb", "u": "caf\u00e9", "n": 12, "ok": true, "none": null,)"
      R"( "nested": {"a": [1, "]}"]}, "after": "kept"}])");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("path"), "C:\\Users\\me\\\"x\"");
  EXPECT_EQ(rows[0].at("tab"), "a\tb");
  EXPECT_EQ(rows[0].at("u"), "caf\xC3\xA9");
  EXPECT_EQ(rows[0].at("n"), "12");
  EXPECT_EQ(rows[0].at("ok"), "true");
  EXPECT_EQ(rows[0].at("none"), "null");
  EXPECT_EQ(rows[0].count("nested"), 0u);
  EXPECT_EQ(rows[0].at("after"), "kept");
}

TEST(PipParsersTest, EmptyOrMalformedJsonYieldsWhatWasReadSoFar) {
  EXPECT_TRUE(parse_json_objects("").empty());
  EXPECT_TRUE(parse_json_objects("[]").empty());
  EXPECT_TRUE(parse_json_objects("ERROR: something\n").empty());
  auto rows = parse_json_objects(R"([{"name": "a"}, {"name": "b)");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("name"), "a");
}

// ── `pip index versions` parsing ────────────────────────────────────────────

TEST(PipParsersTest, ParsesIndexVersions) {
  auto v = parse_index_versions(
      "requests (2.34.2)\n"
      "Available versions: 2.34.2, 2.34.1, 2.31.0\n"
      "  INSTALLED: 2.31.0\n"
      "  LATEST:    2.34.2\n");
  EXPECT_EQ(v.versions, (std::vector<std::string>{"2.34.2", "2.34.1", "2.31.0"}));
  EXPECT_EQ(v.installed, "2.31.0");
  EXPECT_EQ(v.latest, "2.34.2");
}

TEST(PipParsersTest, IndexVersionsOfAPackageThatIsNotInstalled) {
  auto v = parse_index_versions("six (1.17.0)\r\nAvailable versions: 1.17.0\r\n");
  EXPECT_EQ(v.versions, (std::vector<std::string>{"1.17.0"}));
  EXPECT_EQ(v.installed, "");
  EXPECT_EQ(v.latest, "");
  EXPECT_TRUE(parse_index_versions("").versions.empty());
}

// ── argument validation ─────────────────────────────────────────────────────

TEST(PipParsersTest, FlagShapedOrEmptyValuesAreNotSafeArgs) {
  EXPECT_TRUE(is_safe_arg("requests==2.31.0"));
  EXPECT_TRUE(is_safe_arg("git+https://github.com/psf/requests"));
  EXPECT_TRUE(is_safe_arg("./requirements.txt"));
  EXPECT_FALSE(is_safe_arg(""));
  EXPECT_FALSE(is_safe_arg("-r"));
  EXPECT_FALSE(is_safe_arg("--index-url=http://attacker.example"));
}

TEST(PipParsersTest, PackageNamesFollowPep508) {
  EXPECT_TRUE(is_valid_package_name("requests"));
  EXPECT_TRUE(is_valid_package_name("typing_extensions"));
  EXPECT_TRUE(is_valid_package_name("zope.interface"));
  EXPECT_TRUE(is_valid_package_name("Flask-SQLAlchemy2"));
  EXPECT_FALSE(is_valid_package_name(""));
  EXPECT_FALSE(is_valid_package_name("-y"));
  EXPECT_FALSE(is_valid_package_name("requests==2.0"));
  EXPECT_FALSE(is_valid_package_name("a b"));
  EXPECT_FALSE(is_valid_package_name("name-"));
  EXPECT_FALSE(is_valid_package_name("../etc"));
}

TEST(PipParsersTest, SplitsRequirementsOnWhitespace) {
  EXPECT_EQ(split_requirements("  numpy \t pandas==2.2\n"), (std::vector<std::string>{"numpy", "pandas==2.2"}));
  EXPECT_TRUE(split_requirements("   ").empty());
}

TEST(PipParsersTest, ErrorSummarySkipsWarningsBeforePipsErrorLine) {
  const std::string err =
      "WARNING: pip index is currently an experimental command.\n"
      "ERROR: No matching distribution found for nope\n";
  EXPECT_EQ(error_summary(err), "ERROR: No matching distribution found for nope");
  EXPECT_EQ(error_summary("ERROR: first\nERROR: second\n"), "ERROR: first\nERROR: second");
  EXPECT_EQ(error_summary("no such file\n"), "no such file");
}

TEST(PipParsersTest, ErrorSummaryExplainsAnExternallyManagedEnvironment) {
  auto text = error_summary(
      "error: externally-managed-environment\n\n× This environment is externally managed\n"
      "╰─> To install Python packages system-wide, try apt install\n");
  EXPECT_NE(text.find("virtualenv"), std::string::npos) << text;
  EXPECT_NE(text.find("--run=pip -- "), std::string::npos) << text;
  EXPECT_EQ(text.find("apt install"), std::string::npos) << text;
}

} // namespace
