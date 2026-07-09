// MIT License © 2025 Binary Dice Games
// Tests for wish::logger — the per-session RMI logging service.
#include <gtest/gtest.h>

#include <context/logger.hpp>
#include <server/registry.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace bdg::bison;

// ── Fixture ───────────────────────────────────────────────────────────────────

class LoggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    log_path_ = std::filesystem::temp_directory_path() / "wish_test_logger.log";
    std::filesystem::remove(log_path_); // start clean
  }

  void TearDown() override {
    std::filesystem::remove(log_path_);
  }

  bdg::wish::logger make_logger(bool verbose = false) {
    return bdg::wish::logger{dynamic::instantiate("wish"_key, "__WishLogger"_key), verbose, log_path_};
  }

  std::string read_log() const {
    std::ifstream f(log_path_);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{}};
  }

  std::filesystem::path log_path_;
};

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(LoggerTest, ConstructionCreatesLogFile) {
  {
    auto lg = make_logger();
  }
  EXPECT_TRUE(std::filesystem::exists(log_path_));
}

// ── C++ API ───────────────────────────────────────────────────────────────────

TEST_F(LoggerTest, InfoWritesToFile) {
  auto lg = make_logger();
  lg.info("hello");
  const auto content = read_log();
  EXPECT_NE(content.find("[info]"), std::string::npos);
  EXPECT_NE(content.find("hello"), std::string::npos);
}

TEST_F(LoggerTest, DebugWarnErrorLevelsAppearInFile) {
  auto lg = make_logger();
  lg.debug("dbg");
  lg.warn("wrn");
  lg.error("err");
  const auto content = read_log();
  EXPECT_NE(content.find("[debug]"), std::string::npos);
  EXPECT_NE(content.find("[warn]"), std::string::npos);
  EXPECT_NE(content.find("[error]"), std::string::npos);
}

TEST_F(LoggerTest, MultipleMessagesAllAppearInFile) {
  auto lg = make_logger();
  lg.info("first");
  lg.info("second");
  lg.info("third");
  const auto content = read_log();
  EXPECT_NE(content.find("first"), std::string::npos);
  EXPECT_NE(content.find("second"), std::string::npos);
  EXPECT_NE(content.find("third"), std::string::npos);
}

// ── Log generic level ─────────────────────────────────────────────────────────

TEST_F(LoggerTest, GenericLogMethodWithCustomLevel) {
  auto lg = make_logger();
  lg.log("warn", "rmi message");
  const auto content = read_log();
  EXPECT_NE(content.find("[warn]"), std::string::npos);
  EXPECT_NE(content.find("rmi message"), std::string::npos);
}

// ── No file path ──────────────────────────────────────────────────────────────

TEST_F(LoggerTest, EmptyLogPathDoesNotCreateFile) {
  bdg::wish::logger lg{dynamic::instantiate("wish"_key, "__WishLogger"_key), false, {}};
  lg.info("should not appear on disk");
  EXPECT_FALSE(std::filesystem::exists(log_path_));
}
