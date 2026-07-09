// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <file_service.hpp>

#include <filesystem>
#include <string>
#include <unordered_set>

using namespace bdg::bison;
using bdg::wish::file_service;
using bdg::wish::context;

class FileServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_file_service(); // idempotent: registers bison class
    sess_ = std::make_unique<context>("fs_test"_key);
    sess_->file_service =
        std::make_shared<file_service>(dynamic::instantiate("wish"_key, "__WishFileSystem"_key), sess_->resource_dir);
  }

  context& sess() {
    return *sess_;
  }
  file_service& fs() {
    return *sess_->file_service;
  }

 private:
  std::unique_ptr<context> sess_;
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

TEST_F(FileServiceTest, ListSubdirectoryReturnsOnlyItsOwnFiles) {
  fs().upload("root.txt", "r");
  fs().upload("icons/file.png", "f");
  fs().upload("icons/folder.png", "d");

  auto listing = fs().list("icons");
  ASSERT_NE(listing, nullptr);

  std::unordered_set<std::string> found;
  listing->forEach([&](bdg::bison::key_t, const field& f) {
    if (f.is<std::string>())
      found.insert(f.as<std::string>());
  });

  EXPECT_EQ(found.size(), 2U);
  EXPECT_NE(found.find("file.png"), found.end());
  EXPECT_NE(found.find("folder.png"), found.end());
  EXPECT_EQ(found.find("root.txt"), found.end());
}

TEST_F(FileServiceTest, ListNestedSubdirectoryWorks) {
  fs().upload("a/b/c.txt", "deep");

  auto listing = fs().list("a/b");
  std::unordered_set<std::string> found;
  listing->forEach([&](bdg::bison::key_t, const field& f) {
    if (f.is<std::string>())
      found.insert(f.as<std::string>());
  });

  EXPECT_NE(found.find("c.txt"), found.end());
}

TEST_F(FileServiceTest, ListEmptyPathListsResourceDirRoot) {
  fs().upload("alpha.txt", "a");
  fs().upload("sub/beta.txt", "b");

  auto listing = fs().list("");
  std::unordered_set<std::string> found;
  listing->forEach([&](bdg::bison::key_t, const field& f) {
    if (f.is<std::string>())
      found.insert(f.as<std::string>());
  });

  EXPECT_NE(found.find("alpha.txt"), found.end());
  EXPECT_EQ(found.find("beta.txt"), found.end()); // nested; not listed from root
}

TEST_F(FileServiceTest, ListPathTraversalThrows) {
  EXPECT_THROW(fs().list("../escape"), std::runtime_error);
}

TEST_F(FileServiceTest, ListMissingSubdirectoryReturnsEmpty) {
  auto listing = fs().list("does_not_exist");
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
