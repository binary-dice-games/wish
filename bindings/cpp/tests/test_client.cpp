// MIT License © 2025 Binary Dice Games
//
// Tests for the header-only C++ wish client binding. Unlike the native
// client's own tests (tests/test_client.cpp, in-process memory transport),
// these exercise the binding the way a real out-of-process consumer would:
// only wish_client_dll's C ABI on the client side. A real wish::server
// (linked from the full `wish` library, socket transport) is spun up
// in-process purely as the test fixture's counterpart -- the binding under
// test never sees anything but wish_client_c.h/bison_c.h/rmi_c.h.

#include <wish_cpp/wish.hpp>

#include <bison_c.h>
#include <gtest/gtest.h>
#include <auth/local_auth_module.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>

#include "../../../tests/free_port_server.hpp"
#include "src/rmi/shared/schemas.hpp"
#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "tests/tls_test_certs.hpp"

#include <miniz.h>
#include <miniz_zip.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>

namespace wish = bdg::wish::binding;
using namespace bdg::wish::binding;  // for the "_key" literal operator

// ── key_t: constexpr literal must be bit-identical to the runtime C ABI ────

TEST(WishCppKeyTest, LiteralMatchesRuntimeWishKey) {
  for (const char* name : {"clicked", "text", "row0.c", "", "a_much_longer_field_name_here"}) {
    wish::hash_t runtime = wish_key(name);
    wish::hash_t compile_time = wish::key_t{name}.id;
    EXPECT_EQ(runtime, compile_time) << "mismatch for \"" << name << "\"";
  }
}

TEST(WishCppKeyTest, LiteralIsConstexpr) {
  constexpr wish::key_t k = "clicked"_key;
  static_assert(k.id != 0, "must hash to something non-zero");
  EXPECT_EQ(k.id, wish_key("clicked"));
}

TEST(WishCppKeyTest, MsbIsAlwaysSet) {
  EXPECT_NE("x"_key.id & 0x80000000u, 0u);
}

// ── value: header-only bison_handle wrapper ────────────────────────────────

TEST(WishCppValueTest, ScalarFieldsRoundTrip) {
  wish::value v;
  v.set_int("i"_key, 42).set_float("f"_key, 1.5f).set_bool("b"_key, true).set_string("s"_key, "hi");

  EXPECT_EQ(v.get_int("i"_key), 42);
  EXPECT_FLOAT_EQ(*v.get_float("f"_key), 1.5f);
  EXPECT_EQ(v.get_bool("b"_key), true);
  EXPECT_EQ(v.get_string("s"_key), "hi");

  // bison_get_int() auto-vivifies an absent field as its type's default (0)
  // rather than reporting BISON_ERR_NOT_FOUND -- same behavior as
  // bison::dynamic::operator[]() at the C++ level.
  EXPECT_EQ(v.get_int("missing"_key), 0);
}

TEST(WishCppValueTest, FieldRefAssignmentSugar) {
  wish::value v;
  v["text"_key] = std::string{"hello"};
  v["count"_key] = 7;
  EXPECT_EQ(v.get_string("text"_key), "hello");
  EXPECT_EQ(v.get_int("count"_key), 7);
}

TEST(WishCppValueTest, NestedObjectRoundTrips) {
  wish::value inner;
  inner["x"_key] = 1;
  wish::value outer;
  outer["child"_key] = inner;

  auto child = outer.get_object("child"_key);
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->get_int("x"_key), 1);
}

TEST(WishCppValueTest, JsonRoundTrips) {
  auto v = wish::value::parse_json(R"({"a":1,"b":"two"})");
  EXPECT_EQ(v.get_int("a"_key), 1);
  EXPECT_EQ(v.get_string("b"_key), "two");
}

TEST(WishCppValueTest, ArrayIndexRoundTrips) {
  auto v = wish::value::parse_json(R"({"items":["a","b","c"]})");
  auto items = v.get_object("items"_key);
  ASSERT_TRUE(items.has_value());
  ASSERT_EQ(items->size(), 3u);
  EXPECT_EQ(items->get_string_at(0), "a");
  EXPECT_EQ(items->get_string_at(2), "c");
}

// ── client / proxy: end-to-end against a real wish::server ────────────────

namespace {

constexpr const char* kWindowDesc = R"({
  "type": "Window",
  "title": "T",
  "children": { "label": { "type": "Label", "text": "hi" } }
})";

class WishCppClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A free port, not a fixed one: ctest runs these tests as parallel
    // processes (see tests/free_port_server.hpp).
    auto s = start_on_free_port<bdg::bison::rmi::transport::socket_server_transport>(0);
    transport_ = std::move(s.transport);
    server_ = std::move(s.server);
    port_ = s.port;
  }

  void TearDown() override { server_->stop(); }

  std::unique_ptr<bdg::bison::rmi::transport::socket_server_transport> transport_;
  std::unique_ptr<bdg::wish::server> server_;
  uint16_t port_{0};
};

}  // namespace

TEST_F(WishCppClientTest, RegisterInstantiateAndSetGetRoundTrip) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([](wish::client& c) {
    c.register_template("win", kWindowDesc);
    auto root = c.instantiate_template("win", "win");
    EXPECT_TRUE(root.valid());

    auto label = c.proxy_get("win.label");
    EXPECT_EQ(*label.get().get_string("text"_key), "hi");

    wish::value f;
    f["text"_key] = std::string{"updated"};
    label.set(f);
    EXPECT_EQ(*label.get().get_string("text"_key), "updated");
  });
}

TEST_F(WishCppClientTest, RegisterTemplateAppliesTranslations) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([](wish::client& c) {
    c.set_translations("GREETING = Hola", std::string{"GREETING = Hello\nTITLE = Main"});
    c.set_language("es");
    c.register_template(
        "i18n", R"({"type":"Window","title":"$$TITLE","children":{"label":{"type":"Label","text":"$$GREETING"}}})");
    auto root = c.instantiate_template("i18n", "i18n");
    EXPECT_EQ(*root.get().get_string("title"_key), "Main");
    EXPECT_EQ(*c.proxy_get("i18n.label").get().get_string("text"_key), "Hola");
    EXPECT_EQ(c.translate("$$GREETING"), "Hola");
    EXPECT_THROW(c.set_language("../x"), wish::error);
  });
}

TEST_F(WishCppClientTest, SyncResourcePackageUploadsThenCaches) {
  auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto zip_path = std::filesystem::temp_directory_path() /
      (std::string{"wish_cpp_"} + info->test_suite_name() + "_" + info->name() + ".zip");
  {
    mz_zip_archive zip{};
    ASSERT_TRUE(mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0));
    const std::string content = "alpha";
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip, "a.txt", content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
    ASSERT_TRUE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
  }

  // Written on the sync thread, read after wait_resource_syncs().
  std::vector<wish::client::resource_sync_status> first, second;
  std::string content;
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([&](wish::client& c) {
    c.sync_resource_package("test.cpp", zip_path.string(), "assets", [&](const auto& s) { first.push_back(s); });
    c.wait_resource_syncs();
    content = c.download_file("assets/a.txt");

    c.sync_resource_package("test.cpp", zip_path.string(), "assets", [&](const auto& s) { second.push_back(s); });
    c.wait_resource_syncs();

    EXPECT_THROW(c.sync_resource_package("test.cpp", zip_path.string() + ".missing"), wish::error);
  });

  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first.back().state, WISH_SYNC_UPLOADED);
  EXPECT_EQ(first.back().name, "test.cpp");
  for (std::size_t i = 0; i + 1 < first.size(); ++i)
    EXPECT_EQ(first[i].state, WISH_SYNC_PROGRESS);
  EXPECT_EQ(content, "alpha");
  ASSERT_FALSE(second.empty());
  EXPECT_EQ(second.back().state, WISH_SYNC_CACHED);
  EXPECT_EQ(second.back().transferred, second.back().total);

  std::filesystem::remove(zip_path);
}

TEST_F(WishCppClientTest, ProxyGetForUnknownPathThrows) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([](wish::client& c) { EXPECT_THROW(c.proxy_get("no.such.path"), wish::error); });
}

// ── Desktop: run_on_desktop() against a stand-in desktop ─────────────────
//
// A plain wish::server listening on a per-test pipe plays the desktop;
// WISH_DESKTOP_ENDPOINT points client::desktop() at it. Each test runs in
// its own process (ctest), so setting the variable is local to the test.

namespace {

std::string unique_desktop_endpoint() {
  const std::string id = std::to_string(bdg::bison::rmi::shared::generate_id().id);
#if defined(_WIN32) || defined(__CYGWIN__)
  return "\\\\.\\pipe\\wish_cpp_desktop_test_" + id;
#else
  return "@wish_cpp_desktop_test_" + id;
#endif
}

void set_desktop_endpoint_env(const std::string& value) {
#if defined(_WIN32)
  _putenv_s("WISH_DESKTOP_ENDPOINT", value.c_str());
#else
  setenv("WISH_DESKTOP_ENDPOINT", value.c_str(), 1);
#endif
}

// Stops a client's run_on_desktop() loop when a test returns early (a
// failed ASSERT), so the std::async future's destructor cannot hang.
struct stop_desktop_on_exit {
  wish::client& client;
  ~stop_desktop_on_exit() {
    client.stop_desktop();
  }
};

// Counts sessions; tests wait on it instead of sleeping.
class session_counter {
 public:
  void bump() {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      ++count_;
    }
    cv_.notify_all();
  }
  bool wait_for_count(int n, std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    std::unique_lock<std::mutex> lk(mtx_);
    return cv_.wait_for(lk, timeout, [&] { return count_ >= n; });
  }

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  int count_ = 0;
};

class WishCppDesktopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    set_desktop_endpoint_env(endpoint_);
  }

  void TearDown() override {
    stop_desktop();
  }

  // (Re)starts the stand-in desktop.
  void start_desktop() {
    transport_ = std::make_unique<bdg::bison::rmi::transport::named_pipe_server_transport>(endpoint_);
    server_ = std::make_unique<bdg::wish::server>(*transport_, std::make_unique<bdg::wish::null_renderer>());
    server_->start();
  }

  void stop_desktop() {
    if (server_)
      server_->stop();
    server_.reset();
    transport_.reset();
  }

  // Runs client.run_on_desktop() on another thread; the session builds a
  // window, counts itself, and waits.
  std::future<void> run_in_background(wish::client& client) {
    return std::async(std::launch::async, [this, &client] {
      client.run_on_desktop(
          [this](wish::client& c) {
            c.register_template("win", kWindowDesc);
            EXPECT_TRUE(c.instantiate_template("win", "win").valid());
            sessions_.bump();
            c.wait();
          },
          wish::value{},
          std::chrono::milliseconds{20});
    });
  }

  std::string endpoint_ = unique_desktop_endpoint();
  std::unique_ptr<bdg::bison::rmi::transport::named_pipe_server_transport> transport_;
  std::unique_ptr<bdg::wish::server> server_;
  session_counter sessions_;
};

} // namespace

TEST_F(WishCppDesktopTest, SessionRunsAgainEachTimeTheDesktopComesBack) {
  auto client = wish::client::desktop();
  auto done = run_in_background(client); // no desktop yet: retrying
  stop_desktop_on_exit guard{client};

  start_desktop();
  ASSERT_TRUE(sessions_.wait_for_count(1));

  stop_desktop(); // the desktop closes; the client goes back to waiting
  start_desktop();
  ASSERT_TRUE(sessions_.wait_for_count(2));

  client.stop_desktop();
  ASSERT_EQ(done.wait_for(std::chrono::seconds{5}), std::future_status::ready);
  done.get();
}

TEST_F(WishCppDesktopTest, StopWhileWaitingForADesktopReturns) {
  auto client = wish::client::desktop();
  auto done = run_in_background(client);
  stop_desktop_on_exit guard{client};
  client.stop_desktop();
  ASSERT_EQ(done.wait_for(std::chrono::seconds{5}), std::future_status::ready);
  done.get();
}

TEST_F(WishCppDesktopTest, QuitWhileConnectedEndsTheLoop) {
  start_desktop();
  auto client = wish::client::desktop();
  auto done = std::async(std::launch::async, [&client] {
    client.run_on_desktop([](wish::client& c) {
      c.quit();
      c.wait();
    });
  });
  ASSERT_EQ(done.wait_for(std::chrono::seconds{5}), std::future_status::ready);
  done.get();
}

TEST_F(WishCppDesktopTest, StopBeforeRunReturnsAtOnce) {
  auto client = wish::client::desktop();
  client.stop_desktop();
  bool ran = false;
  client.run_on_desktop([&ran](wish::client&) { ran = true; });
  EXPECT_FALSE(ran);
}

// ── User store ───────────────────────────────────────────────────────────

namespace {

class WishCppUserStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string{"wish_cpp_store_"} + info->name());
    std::filesystem::remove_all(dir_);
    auto s = start_on_free_port_configured<bdg::bison::rmi::transport::socket_server_transport>(
        0, [this](bdg::wish::server& srv) { srv.set_store_dir(dir_); },
        std::make_shared<bdg::wish::local_auth_module>());
    transport_ = std::move(s.transport);
    server_ = std::move(s.server);
    port_ = s.port;
  }

  void TearDown() override {
    server_->stop();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  static wish::value as_user(const std::string& username) {
    wish::value p;
    if (!username.empty()) p["username"_key] = username;
    return p;
  }

  std::filesystem::path dir_;
  std::unique_ptr<bdg::bison::rmi::transport::socket_server_transport> transport_;
  std::unique_ptr<bdg::wish::server> server_;
  uint16_t port_{0};
};

}  // namespace

TEST_F(WishCppUserStoreTest, IdentifiedClientRoundTripsAndPersists) {
  {
    auto client = wish::client::tcp("127.0.0.1", port_);
    client.run(
        [](wish::client& c) {
          ASSERT_TRUE(c.has_user_store());
          wish::value v;
          v["filter"_key] = std::string{"error"};
          c.user_store_set("bdg.test", v);
          EXPECT_FALSE(c.user_store_get("missing").has_value());
          EXPECT_EQ(c.user_store_keys(), (std::vector<std::string>{"bdg.test"}));
        },
        as_user("alice"));
  }
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run(
      [](wish::client& c) {
        auto v = c.user_store_get("bdg.test");
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v->get_string("filter"_key), "error");
        EXPECT_TRUE(c.user_store_erase("bdg.test"));
        EXPECT_FALSE(c.user_store_erase("bdg.test"));
      },
      as_user("alice"));
}

TEST_F(WishCppUserStoreTest, DescribesSettingsInTheUserStore) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run(
      [](wish::client& c) {
        wish::setting_info s;
        s.name = "bdg.desktop.example.mode";
        s.type = "enum";
        s.label = "Mode \"quoted\"";
        s.options = {{"a", "Alpha"}, {"b", ""}};
        s.default_value = std::string{"a"};
        c.describe_setting(s);
        wish::setting_info n;
        n.name = "bdg.desktop.example.size";
        n.type = "float";
        n.min = 0.5;
        n.max = 2.0;
        n.default_value = 1.25;
        c.describe_setting(n);
        c.describe_settings_category({"example", "Example", "", "res/icons/palette.png", "#AF52DE", 1});
        EXPECT_EQ(
            c.user_store_keys(),
            (std::vector<std::string>{"bdg.desktop.example#schema", "wish.category.example#schema"}));
        wish::setting_info bad;
        bad.name = "nodot";
        EXPECT_THROW(c.describe_setting(bad), wish::error);
      },
      as_user("alice"));
}

TEST_F(WishCppUserStoreTest, ReadsSettingsOrTheirDefaults) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run(
      [](wish::client& c) {
        wish::setting_info size;
        size.name = "bdg.desktop.example.size";
        size.type = "float";
        size.min = 0.5;
        size.max = 2.0;
        size.default_value = 1; // an int default reads as the float 1.0
        EXPECT_EQ(std::get<double>(c.read_setting(size)), 1.0);
        wish::setting_info on;
        on.name = "bdg.desktop.example.on";
        on.type = "bool";
        on.default_value = true;
        wish::setting_info mode;
        mode.name = "bdg.desktop.example.mode";
        mode.type = "enum";
        mode.options = {{"a", ""}, {"b", ""}};
        mode.default_value = std::string{"a"};

        wish::value v;
        v.set_float("size"_key, 1.5f).set_bool("on"_key, false).set_string("mode"_key, "zzz");
        c.user_store_set("bdg.desktop.example", v);
        EXPECT_EQ(std::get<double>(c.read_setting(size)), 1.5);
        EXPECT_FALSE(std::get<bool>(c.read_setting(on)));
        EXPECT_EQ(std::get<std::string>(c.read_setting(mode)), "a"); // not an option
        wish::setting_info bad;
        bad.name = "nodot";
        EXPECT_THROW(c.read_setting(bad), wish::error);
      },
      as_user("alice"));
  // Anonymous: the default, not an error.
  auto anon = wish::client::tcp("127.0.0.1", port_);
  anon.run([](wish::client& c) {
    wish::setting_info on;
    on.name = "bdg.desktop.example.on";
    on.type = "bool";
    on.default_value = true;
    EXPECT_TRUE(std::get<bool>(c.read_setting(on)));
  });
}

TEST_F(WishCppUserStoreTest, ClientPostsAndDismissesNotifications) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([](wish::client& c) {
    const uint64_t id =
        c.notify("Backup finished", "42 files", wish::client::notify_kind::success, 0, "Ctrl+B", "backup");
    EXPECT_GT(id, 0u);
    EXPECT_EQ(c.notify("Backup finished", "43 files", wish::client::notify_kind::success, 0, "", "backup"), id)
        << "same tag: replaced";
    c.dismiss_notification(id);
    EXPECT_THROW(c.notify(""), wish::error);
  });
}

TEST_F(WishCppUserStoreTest, AnonymousClientGetsUnavailable) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([](wish::client& c) {
    EXPECT_FALSE(c.has_user_store());
    try {
      c.user_store_keys();
      FAIL() << "expected wish::error";
    } catch (const wish::error& e) {
      EXPECT_EQ(e.code(), WISH_ERR_UNAVAILABLE);
    }
  });
}

// Firing a real "clicked" event needs a live renderer frame loop (see
// src/context/context.hpp's emit_event() / pending_events plumbing) --
// beyond what a headless null_renderer test fixture can drive. This test
// covers the C ABI subscription round-trip only (rmi_proxy_on_event()
// succeeding, callback pointer surviving); the calculator/nano examples
// under bindings/cpp/examples/ exercise real event delivery end-to-end
// against a rendering server.
TEST_F(WishCppClientTest, OnEventSubscriptionSucceeds) {
  auto client = wish::client::tcp("127.0.0.1", port_);
  client.run([](wish::client& c) {
    c.register_template(
        "btnwin", R"({"type":"Window","title":"T","children":{"btn":{"type":"Button","label":"go"}}})");
    auto root = c.instantiate_template("btnwin", "btnwin");
    auto btn = c.proxy_get("btnwin.btn");

    EXPECT_NO_THROW(btn.on_event("clicked"_key, [](wish::value) {}));
  });
}

// ── client / proxy: end-to-end against a real wish::server over TLS ────────
//
// Exercises wish_client_tls_create() (the C ABI factory added alongside
// bison's tls_socket_client_transport) through the header-only binding's
// client::tls(), against a real tls_socket_server_transport-backed
// wish::server -- the same shape as WishCppClientTest above, but over an
// encrypted, server-authenticated connection.

namespace {

class WishCppTlsClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto s = start_on_free_port<bdg::bison::rmi::transport::tls_socket_server_transport>(
        0,
        bdg::bison::rmi::auth_module_ptr{},
        bdg::bison::rmi::transport::test::tls_server_params(
            bdg::bison::rmi::transport::test::kTestServerCert, bdg::bison::rmi::transport::test::kTestServerKey));
    transport_ = std::move(s.transport);
    server_ = std::move(s.server);
    port_ = s.port;
  }

  void TearDown() override { server_->stop(); }

  std::unique_ptr<bdg::bison::rmi::transport::tls_socket_server_transport> transport_;
  std::unique_ptr<bdg::wish::server> server_;
  uint16_t port_{0};
};

}  // namespace

TEST_F(WishCppTlsClientTest, RegisterInstantiateAndSetGetRoundTrip) {
  auto client = wish::client::tls("127.0.0.1", port_);
  wish::value connect_params;
  connect_params["ca_pem"_key] = bdg::bison::rmi::transport::test::kTestCaCert;
  client.run(
      [](wish::client& c) {
        c.register_template("win", kWindowDesc);
        auto root = c.instantiate_template("win", "win");
        EXPECT_TRUE(root.valid());

        auto label = c.proxy_get("win.label");
        EXPECT_EQ(*label.get().get_string("text"_key), "hi");
      },
      connect_params);
}

TEST_F(WishCppTlsClientTest, MissingTrustAnchorFailsHandshake) {
  // No ca_pem supplied and insecure_skip_verify left false -- the client has
  // no trust anchor for the server's certificate, so the handshake inside
  // run()'s connect step must fail rather than silently succeeding.
  auto client = wish::client::tls("127.0.0.1", port_);
  EXPECT_THROW(client.run([](wish::client&) {}), wish::error);
}

// ── Media playback ───────────────────────────────────────────────────────

#ifdef WISH_MEDIA_ENABLED

namespace {

// 16-bit stereo 48 kHz WAV of silence, @p seconds long.
std::string silent_wav(double seconds) {
  const uint32_t frames = static_cast<uint32_t>(seconds * 48000);
  const uint32_t data = frames * 4;
  std::string out;
  auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); };
  out += "RIFF";
  u32(36 + data);
  out += "WAVEfmt ";
  u32(16);
  u16(1);
  u16(2);
  u32(48000);
  u32(48000 * 4);
  u16(4);
  u16(16);
  out += "data";
  u32(data);
  out.append(data, '\0');
  return out;
}

}  // namespace

TEST(WishCppMediaTest, OpensPlaysAndHearsTheEnd) {
  auto s = start_on_free_port<bdg::bison::rmi::transport::socket_server_transport>(0);
  auto client = wish::client::tcp("127.0.0.1", s.port);
  bool has = false, ended = false, refused = false, closed_throws = false;
  wish::media_status st;
  client.run([&](wish::client& c) {
    has = c.has_media();
    c.upload_file("quiet.wav", silent_wav(0.3));
    wish::media_options options;
    options.volume = 0.0f;
    auto player = c.open_media("quiet", "quiet.wav", options);
    EXPECT_EQ(player.name(), "quiet");
    std::promise<void> end;
    std::once_flag once;
    player.on_event([&](const wish::media_player::event& e) {
      if (e.what == wish::media_player::event::kind::ended)
        std::call_once(once, [&] { end.set_value(); });
    });
    st = player.status();
    player.play();
    ended = end.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    EXPECT_EQ(player.status().state, wish::media_state::ended);
    EXPECT_EQ(player.error_message(), "");
    try {
      c.open_media("x", "../escape.wav");
    } catch (const wish::error&) {
      refused = true;
    }
    player.close();
    try {
      player.play();
    } catch (const wish::error&) {
      closed_throws = true;
    }
  });
  s.server->stop();
  EXPECT_TRUE(has);
  EXPECT_TRUE(st.has_audio);
  EXPECT_FALSE(st.has_video);
  EXPECT_NEAR(st.duration, 0.3, 0.05);
  EXPECT_EQ(st.state, wish::media_state::stopped);
  EXPECT_TRUE(ended);
  EXPECT_TRUE(refused);
  EXPECT_TRUE(closed_throws);
}

#endif  // WISH_MEDIA_ENABLED
