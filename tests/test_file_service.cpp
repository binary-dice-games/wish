// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <context/file_service.hpp>
#include <net/http_client.hpp>

#include <civetweb.h>
#include <miniz.h>
#include <miniz_zip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace bdg::bison;
using bdg::wish::file_service;
using bdg::wish::context;
using namespace std::chrono_literals;

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

// Minimal throwaway HTTP server for resolve_or_fetch()'s URL-download tests:
// binds an ephemeral localhost port and serves a single configurable body
// (or error status) from every request, counting how many requests arrive so
// tests can assert on in-flight-dedup / no-retry-after-failure behavior.
class TestHttpServer {
 public:
  TestHttpServer() {
    mg_callbacks callbacks{};
    const char* options[] = {"listening_ports", "127.0.0.1:0", "num_threads", "4", nullptr};
    ctx_ = mg_start(&callbacks, this, options);
    mg_set_request_handler(ctx_, "/", &TestHttpServer::handle, this);
  }
  ~TestHttpServer() {
    if (ctx_)
      mg_stop(ctx_);
  }

  std::string url(const std::string& path) const {
    mg_server_port ports[4];
    int n = mg_get_server_ports(ctx_, 4, ports);
    int port = n > 0 ? ports[0].port : 0;
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }

  std::atomic<int> request_count{0};
  std::string body{"fake-resource-bytes"};
  int status_code = 200;
  int delay_ms = 0;

 private:
  static int handle(mg_connection* conn, void* cbdata) {
    auto* self = static_cast<TestHttpServer*>(cbdata);
    ++self->request_count;
    if (self->delay_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(self->delay_ms));
    if (self->status_code != 200) {
      mg_send_http_error(conn, self->status_code, "test error");
      return 1;
    }
    mg_send_http_ok(conn, "application/octet-stream", static_cast<long long>(self->body.size()));
    mg_write(conn, self->body.data(), self->body.size());
    return 1;
  }

  mg_context* ctx_ = nullptr;
};

// Polls @p fn (expected to return a possibly-empty std::filesystem::path)
// until it returns non-empty or @p timeout elapses; returns the last result.
template <typename Fn>
std::filesystem::path poll_until_nonempty(Fn&& fn, std::chrono::milliseconds timeout = 2000ms) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  std::filesystem::path result;
  do {
    result = fn();
    if (!result.empty())
      return result;
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return result;
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

// ── net::http_get ────────────────────────────────────────────────────────────

TEST(HttpGetTest, FetchesBodyOnSuccess) {
  TestHttpServer server;
  server.body = "hello from server";

  auto resp = bdg::wish::net::http_get(server.url("/x.txt"));
  EXPECT_TRUE(resp.ok);
  EXPECT_EQ(resp.body, "hello from server");
}

TEST(HttpGetTest, NonSuccessStatusIsNotOk) {
  TestHttpServer server;
  server.status_code = 404;

  auto resp = bdg::wish::net::http_get(server.url("/missing.txt"));
  EXPECT_FALSE(resp.ok);
}

TEST(HttpGetTest, RejectsResponseLargerThanMaxBytes) {
  TestHttpServer server;
  server.body = std::string(1024, 'x');

  auto resp = bdg::wish::net::http_get(server.url("/big.bin"), /*max_bytes=*/16);
  EXPECT_FALSE(resp.ok);
}

TEST(HttpGetTest, TimesOutOnSlowServer) {
  TestHttpServer server;
  server.delay_ms = 300;

  auto resp = bdg::wish::net::http_get(server.url("/slow.bin"), /*max_bytes=*/1 << 20, /*timeout_ms=*/50);
  EXPECT_FALSE(resp.ok);
}

TEST(HttpGetTest, InvalidUrlIsRejected) {
  auto resp = bdg::wish::net::http_get("not-a-url");
  EXPECT_FALSE(resp.ok);
}

// ── file_service::resolve_or_fetch ──────────────────────────────────────────

TEST_F(FileServiceTest, ResolveOrFetchLocalNameMatchesResolvePath) {
  auto expected = file_service::resolve_path("res/icons/foo.png", sess().resource_dir, false);
  auto actual = file_service::resolve_or_fetch("res/icons/foo.png", sess().resource_dir, false, true, {"png"});
  EXPECT_EQ(actual, expected);
}

TEST_F(FileServiceTest, ResolveOrFetchLocalPathTraversalRejected) {
  auto actual = file_service::resolve_or_fetch("../evil.png", sess().resource_dir, false, true, {"png"});
  EXPECT_TRUE(actual.empty());
}

TEST_F(FileServiceTest, ResolveOrFetchFileSchemeRequiresAllowAbsolute) {
  auto path = (sess().resource_dir / "abs.png").string();
  // file:// is governed by allow_absolute only -- allow_fetch is irrelevant
  // and left false here to prove that.
  EXPECT_TRUE(file_service::resolve_or_fetch("file://" + path, sess().resource_dir, false, false, {"png"}).empty());
  EXPECT_EQ(
      file_service::resolve_or_fetch("file://" + path, sess().resource_dir, true, false, {"png"}),
      std::filesystem::path(path));
}

TEST_F(FileServiceTest, ResolveOrFetchRejectsUrlsWhenFetchDisabled) {
  TestHttpServer server;

  auto result = file_service::resolve_or_fetch(server.url("/photo.png"), sess().resource_dir, false, false, {"png"});
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(server.request_count.load(), 0);
}

TEST_F(FileServiceTest, ResolveOrFetchRejectsDisallowedExtensionWithoutNetworkIo) {
  TestHttpServer server;

  auto result =
      file_service::resolve_or_fetch(server.url("/payload.exe"), sess().resource_dir, false, true, {"png", "jpg"});
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(server.request_count.load(), 0);
}

TEST_F(FileServiceTest, ResolveOrFetchDownloadsUrlAsynchronouslyAndCaches) {
  TestHttpServer server;
  server.body = "png-bytes";
  server.delay_ms = 150;
  auto url = server.url("/photo.png");

  auto start = std::chrono::steady_clock::now();
  auto first = file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"});
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(first.empty());
  EXPECT_LT(elapsed, 100ms); // must not block on the 150ms-delayed server

  auto resolved = poll_until_nonempty(
      [&] { return file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"}); });
  ASSERT_FALSE(resolved.empty());
  EXPECT_EQ(read_binary_file(resolved), "png-bytes");

  // Cached: a later call returns immediately without another request.
  int count_after_first_download = server.request_count.load();
  auto cached = file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"});
  EXPECT_EQ(cached, resolved);
  EXPECT_EQ(server.request_count.load(), count_after_first_download);
}

TEST_F(FileServiceTest, ResolveOrFetchDedupsConcurrentFramesToOneRequest) {
  TestHttpServer server;
  server.delay_ms = 150;
  auto url = server.url("/shared.png");

  for (int i = 0; i < 20; ++i) {
    auto result = file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"});
    EXPECT_TRUE(result.empty()); // still downloading throughout this burst
  }

  auto resolved = poll_until_nonempty(
      [&] { return file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"}); });
  ASSERT_FALSE(resolved.empty());
  EXPECT_EQ(server.request_count.load(), 1);
}

TEST_F(FileServiceTest, ResolveOrFetchDoesNotRetryAfterFailure) {
  TestHttpServer server;
  server.status_code = 404;
  auto url = server.url("/gone.png");

  file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"});
  // Give the background thread time to run and record the failure.
  std::this_thread::sleep_for(200ms);
  ASSERT_EQ(server.request_count.load(), 1);

  for (int i = 0; i < 10; ++i) {
    auto result = file_service::resolve_or_fetch(url, sess().resource_dir, false, true, {"png"});
    EXPECT_TRUE(result.empty());
  }
  EXPECT_EQ(server.request_count.load(), 1);
}
