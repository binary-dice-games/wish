// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <context/file_service.hpp>

#include <miniz.h>
#include <miniz_zip.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace bdg::bison;
using bdg::wish::file_service;
using bdg::wish::context;

namespace {

// Builds a zip archive on disk containing the given (name -> content)
// entries, for exercising file_service::unpack() without depending on any
// pre-existing fixture file.
std::filesystem::path make_test_zip(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries) {
  mz_zip_archive zip{};
  EXPECT_TRUE(mz_zip_writer_init_file(&zip, path.string().c_str(), 0));
  for (const auto& [name, content] : entries) {
    EXPECT_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
  }
  EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
  mz_zip_writer_end(&zip);
  return path;
}

std::string read_binary_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

} // namespace

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

// ── upload_chunk / download_chunk ───────────────────────────────────────────────

TEST_F(FileServiceTest, UploadChunkRoundTrip) {
  fs().upload_chunk("big.txt", "hello, ", /*first=*/true, /*eof=*/false);
  fs().upload_chunk("big.txt", "world!", /*first=*/false, /*eof=*/true);
  EXPECT_EQ(fs().download("big.txt"), "hello, world!");
}

TEST_F(FileServiceTest, UploadChunkSingleChunkRoundTrip) {
  fs().upload_chunk("one.txt", "solo", /*first=*/true, /*eof=*/true);
  EXPECT_EQ(fs().download("one.txt"), "solo");
}

TEST_F(FileServiceTest, UploadChunkEmptyFileCreatesEmptyFile) {
  fs().upload_chunk("empty.txt", "", /*first=*/true, /*eof=*/true);
  EXPECT_EQ(fs().download("empty.txt"), "");
}

TEST_F(FileServiceTest, UploadChunkOverwritesExistingFileOnlyOnEof) {
  fs().upload("existing.txt", "old content");
  fs().upload_chunk("existing.txt", "new ", /*first=*/true, /*eof=*/false);
  // Not finalized yet -- the visible file must still hold the old content.
  EXPECT_EQ(fs().download("existing.txt"), "old content");
  fs().upload_chunk("existing.txt", "content", /*first=*/false, /*eof=*/true);
  EXPECT_EQ(fs().download("existing.txt"), "new content");
}

TEST_F(FileServiceTest, UploadChunkPathTraversalThrows) {
  EXPECT_THROW(fs().upload_chunk("../evil.txt", "x", true, true), std::runtime_error);
}

TEST_F(FileServiceTest, DownloadChunkReadsAtOffsetAndReportsEof) {
  fs().upload("range.txt", "0123456789");

  auto c0 = fs().download_chunk("range.txt", 0, 4);
  EXPECT_EQ(c0.data, "0123");
  EXPECT_FALSE(c0.eof);

  auto c1 = fs().download_chunk("range.txt", 4, 4);
  EXPECT_EQ(c1.data, "4567");
  EXPECT_FALSE(c1.eof);

  auto c2 = fs().download_chunk("range.txt", 8, 4);
  EXPECT_EQ(c2.data, "89");
  EXPECT_TRUE(c2.eof);
}

TEST_F(FileServiceTest, DownloadChunkExactlyOnEofBoundaryReportsEof) {
  fs().upload("exact.txt", "abcd");
  auto c = fs().download_chunk("exact.txt", 0, 4);
  EXPECT_EQ(c.data, "abcd");
  EXPECT_TRUE(c.eof);
}

TEST_F(FileServiceTest, DownloadChunkEmptyFileReportsEofImmediately) {
  fs().upload("empty2.txt", "");
  auto c = fs().download_chunk("empty2.txt", 0, 16);
  EXPECT_EQ(c.data, "");
  EXPECT_TRUE(c.eof);
}

TEST_F(FileServiceTest, DownloadChunkPastEndReturnsEmptyAndEof) {
  fs().upload("short.txt", "abc");
  auto c = fs().download_chunk("short.txt", 10, 4);
  EXPECT_EQ(c.data, "");
  EXPECT_TRUE(c.eof);
}

TEST_F(FileServiceTest, DownloadChunkMissingFileThrows) {
  EXPECT_THROW(fs().download_chunk("ghost.txt", 0, 16), std::runtime_error);
}

// ── unpack ───────────────────────────────────────────────────────────────────

TEST_F(FileServiceTest, UnpackExtractsZipEntriesIntoDest) {
  auto zip_path = sess().resource_dir / "staging.zip";
  make_test_zip(zip_path, {{"a.txt", "alpha"}, {"sub/b.txt", "beta"}});
  // Land the archive in the sandbox the same way a real upload would.
  fs().upload("pkg.zip", read_binary_file(zip_path));

  fs().unpack("pkg.zip", "dest_dir");

  EXPECT_EQ(fs().download("dest_dir/a.txt"), "alpha");
  EXPECT_EQ(fs().download("dest_dir/sub/b.txt"), "beta");
}

TEST_F(FileServiceTest, UnpackMergesIntoExistingDestDirectory) {
  auto zip_path = sess().resource_dir / "staging2.zip";
  make_test_zip(zip_path, {{"new.txt", "new"}});
  fs().upload("pkg2.zip", read_binary_file(zip_path));
  fs().upload("dest2/keep.txt", "keep me");

  fs().unpack("pkg2.zip", "dest2");

  EXPECT_EQ(fs().download("dest2/new.txt"), "new");
  EXPECT_EQ(fs().download("dest2/keep.txt"), "keep me");
}

TEST_F(FileServiceTest, UnpackRemovesStagingZipAfterSuccess) {
  auto zip_path = sess().resource_dir / "staging3.zip";
  make_test_zip(zip_path, {{"a.txt", "alpha"}});
  fs().upload("pkg3.zip", read_binary_file(zip_path));

  fs().unpack("pkg3.zip", "dest3");

  EXPECT_THROW(fs().download("pkg3.zip"), std::runtime_error);
}

TEST_F(FileServiceTest, UnpackRejectsZipSlipEntry) {
  auto zip_path = sess().resource_dir / "evil.zip";
  make_test_zip(zip_path, {{"../escape.txt", "pwned"}});
  fs().upload("evil_pkg.zip", read_binary_file(zip_path));

  EXPECT_THROW(fs().unpack("evil_pkg.zip", "dest4"), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(sess().resource_dir / "escape.txt"));
}

TEST_F(FileServiceTest, UnpackThrowsOnCorruptArchive) {
  fs().upload("not_a_zip.zip", "this is not a zip file");
  EXPECT_THROW(fs().unpack("not_a_zip.zip", "dest5"), std::runtime_error);
}

TEST_F(FileServiceTest, UnpackPathTraversalOnZipNameThrows) {
  EXPECT_THROW(fs().unpack("../evil.zip", "dest6"), std::runtime_error);
}

TEST_F(FileServiceTest, UnpackPathTraversalOnDestThrows) {
  auto zip_path = sess().resource_dir / "staging4.zip";
  make_test_zip(zip_path, {{"a.txt", "alpha"}});
  fs().upload("pkg4.zip", read_binary_file(zip_path));

  EXPECT_THROW(fs().unpack("pkg4.zip", "../evil_dest"), std::runtime_error);
}
