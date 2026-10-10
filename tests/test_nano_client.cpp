// MIT License © 2026 Binary Dice Games
//
// End-to-end test of nano's client runner (run_nano) against a real server
// over an in-memory transport: where it puts the sandbox copies of the
// local files it opens.
#include <gtest/gtest.h>

#include "modules/bdg/desktop/nano/client/nano.hpp"

#include <client/app_registry.hpp>
#include <client/client.hpp>
#include <client/wish_app_host.hpp>
#include <context/context.hpp>
#include <server/server.hpp>

#include "src/rmi/rmi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;
namespace fs = std::filesystem;

namespace {

/// wish_app_host over a wish::client, like the C ABI's c_abi_app_host.
class client_host : public wish::wish_app_host {
 public:
  client_host(wish::client& client, std::vector<std::string> args) : client_(client), args_(std::move(args)) {}

  std::future<rmi::proxy::dynamic> instantiate(bdg::bison::key_t ns, bdg::bison::key_t klass, dynamic params) override {
    return client_.instantiate(ns, klass, std::move(params));
  }
  std::future<void>
  upload_file(const std::string& name, const std::string& data, wish::transfer_progress_callback cb) override {
    return client_.upload_file(name, data, std::move(cb));
  }
  std::future<std::string> download_file(const std::string& name, wish::transfer_progress_callback cb) override {
    return client_.download_file(name, std::move(cb));
  }
  std::future<std::string> create_temp_dir(const std::string& qualified_app) override {
    return client_.create_temp_dir(qualified_app);
  }
  void keep_alive(rmi::proxy::dynamic&&) override {}
  void signal_done() override {}
  const std::vector<std::string>& app_args() const override {
    return args_;
  }
  bool read_console_line(std::string&) override {
    return false;
  }

 private:
  wish::client& client_;
  std::vector<std::string> args_;
};

class capturing_server : public wish::server {
 public:
  using wish::server::server;
  std::atomic<wish::context*> last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

/// Runs nano with the given startup files, then calls @p inspect while
/// still connected.
class nano_client : public wish::client {
 public:
  using wish::client::client;
  std::vector<std::string> args;
  std::function<void()> inspect;

 protected:
  void on_session() override {
    client_host host{*this, args};
    auto resolution = wish::resolve_app("bdg/desktop/nano");
    ASSERT_EQ(resolution.status, wish::app_resolve_status::found);
    host.set_running_app(resolution.info);
    wish::run_nano(host);
    inspect();
  }
};

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, const std::string& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << data;
}

} // namespace

TEST(NanoClient, OpensLocalFilesInASessionTempDirNotTheSandboxRoot) {
  const auto local = fs::temp_directory_path() /
      ("wish_nano_client_" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()} + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  write_file(local / "a" / "notes.txt", "from a");
  write_file(local / "b" / "notes.txt", "from b");

  memory_server_transport transport;
  capturing_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  // Read while connected: the session's sandbox goes away with it.
  fs::path sandbox;
  std::vector<std::string> copies; // sandbox-relative
  std::vector<std::string> contents;
  bool copy_at_root = true;
  {
    nano_client c{transport.connect()};
    c.args = {(local / "a" / "notes.txt").string(), (local / "b" / "notes.txt").string()};
    c.inspect = [&] {
      sandbox = srv.last_session.load()->resource_dir;
      for (auto& entry : fs::recursive_directory_iterator(sandbox))
        if (entry.is_regular_file() && entry.path().filename() == "notes.txt") {
          copies.push_back(entry.path().lexically_relative(sandbox).generic_string());
          contents.push_back(read_file(entry.path()));
        }
      copy_at_root = fs::exists(sandbox / "notes.txt");
    };
    c.run(dynamic{});
  }

  // Both copies keep their name, in separate subdirectories of nano's
  // private temp dir, and none lands at the shared root.
  ASSERT_EQ(copies.size(), 2u);
  for (const auto& rel : copies)
    EXPECT_EQ(rel.rfind("private/apps/bdg.desktop.nano/tmp/", 0), 0u) << rel;
  std::sort(contents.begin(), contents.end());
  EXPECT_EQ(contents, (std::vector<std::string>{"from a", "from b"}));
  EXPECT_FALSE(copy_at_root);

  srv.stop();
  std::error_code ec;
  fs::remove_all(local, ec);
}
