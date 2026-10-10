// MIT License © 2026 Binary Dice Games
//
// curl_source persistence through the user store (see
// docs/persistent-store.md and modules/bdg/dev/curl/DESIGN.md
// "Persistence"): Collections, Environments and History survive a new
// session of the same identity, and an anonymous session keeps them in
// memory only and says so in the Console. Drives curl_source directly
// against a stub form that records the update_* calls it receives, over a
// real wish::server + wish::client (so the real __WishUserStore service is
// used end to end).
#include <gtest/gtest.h>

#include "modules/bdg/common/command_worker.hpp"
#include "modules/bdg/dev/curl/client/curl_source.hpp"

#include <auth/local_auth_module.hpp>
#include <client/client.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/forms/form.hpp>

#include "src/client/wish_app_host.hpp"

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/rmi.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;
namespace curl = bdg::wish::curl;

namespace {

// ── Stub CurlFrontend form ───────────────────────────────────────────────────

struct recorded_call {
  std::string method;
  dynamic args;
};

bdg::bison::synchronized<std::vector<recorded_call>>& recorded_calls() {
  static bdg::bison::synchronized<std::vector<recorded_call>> calls;
  return calls;
}

class recording_form : public wish::form {
 public:
  explicit recording_form(dynamic&& base) : form(std::move(base)) {}

 protected:
  void on_init() override {}
};

void ensure_registered() {
  static bool done = false;
  if (done)
    return;
  done = true;
  wish::register_all();
  auto proto = dynamic_ptr{"__CurlStoreStub"_key, {}};
  for (const char* name :
       {"append_command_log",
        "command_result",
        "update_collections",
        "update_environments",
        "update_history",
        "update_request_builder",
        "update_environment_vars",
        "update_response"}) {
    const std::string method = name;
    proto->addMethod(bdg::bison::key_t{name}, bdg::bison::method{[method](dynamic&, const dynamic& args) -> dynamic {
                       recorded_calls().wlock()->push_back({method, args});
                       return {};
                     }});
  }
  dynamic::addClass(
      "wish"_key,
      std::move(proto),
      bdg::bison::key_t{0U},
      dynamic::make_factory<recording_form>("wish"_key, "__CurlStoreStub"_key));
}

/// The last call the stub received for @p method.
std::optional<dynamic> last_call(const std::string& method) {
  auto calls = recorded_calls().rlock();
  for (auto it = calls->rbegin(); it != calls->rend(); ++it)
    if (it->method == method)
      return it->args;
  return std::nullopt;
}

/// The `entries` array of an update_* call, as item objects.
std::vector<dynamic> entries_of(const dynamic& args) {
  std::vector<dynamic> out;
  const auto* arr = args.findField<dynamic_ptr>("entries"_key);
  if (!arr || !*arr)
    return out;
  (*arr)->forEach([&](bdg::bison::key_t, const field& f) {
    if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
      out.push_back(*f.as<dynamic_ptr>());
  });
  return out;
}

// ── Host over a real wish::client ────────────────────────────────────────────

class client_host : public wish::wish_app_host {
 public:
  explicit client_host(wish::client& client) : client_(client) {}

  std::future<rmi::proxy::dynamic> instantiate(bdg::bison::key_t ns, bdg::bison::key_t klass, dynamic params) override {
    return client_.instantiate(ns, klass, std::move(params));
  }
  std::future<void> upload_file(const std::string& name, const std::string& data, wish::transfer_progress_callback)
      override {
    return client_.upload_file(name, data);
  }
  std::future<std::string> download_file(const std::string& name, wish::transfer_progress_callback) override {
    return client_.download_file(name);
  }
  bool has_user_store() const override {
    return client_.has_user_store();
  }
  std::future<std::optional<dynamic>> user_store_get(const std::string& name) override {
    return client_.user_store_get(name);
  }
  std::future<void> user_store_set(const std::string& name, dynamic value) override {
    return client_.user_store_set(name, std::move(value));
  }
  std::future<bool> user_store_erase(const std::string& name) override {
    return client_.user_store_erase(name);
  }
  std::future<std::vector<std::string>> user_store_keys() override {
    return client_.user_store_keys();
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

/// One connected client session driving a fresh curl_source.
struct curl_session {
  curl_session(memory_server_transport& transport, const std::string& username) : client(transport.connect()) {
    dynamic params;
    if (!username.empty())
      params["username"_key] = username;
    client.connect(std::move(params));
    proxy = std::make_shared<rmi::proxy::dynamic>(client.instantiate("wish"_key, "__CurlStoreStub"_key).get());
    host = std::make_unique<client_host>(client);
    // Not start()ed: the test thread acts as the worker thread.
    worker = std::make_shared<wish::common::command_worker>(*host, "Sending request");
    source = std::make_shared<curl::curl_source>(proxy, *host, worker);
  }

  ~curl_session() {
    source.reset();
    worker.reset();
    proxy.reset();
    client.disconnect();
  }

  wish::client client;
  std::shared_ptr<rmi::proxy::dynamic> proxy;
  std::unique_ptr<client_host> host;
  std::shared_ptr<wish::common::command_worker> worker;
  std::shared_ptr<curl::curl_source> source;
};

dynamic make_request_payload(const std::string& name, const std::string& url) {
  dynamic p;
  p["name"_key] = name;
  p["collection"_key] = std::string{"API"};
  p["method"_key] = std::string{"POST"};
  p["url"_key] = url;
  auto headers = std::make_shared<dynamic>();
  auto h = std::make_shared<dynamic>();
  (*h)["key"_key] = std::string{"Accept"};
  (*h)["value"_key] = std::string{"application/json"};
  (*h)["enabled"_key] = true;
  (*headers)[0] = dynamic_ptr{h};
  p["headers"_key] = dynamic_ptr{headers};
  p["params"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  p["form_fields"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  p["body_mode"_key] = std::string{"json"};
  p["body_text"_key] = std::string{"{\"a\":1}"};
  p["auth_mode"_key] = std::string{"bearer"};
  p["auth_username"_key] = std::string{};
  p["auth_password"_key] = std::string{};
  p["auth_token"_key] = std::string{"secret-token"};
  p["follow_redirects"_key] = false;
  return p;
}

class CurlStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensure_registered();
    recorded_calls().wlock()->clear();
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string{"wish_curl_store_"} + info->name());
    std::filesystem::remove_all(dir_);
    srv_ = std::make_unique<wish::server>(transport_, std::make_unique<wish::null_renderer>());
    srv_->set_store_dir(dir_);
    srv_->start(std::make_shared<wish::local_auth_module>());
  }

  void TearDown() override {
    srv_->stop();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  memory_server_transport transport_;
  std::unique_ptr<wish::server> srv_;
  std::filesystem::path dir_;
};

} // namespace

// ── Persistence ──────────────────────────────────────────────────────────────

TEST_F(CurlStoreTest, CollectionsAndEnvironmentsPersistAcrossSessions) {
  {
    curl_session s{transport_, "alice"};
    s.source->refresh_all();
    s.source->on_save_request_requested(make_request_payload("Create user", "https://example.test/users"));
    s.source->on_new_environment_requested("dev");

    auto envs = entries_of(*last_call("update_environments"));
    ASSERT_EQ(envs.size(), 1u);
    dynamic vars_payload;
    vars_payload["id"_key] = envs[0].as<std::string>("id"_key);
    auto vars = std::make_shared<dynamic>();
    auto v = std::make_shared<dynamic>();
    (*v)["key"_key] = std::string{"host"};
    (*v)["value"_key] = std::string{"example.test"};
    (*vars)[0] = dynamic_ptr{v};
    vars_payload["vars"_key] = dynamic_ptr{vars};
    s.source->on_save_environment_vars_requested(vars_payload);
  }
  EXPECT_TRUE(std::filesystem::exists(dir_ / "users" / "alice.bison"));

  recorded_calls().wlock()->clear();
  curl_session s{transport_, "alice"};
  s.source->refresh_all();

  auto collections = entries_of(*last_call("update_collections"));
  ASSERT_EQ(collections.size(), 1u);
  EXPECT_EQ(collections[0].as<std::string>("name"_key), "Create user");
  EXPECT_EQ(collections[0].as<std::string>("collection"_key), "API");
  EXPECT_EQ(collections[0].as<std::string>("method"_key), "POST");

  auto envs = entries_of(*last_call("update_environments"));
  ASSERT_EQ(envs.size(), 1u);
  EXPECT_EQ(envs[0].as<std::string>("name"_key), "dev");
  EXPECT_EQ(envs[0].as<int32_t>("var_count"_key), 1);

  // Loading the saved request restores the full builder state.
  s.source->on_load_request_requested(collections[0].as<std::string>("id"_key));
  auto builder = last_call("update_request_builder");
  ASSERT_TRUE(builder.has_value());
  EXPECT_EQ(builder->as<std::string>("url"_key), "https://example.test/users");
  EXPECT_EQ(builder->as<std::string>("body_text"_key), "{\"a\":1}");
  EXPECT_EQ(builder->as<std::string>("auth_mode"_key), "bearer");
  EXPECT_EQ(builder->as<std::string>("auth_token"_key), "secret-token");
  const auto& headers = builder->as<dynamic_ptr>("headers"_key);
  ASSERT_TRUE(headers);
  EXPECT_EQ(headers->size(), 1u);

  auto keys = s.client.user_store_keys().get();
  EXPECT_EQ(keys, (std::vector<std::string>{"bdg.dev.curl.collections", "bdg.dev.curl.environments"}));
}

TEST_F(CurlStoreTest, OtherUserDoesNotSeeCollections) {
  {
    curl_session s{transport_, "alice"};
    s.source->refresh_all();
    s.source->on_save_request_requested(make_request_payload("Mine", "https://example.test"));
  }
  recorded_calls().wlock()->clear();
  curl_session s{transport_, "bob"};
  s.source->refresh_all();
  EXPECT_TRUE(entries_of(*last_call("update_collections")).empty());
}

TEST_F(CurlStoreTest, HistoryPersistsAcrossSessions) {
  if (std::system("curl --version > /dev/null 2>&1") != 0)
    GTEST_SKIP() << "no curl binary on PATH";
  {
    curl_session s{transport_, "alice"};
    s.source->refresh_all();
    // Nothing listens on port 1: curl fails fast, and the attempt is still
    // recorded in History.
    s.source->on_send_requested(make_request_payload("", "http://127.0.0.1:1/"));
    ASSERT_EQ(entries_of(*last_call("update_history")).size(), 1u);
  }
  recorded_calls().wlock()->clear();
  curl_session s{transport_, "alice"};
  s.source->refresh_all();
  auto history = entries_of(*last_call("update_history"));
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].as<std::string>("url"_key), "http://127.0.0.1:1/");

  s.source->on_clear_history_requested();
  recorded_calls().wlock()->clear();
  curl_session again{transport_, "alice"};
  again.source->refresh_all();
  EXPECT_TRUE(entries_of(*last_call("update_history")).empty());
}

// ── Anonymous sessions ───────────────────────────────────────────────────────

TEST_F(CurlStoreTest, AnonymousSessionKeepsDataInMemoryAndSaysSo) {
  curl_session s{transport_, ""};
  s.source->refresh_all();

  auto log = last_call("append_command_log");
  ASSERT_TRUE(log.has_value());
  EXPECT_FALSE(log->as<bool>("ok"_key));
  EXPECT_NE(log->as<std::string>("output"_key).find("anonymous"), std::string::npos);

  s.source->on_save_request_requested(make_request_payload("Temp", "https://example.test"));
  EXPECT_EQ(entries_of(*last_call("update_collections")).size(), 1u);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "users"));
}
