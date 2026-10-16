// MIT License © 2026 Binary Dice Games
/// @file wish_test_main.cpp
/// @brief GoogleTest main for every wish test binary: points `WISH_HOME`
///        at a fresh per-process directory before any test runs.
///
/// Clients connect as the OS user by default and servers keep identified
/// sessions' files under the wish home (see src/context/DESIGN.md), so
/// without this a test would write into the developer's real `~/.wish`.
/// ctest runs every test case as its own process, so a per-process
/// directory is never shared between tests.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace {

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
  std::error_code ec;
  const auto home = std::filesystem::temp_directory_path(ec) /
      ("wish_test_home_" + std::to_string(std::random_device{}()) + "_" +
       std::to_string(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
  set_env("WISH_HOME", home.string());

  ::testing::InitGoogleTest(&argc, argv);
  const int rc = RUN_ALL_TESTS();
  std::filesystem::remove_all(home, ec);
  return rc;
}
