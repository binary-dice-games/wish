// MIT License © 2026 Binary Dice Games
/// @file test_nymph_cli.cpp
/// @brief nymph as a command line tool: the real `wish` binary run as
///        `wish standalone --renderer none --run=nymph -- ...`, checked by
///        its exit code, its stderr and the files it leaves.
#include <gtest/gtest.h>

#include "modules/bdg/dev/nymph/server/nymph_png_meta.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
namespace nymph = bdg::wish::nymph;

namespace {

const std::string kExample = "Monthly revenue against cost.\n"
                             "--\n"
                             "image:\n"
                             "  width: 320\n"
                             "  height: 200\n"
                             "root:\n"
                             "  type: Plot\n"
                             "  title: Revenue vs cost\n"
                             "  children:\n"
                             "    - type: PlotLine\n"
                             "      label: revenue\n"
                             "      xs: $month\n"
                             "      ys: $revenue\n"
                             "--\n"
                             "month, revenue\n1, 12\n2, 15\n3, 11\n";

class NymphCliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // One directory per test case: ctest runs each case as its own process,
    // in parallel.
    dir_ = fs::temp_directory_path() /
           (std::string("wish_nymph_cli_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void write(const std::string& name, const std::string& content) {
    std::ofstream out(dir_ / name, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  std::string read(const std::string& name) {
    std::ifstream in(dir_ / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
  }

  bool exists(const std::string& name) { return fs::exists(dir_ / name); }

  /// Runs `nymph <arguments>` in the test directory, with no display.
  /// stdout goes to "stdout.txt", stderr to "stderr.txt".
  /// @return The process exit code.
  int nymph_cli(const std::string& arguments) {
    std::string command = "cd \"" + dir_.string() + "\" && ";
#if !defined(_WIN32)
    command += "env -u DISPLAY -u WAYLAND_DISPLAY ";
#endif
    command += std::string("\"") + WISH_CLI_PATH + "\" standalone --renderer none --run=nymph -- " + arguments +
               " > stdout.txt 2> stderr.txt";
    int status = std::system(command.c_str());
#if !defined(_WIN32)
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
    return status;
#endif
  }

  fs::path dir_;
};

} // namespace

TEST_F(NymphCliTest, RenderWritesANymphPng) {
  write("ex.nymph", kExample);
  ASSERT_EQ(nymph_cli("render ex.nymph -o out.png"), 0) << read("stderr.txt");
  std::string png = read("out.png");
  ASSERT_TRUE(nymph::is_png(png));
  auto source = nymph::read_source(png);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(*source, kExample);
  EXPECT_EQ(read("stderr.txt"), "");
  // A command line tool leaves nothing else behind in the working directory.
  EXPECT_FALSE(exists("wish_logs"));
  EXPECT_FALSE(exists("out.png.nymph-tmp"));
}

TEST_F(NymphCliTest, DefaultOutputReplacesTheExtension) {
  write("chart.nymph", kExample);
  ASSERT_EQ(nymph_cli("render chart.nymph"), 0) << read("stderr.txt");
  EXPECT_TRUE(nymph::is_png(read("chart.png")));
}

TEST_F(NymphCliTest, APngInputIsRewrittenInPlaceIdentically) {
  write("ex.nymph", kExample);
  ASSERT_EQ(nymph_cli("render ex.nymph -o chart.png"), 0) << read("stderr.txt");
  std::string first = read("chart.png");
  ASSERT_EQ(nymph_cli("render chart.png"), 0) << read("stderr.txt");
  EXPECT_EQ(read("chart.png"), first);
}

TEST_F(NymphCliTest, ExtractPrintsTheSource) {
  write("ex.nymph", kExample);
  ASSERT_EQ(nymph_cli("render ex.nymph -o chart.png"), 0) << read("stderr.txt");
  ASSERT_EQ(nymph_cli("extract chart.png"), 0) << read("stderr.txt");
  EXPECT_EQ(read("stdout.txt"), kExample);
  ASSERT_EQ(nymph_cli("extract chart.png -o back.nymph"), 0) << read("stderr.txt");
  EXPECT_EQ(read("back.nymph"), kExample);
}

TEST_F(NymphCliTest, BadSourceFailsWithAPositionAndWritesNothing) {
  std::string bad = kExample;
  bad.replace(bad.find("$revenue"), 8, "$revenu");
  write("bad.nymph", bad);
  EXPECT_EQ(nymph_cli("render bad.nymph -o out.png"), 1);
  EXPECT_EQ(read("stderr.txt").rfind("nymph: bad.nymph:13:11: unknown column 'revenu'", 0), 0u) << read("stderr.txt");
  EXPECT_FALSE(exists("out.png"));
}

TEST_F(NymphCliTest, FailureLeavesAnExistingOutputUntouched) {
  write("ex.nymph", kExample);
  ASSERT_EQ(nymph_cli("render ex.nymph -o out.png"), 0) << read("stderr.txt");
  std::string good = read("out.png");
  write("bad.nymph", "d\n--\nroot:\n  type: Image\n--\n");
  EXPECT_EQ(nymph_cli("render bad.nymph -o out.png"), 1);
  EXPECT_EQ(read("out.png"), good);
}

TEST_F(NymphCliTest, MissingInputFails) {
  EXPECT_EQ(nymph_cli("render nope.nymph"), 1);
  EXPECT_EQ(read("stderr.txt").rfind("nymph: nope.nymph: ", 0), 0u) << read("stderr.txt");
  EXPECT_FALSE(exists("nope.png"));
}

TEST_F(NymphCliTest, ExtractFromAPlainPngFails) {
  nymph::image img;
  img.width = img.height = 4;
  img.rgba.assign(4 * 4 * 4, 128);
  std::string png = nymph::encode_png(img, "x", "");
  uint32_t length = 0;
  for (int i = 0; i < 4; ++i)
    length = length << 8 | static_cast<uint8_t>(png[33 + i]);
  png.erase(33, 12 + length); // remove the nymph chunk
  write("plain.png", png);
  EXPECT_EQ(nymph_cli("extract plain.png"), 1);
  EXPECT_NE(read("stderr.txt").find("not a nymph image"), std::string::npos) << read("stderr.txt");
}

TEST_F(NymphCliTest, UsageErrorFails) {
  EXPECT_EQ(nymph_cli("frobnicate x"), 1);
  EXPECT_NE(read("stderr.txt").find("usage: nymph render"), std::string::npos);
  EXPECT_EQ(nymph_cli("render"), 1);
}

#if !defined(_WIN32)
TEST_F(NymphCliTest, RenderReadsTheSourceFromTheConsole) {
  write("ex.nymph", kExample);
  ASSERT_EQ(nymph_cli("render - -o out.png < ex.nymph"), 0) << read("stderr.txt");
  auto source = nymph::read_source(read("out.png"));
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(*source, kExample);
}
#endif
