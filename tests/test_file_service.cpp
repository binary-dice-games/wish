// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/file_service.hpp>

#include <filesystem>
#include <string>
#include <unordered_set>

using namespace bdg::bison;
using bdg::wish::file_service;
using bdg::wish::session;

class FileServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_file_service(); // idempotent: registers bison class
    sess_ = std::make_unique<session>("fs_test"_key);
    sess_->file_service =
        std::make_shared<file_service>(dynamic::instantiate("wish"_key, "__WishFileSystem"_key), sess_->resource_dir);
  }

  session& sess() {
    return *sess_;
  }
  file_service& fs() {
    return *sess_->file_service;
  }

 private:
  std::unique_ptr<session> sess_;
};

// ── Round-trip ────────────────────────────────────────────────────────────────

TEST_F(FileServiceTest, UploadDownloadRoundTrip) {
  fs().upload("hello.txt", "world");
  EXPECT_EQ(fs().download("hello.txt"), "world");
}

TEST_F(FileServiceTest, UploadDownloadBinaryContent) {
  std::string binary{'\x00', '\x01', '\xFF', '\xFE'};
  fs().upload("bin.dat", binary);
  EXPECT_EQ(fs().download("bin.dat"), binary);
}

// ── list ─────────────────────────────────────────────────────────────────────

TEST_F(FileServiceTest, ListReturnsUploadedFileNames) {
  fs().upload("alpha.txt", "a");
  fs().upload("beta.txt", "b");

  auto listing = fs().list();
  ASSERT_NE(listing, nullptr);

  std::unordered_set<std::string> found;
  listing->forEach([&](bdg::bison::key_t, const field& f) {
    if (f.is<std::string>())
      found.insert(f.as<std::string>());
  });

  EXPECT_NE(found.find("alpha.txt"), found.end());
  EXPECT_NE(found.find("beta.txt"), found.end());
}

TEST_F(FileServiceTest, ListEmptyWhenNoFiles) {
  auto listing = fs().list();
  int count = 0;
  listing->forEach([&](bdg::bison::key_t, const field& f) {
    if (f.is<std::string>())
      ++count;
  });
  EXPECT_EQ(count, 0);
}

// ── erase ─────────────────────────────────────────────────────────────────────

TEST_F(FileServiceTest, EraseRemovesFile) {
  fs().upload("temp.txt", "data");
  fs().erase("temp.txt");
  EXPECT_THROW(fs().download("temp.txt"), std::runtime_error);
}

TEST_F(FileServiceTest, EraseNonExistentFileThrows) {
  EXPECT_THROW(fs().erase("ghost.txt"), std::runtime_error);
}

// ── subdirectory support ──────────────────────────────────────────────────────

TEST_F(FileServiceTest, SubdirectoryUploadDownloadWorks) {
  fs().upload("subdir/file.txt", "hello");
  EXPECT_EQ(fs().download("subdir/file.txt"), "hello");
}

TEST_F(FileServiceTest, NestedSubdirectoriesWork) {
  fs().upload("a/b/c.txt", "deep");
  EXPECT_EQ(fs().download("a/b/c.txt"), "deep");
}

TEST_F(FileServiceTest, SubdirectoryEraseWorks) {
  fs().upload("sub/temp.txt", "data");
  fs().erase("sub/temp.txt");
  EXPECT_THROW(fs().download("sub/temp.txt"), std::runtime_error);
}

// ── sandbox enforcement ───────────────────────────────────────────────────────

TEST_F(FileServiceTest, PathTraversalUploadThrows) {
  EXPECT_THROW(fs().upload("../evil.txt", "x"), std::runtime_error);
}

TEST_F(FileServiceTest, PathTraversalDownloadThrows) {
  EXPECT_THROW(fs().download("../secret.txt"), std::runtime_error);
}

TEST_F(FileServiceTest, PathTraversalFromSubdirThrows) {
  EXPECT_THROW(fs().upload("sub/../../evil.txt", "x"), std::runtime_error);
}

TEST_F(FileServiceTest, DotDotAloneThrows) {
  EXPECT_THROW(fs().upload("..", "x"), std::runtime_error);
}

// ── resource_dir gone ────────────────────────────────────────────────────────

TEST_F(FileServiceTest, UploadToDeletedResourceDirThrows) {
  std::filesystem::remove_all(sess().resource_dir);
  EXPECT_THROW(fs().upload("file.txt", "data"), std::runtime_error);
}
