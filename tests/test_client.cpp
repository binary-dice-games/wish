// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <client/client.hpp>
#include <server/server.hpp>
#include <ui/ui_descriptor.hpp>

#include "src/rmi/rmi.hpp"

#include <miniz.h>
#include <miniz_zip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Test helpers ──────────────────────────────────────────────────────────────

class tracking_client : public wish::client {
 public:
  using wish::client::client;

  std::atomic<bool> session_fired{false};

 protected:
  void on_session() override {
    session_fired.store(true, std::memory_order_release);
  }
};

// Builds a zip archive on disk containing the given (name -> content)
// entries, for exercising client::upload_package() without depending on
// any pre-existing fixture file.
static std::filesystem::path make_test_zip(
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

// Minimal JSON descriptor helpers used across tests.
static const char* kWindowJson = R"({"type":"Window","title":"Root"})";

static const char* kWindowWithChildJson =
    R"({"type":"Window","title":"Root","children":{"lbl":{"type":"Label","text":"Hi"}}})";

// ── run() lifecycle ───────────────────────────────────────────────────────────

TEST(ClientTest, RunCallsOnSessionAndDisconnects) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  tracking_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.session_fired.load(std::memory_order_acquire));
  srv.stop();
}

TEST(ClientTest, RunDisconnectsOnException) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class throwing_client : public wish::client {
   public:
    using wish::client::client;

   protected:
    void on_session() override {
      throw std::runtime_error("test error");
    }
  };

  throwing_client c{transport.connect()};
  EXPECT_THROW(c.run(), std::runtime_error);
  srv.stop();
}

// ── Inherited bison RMI operations ───────────────────────────────────────────

TEST(ClientTest, InheritedInstantiateWorks) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class rmi_client : public wish::client {
   public:
    using wish::client::client;

   protected:
    void on_session() override {}
  };

  rmi_client c{transport.connect()};
  c.connect();
  auto proxy = c.instantiate("wish"_key, "Window"_key).get();
  EXPECT_TRUE(proxy.valid());
  EXPECT_NE(proxy.id(), 0u);
  c.disconnect();
  srv.stop();
}

TEST(ClientTest, InheritedSetGetRoundTrips) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class rmi_client : public wish::client {
   public:
    using wish::client::client;

   protected:
    void on_session() override {}
  };

  rmi_client c{transport.connect()};
  c.connect();

  auto proxy = c.instantiate("wish"_key, "Window"_key).get();
  dynamic fields;
  fields["title"_key] = std::string{"ClientTest"};
  EXPECT_TRUE(proxy.set(std::move(fields)).get());
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "ClientTest");

  c.disconnect();
  srv.stop();
}

// ── register_template + instantiate_template ─────────────────────────────────

TEST(ClientTest, InstantiateReturnsNonEmptyMap) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowJson)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_FALSE(c.result.empty());
  srv.stop();
}

TEST(ClientTest, InstantiateRootProxyIsValid) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowJson)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  ASSERT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.at("").valid());
  srv.stop();
}

TEST(ClientTest, InstantiateNamedChildrenAppearInMap) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowWithChildJson)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.count("lbl"));
  if (c.result.count("lbl")) {
    EXPECT_TRUE(c.result.at("lbl").valid());
  }
  srv.stop();
}

TEST(ClientTest, InstantiateInvalidDescriptorThrows) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    bool threw{false};

   protected:
    void on_session() override {
      try {
        register_template("tpl"_key, wish::import_descriptor_json("{invalid json}")).get();
        instantiate_template("tpl"_key).get();
      } catch (const std::exception&) {
        threw = true;
      }
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.threw);
  srv.stop();
}

TEST(ClientTest, TemplateCanBeInstantiatedMultipleTimes) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map first;
    wish::proxy_map second;

   protected:
    void on_session() override {
      register_template("tpl"_key, wish::import_descriptor_json(kWindowWithChildJson)).get();
      first = instantiate_template("tpl"_key).get();
      second = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.first.count("") && c.first.count("lbl"));
  EXPECT_TRUE(c.second.count("") && c.second.count("lbl"));
  srv.stop();
}

// A hand-built bison::dynamic descriptor, with no JSON/YAML text at all --
// proves register_template's argument is a real dynamic tree, not just a
// convenience wrapper around text parsing.
TEST(ClientTest, RegisterTemplateAcceptsHandBuiltDescriptor) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    wish::proxy_map result;

   protected:
    void on_session() override {
      dynamic desc;
      desc["__type__"_key] = "Window"_key;
      desc["title"_key] = std::string{"Hand-built"};

      register_template("tpl"_key, std::move(desc)).get();
      result = instantiate_template("tpl"_key).get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  ASSERT_TRUE(c.result.count(""));
  EXPECT_TRUE(c.result.at("").valid());
  srv.stop();
}

TEST(ClientTest, InstantiateUnregisteredTemplateThrows) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    bool threw{false};

   protected:
    void on_session() override {
      try {
        instantiate_template("no_such_template"_key).get();
      } catch (const std::exception&) {
        threw = true;
      }
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_TRUE(c.threw);
  srv.stop();
}

// ── upload_file / download_file ───────────────────────────────────────────────

TEST(ClientTest, UploadDownloadRoundTrips) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string downloaded;

   protected:
    void on_session() override {
      upload_file("hello.txt", "world").get();
      downloaded = download_file("hello.txt").get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_EQ(c.downloaded, "world");
  srv.stop();
}

TEST(ClientTest, ChunkedUploadDownloadReportsProgress) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string data{std::string(1u << 21, 'x')}; // 2 MiB, multiple chunks.
    std::string downloaded;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> upload_progress;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> download_progress;

   protected:
    void on_session() override {
      upload_file(
          "big.bin", data,
          [this](std::uint64_t transferred, std::uint64_t total) {
            upload_progress.emplace_back(transferred, total);
          })
          .get();
      downloaded = download_file(
                       "big.bin",
                       [this](std::uint64_t transferred, std::uint64_t total) {
                         download_progress.emplace_back(transferred, total);
                       })
                       .get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_EQ(c.downloaded, c.data);

  ASSERT_FALSE(c.upload_progress.empty());
  EXPECT_GT(c.upload_progress.size(), 1u);
  for (std::size_t i = 1; i < c.upload_progress.size(); ++i)
    EXPECT_GT(c.upload_progress[i].first, c.upload_progress[i - 1].first);
  EXPECT_EQ(c.upload_progress.back().first, c.data.size());
  EXPECT_EQ(c.upload_progress.back().second, c.data.size());

  ASSERT_FALSE(c.download_progress.empty());
  EXPECT_GT(c.download_progress.size(), 1u);
  for (std::size_t i = 1; i < c.download_progress.size(); ++i)
    EXPECT_GT(c.download_progress[i].first, c.download_progress[i - 1].first);
  EXPECT_EQ(c.download_progress.back().first, c.data.size());
  EXPECT_EQ(c.download_progress.back().second, c.data.size());

  srv.stop();
}

// ── list_files ─────────────────────────────────────────────────────────────────

TEST(ClientTest, ListFilesReturnsSubdirectoryContents) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::vector<std::string> names;

   protected:
    void on_session() override {
      upload_file("icons/a.png", "a").get();
      upload_file("icons/b.png", "b").get();
      upload_file("root.txt", "r").get();
      names = list_files("icons").get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  std::unordered_set<std::string> found(c.names.begin(), c.names.end());
  EXPECT_EQ(found.size(), 2U);
  EXPECT_NE(found.find("a.png"), found.end());
  EXPECT_NE(found.find("b.png"), found.end());
  EXPECT_EQ(found.find("root.txt"), found.end());
  srv.stop();
}

TEST(ClientTest, ListFilesEmptyPathListsResourceDirRoot) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::vector<std::string> names;

   protected:
    void on_session() override {
      upload_file("root.txt", "r").get();
      names = list_files().get();
    }
  };

  test_client c{transport.connect()};
  c.run();

  std::unordered_set<std::string> found(c.names.begin(), c.names.end());
  EXPECT_NE(found.find("root.txt"), found.end());
  srv.stop();
}

// ── streamed upload_file / download_file ────────────────────────────────────────

TEST(ClientTest, UploadDownloadFileViaStreamsRoundTrips) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  // Content longer than the small chunk_size override below, so the test
  // actually exercises multiple upload_chunk/download_chunk round trips
  // instead of a single call.
  const std::string kContent = "The quick brown fox jumps over the lazy dog.";

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string downloaded;
    std::string content;

   protected:
    void on_session() override {
      std::istringstream in(content);
      upload_file("streamed.txt", in, /*chunk_size=*/8).get();

      std::ostringstream out;
      download_file("streamed.txt", out, /*chunk_size=*/8).get();
      downloaded = out.str();
    }
  };

  test_client c{transport.connect()};
  c.content = kContent;
  c.run();

  EXPECT_EQ(c.downloaded, kContent);
  srv.stop();
}

TEST(ClientTest, UploadDownloadFileViaStreamsHandlesEmptyContent) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string downloaded{"not empty"};

   protected:
    void on_session() override {
      std::istringstream in("");
      upload_file("empty_stream.txt", in).get();

      std::ostringstream out;
      download_file("empty_stream.txt", out).get();
      downloaded = out.str();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_EQ(c.downloaded, "");
  srv.stop();
}

// ── upload_package ───────────────────────────────────────────────────────────

TEST(ClientTest, UploadPackageExtractsFilesIntoDestFolder) {
  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  auto zip_path = std::filesystem::temp_directory_path() / "wish_test_client_upload_package.zip";
  make_test_zip(zip_path, {{"a.txt", "alpha"}, {"sub/b.txt", "beta"}});

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::filesystem::path zip_path;
    std::vector<std::string> dest_names;
    std::string a_content;

   protected:
    void on_session() override {
      std::ifstream in(zip_path, std::ios::binary);
      upload_package("my_folder/my_package", in, /*chunk_size=*/16).get();

      dest_names = list_files("my_folder/my_package").get();
      a_content = download_file("my_folder/my_package/a.txt").get();
    }
  };

  test_client c{transport.connect()};
  c.zip_path = zip_path;
  c.run();

  std::unordered_set<std::string> found(c.dest_names.begin(), c.dest_names.end());
  EXPECT_NE(found.find("a.txt"), found.end());
  EXPECT_EQ(c.a_content, "alpha");
  srv.stop();

  std::filesystem::remove(zip_path);
}

// ── Disconnect propagation ────────────────────────────────────────────────────
//
// When the server goes away (process exit, srv.stop(), etc.), on_disconnect()
// must fire so a caller blocked on its own completion signal (e.g.
// wish_client_app::on_session()'s done_future_.wait(), mirrored by
// waiting_client::on_session() below) wakes up instead of hanging forever --
// see wish::client::set_on_disconnected()'s doc comment. memory_server_transport
// can't exercise this: memory_client_transport never overrides is_connected()
// (the base client_transport_iface default always returns true), so
// bison::rmi::client::worker_loop()'s abrupt-disconnect detection never fires
// over it -- a real socket transport is required to reproduce the EOF the
// worker loop actually reacts to.

TEST(ClientTest, OnDisconnectedFiresWhenServerClosesConnection) {
  constexpr uint16_t kPort = 17075;
  socket_server_transport transport{"127.0.0.1", kPort};
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  class waiting_client : public wish::client {
   public:
    using wish::client::client;
    std::mutex mtx;
    std::condition_variable cv;
    bool disconnected{false};

   protected:
    void on_session() override {
      set_on_disconnected([this] {
        {
          std::lock_guard<std::mutex> lk(mtx);
          disconnected = true;
        }
        cv.notify_all();
      });
      // Bounded even if on_disconnect() never fires, so a regression here
      // fails this test instead of hanging the whole suite.
      std::unique_lock<std::mutex> lk(mtx);
      cv.wait_for(lk, std::chrono::seconds(5), [this] { return disconnected; });
    }
  };

  waiting_client c{std::make_unique<socket_client_transport>("127.0.0.1", kPort)};
  std::thread session_thread([&c] { c.run(); });

  // Let the client finish connecting before severing it.
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  srv.stop(); // closes the underlying socket -- same as the server process exiting

  session_thread.join(); // bounded by on_session()'s own 5s wait above

  EXPECT_TRUE(c.disconnected) << "on_session() unblocked via the 5s timeout instead of on_disconnect() firing";
}
