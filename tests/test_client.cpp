// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <client/client.hpp>
#include <server/server.hpp>
#include <ui/ui_descriptor.hpp>

#include "src/rmi/rmi.hpp"

#include <miniz.h>
#include <miniz_zip.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
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

  class test_client : public wish::client {
   public:
    using wish::client::client;
    std::string downloaded;

    // Content longer than the small chunk_size override below, so the test
    // actually exercises multiple upload_chunk/download_chunk round trips
    // instead of a single call.
    static inline const std::string kContent = "The quick brown fox jumps over the lazy dog.";

   protected:
    void on_session() override {
      std::istringstream in(kContent);
      upload_file("streamed.txt", in, /*chunk_size=*/8).get();

      std::ostringstream out;
      download_file("streamed.txt", out, /*chunk_size=*/8).get();
      downloaded = out.str();
    }
  };

  test_client c{transport.connect()};
  c.run();

  EXPECT_EQ(c.downloaded, test_client::kContent);
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
