// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/bdg/desktop/git/client/git_process.hpp"

#include <filesystem>

using bdg::wish::git::resolve_repo_root;
using bdg::wish::git::run_git;

namespace {

// Real, throwaway `git init`'d repo -- this module never mocks git (see
// DESIGN.md's Design Goal 1), so resolve_repo_root() is exercised against
// an actual `git rev-parse --show-toplevel` subprocess, same as production.
class GitProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / "wish_git_process_test_repo";
    std::filesystem::remove_all(root_);
    subdir_ = root_ / "a" / "b";
    std::filesystem::create_directories(subdir_);

    ASSERT_TRUE(run_git(root_.string(), {"init", "--quiet"}).ok());
    // std::filesystem::canonical() resolves any symlinks in the platform's
    // own temp dir (e.g. /tmp -> /private/tmp on macOS) so this matches
    // exactly what `git rev-parse --show-toplevel` itself reports, which
    // does the same resolution.
    canonical_root_ = std::filesystem::canonical(root_).string();
  }

  void TearDown() override {
    std::filesystem::remove_all(root_);
  }

  std::filesystem::path root_;
  std::filesystem::path subdir_;
  std::string canonical_root_;
};

TEST_F(GitProcessTest, ResolvesAbsoluteSubdirectoryToRepoRoot) {
  EXPECT_EQ(resolve_repo_root(subdir_.string()), canonical_root_);
}

TEST_F(GitProcessTest, ResolvesRepoRootItselfToItself) {
  EXPECT_EQ(resolve_repo_root(root_.string()), canonical_root_);
}

TEST_F(GitProcessTest, FallsBackToInputPathWhenNotInsideAWorkTree) {
  const auto outside = std::filesystem::temp_directory_path() / "wish_git_process_test_outside";
  std::filesystem::remove_all(outside);
  std::filesystem::create_directories(outside);
  EXPECT_EQ(resolve_repo_root(outside.string()), outside.string());
  std::filesystem::remove_all(outside);
}

} // namespace
