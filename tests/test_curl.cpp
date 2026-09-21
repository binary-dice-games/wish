// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// __curl_N is the Request window (main root); __curl_N_<suffix> the rest --
// mirrors test_docker.cpp's find_form_root().
std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__curl_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    if (k.find('_', 7) != std::string::npos)
      continue;
    return k;
  }
  return {};
}

std::string find_root_with_prefix(const wish::name_map& objects, const std::string& prefix) {
  for (const auto& [k, _] : objects)
    if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
      return k;
  return {};
}

dynamic_ptr make_kv_array(const std::vector<std::tuple<std::string, std::string, bool>>& entries) {
  auto arr = std::make_shared<dynamic>();
  size_t i = 0;
  for (auto& [k, v, en] : entries) {
    auto e = std::make_shared<dynamic>();
    (*e)["key"_key] = k;
    (*e)["value"_key] = v;
    (*e)["enabled"_key] = en;
    (*arr)[i++] = dynamic_ptr{e};
  }
  return dynamic_ptr{arr};
}

} // namespace

class CurlLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(CurlLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "CurlFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "CurlFrontend"_key);
}

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

class CurlRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "CurlFrontend"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  dynamic call(bison::key_t method, dynamic args) {
    return proxy_->call(method, std::move(args)).get();
  }

  dynamic_ptr elem_at(const std::string& dot_path) const {
    auto it = srv_->last_session->ui_objects.find(root_ + "." + dot_path);
    return it == srv_->last_session->ui_objects.end() ? nullptr : it->second;
  }

  bison::key_t id_at(const std::string& dot_path) const {
    auto e = elem_at(dot_path);
    return e ? e->as<bison::key_t>("__wish_id"_key) : bison::key_t{};
  }

  // Counts only TableRow children -- a kv_table's / list table's "children"
  // map also holds the table's static TableColumn definitions (string
  // keys), which a plain "is this a live dynamic_ptr" count would include
  // too. Mirrors the CLASS-filtered walks docker.cpp's set_text_lines() /
  // this module's rebuild_response_headers() use to find rows to erase.
  size_t row_count(const std::string& table_path) const {
    auto it = srv_->last_session->ui_objects.find(table_path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    size_t n = 0;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() && f.as<dynamic_ptr>()->as<bison::key_t>(dynamic::CLASS) == "TableRow"_key)
        ++n;
    });
    return n;
  }

  // Only ever used to index into TableRow children (never TableColumns),
  // so it's safe to skip non-rows the same way row_count() does.
  // NOTE: a default-constructed dynamic_ptr is NOT falsy (unlike
  // std::shared_ptr) -- it wraps a freshly-allocated, empty dynamic object
  // (CLASS == 0), not a null pointer. So "found" can't double as its own
  // not-found sentinel the way parent/cf checks elsewhere in this fixture
  // do (those are always real, populated fields, never a bare default
  // dynamic_ptr); an explicit bool tracks whether a match was made.
  static dynamic_ptr nth_child(const dynamic_ptr& parent, size_t index) {
    if (!parent)
      return nullptr;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    dynamic_ptr found;
    bool have_found = false;
    size_t seen = 0;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (have_found || !f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      auto c = f.as<dynamic_ptr>();
      if (c->as<bison::key_t>(dynamic::CLASS) == "TableColumn"_key)
        return;
      if (seen++ == index) {
        found = c;
        have_found = true;
      }
    });
    return have_found ? found : nullptr;
  }

  dynamic_ptr row_at(const std::string& table_path, size_t index) const {
    auto it = srv_->last_session->ui_objects.find(table_path);
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    return nth_child(it->second, index);
  }

  // Finds the row's MenuButton by CLASS (not a hardcoded column index, since
  // History/Collections/Environments rows have different cell counts) and
  // returns the __wish_id of the MenuItem with the given label.
  bison::key_t menu_item_id_in_row(const dynamic_ptr& row, const std::string& label) const {
    auto* cf = row ? row->findField<dynamic_ptr>("children"_key) : nullptr;
    if (!cf || !*cf)
      return {};
    dynamic_ptr menu;
    bool have_menu = false;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (have_menu || !f.is<dynamic_ptr>())
        return;
      auto c = f.as<dynamic_ptr>();
      if (c && c->as<bison::key_t>(dynamic::CLASS) == "MenuButton"_key) {
        menu = c;
        have_menu = true;
      }
    });
    auto* mcf = have_menu ? menu->findField<dynamic_ptr>("children"_key) : nullptr;
    if (!mcf || !*mcf)
      return {};
    bison::key_t found{};
    (*mcf)->forEach([&](bison::key_t, const field& f) {
      if (found.id || !f.is<dynamic_ptr>())
        return;
      auto item = f.as<dynamic_ptr>();
      if (!item)
        return;
      auto* lf = item->findField<std::string>("label"_key);
      if (lf && *lf == label)
        found = item->as<bison::key_t>("__wish_id"_key);
    });
    return found;
  }

  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }
  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }

  static void wait_for(const bool& flag) {
    auto t0 = std::chrono::steady_clock::now();
    while (!flag && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Listens for the next occurrence of @p event_name on this session and
  // returns the captured payload via @p out once @p got is true.
  struct capture {
    bool got{false};
    dynamic payload;
  };
  std::shared_ptr<capture> capture_event(bison::key_t event_name) {
    auto c = std::make_shared<capture>();
    auto prev = std::move(srv_->last_session->emit_event);
    srv_->last_session->emit_event = [this, c, event_name, prev](bison::key_t id, bison::key_t event, dynamic payload) {
      if (event == event_name) {
        c->got = true;
        c->payload = payload.clone();
      }
      if (prev)
        prev(id, event, std::move(payload));
    };
    return c;
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

// ── Window construction ─────────────────────────────────────────────────

TEST_F(CurlRmiTest, InstantiationBuildsAllSixWindows) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.toolbar.method"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.toolbar.url"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.tabs.params_tab.p_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.tabs.headers_tab.h_table"));

  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_response").empty());
  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_history").empty());
  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_collections").empty());
  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_environments").empty());
  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_console").empty());
}

// ── Editable key-value tables ───────────────────────────────────────────

TEST_F(CurlRmiTest, AddParamRowIncreasesRowCount) {
  fire(id_at("vbox.tabs.params_tab.p_toolbar.btn_add"), "clicked"_key);
  fire(id_at("vbox.tabs.params_tab.p_toolbar.btn_add"), "clicked"_key);
  EXPECT_EQ(row_count(root_ + ".vbox.tabs.params_tab.p_table"), 2u);
}

TEST_F(CurlRmiTest, RemoveParamRowDeletesJustThatRow) {
  fire(id_at("vbox.tabs.params_tab.p_toolbar.btn_add"), "clicked"_key);
  fire(id_at("vbox.tabs.params_tab.p_toolbar.btn_add"), "clicked"_key);
  ASSERT_EQ(row_count(root_ + ".vbox.tabs.params_tab.p_table"), 2u);

  auto row0 = row_at(root_ + ".vbox.tabs.params_tab.p_table", 0);
  ASSERT_TRUE(row0);
  auto remove_btn = nth_child(row0, 3); // enabled, key, value, remove
  ASSERT_TRUE(remove_btn);
  fire(remove_btn->as<bison::key_t>("__wish_id"_key), "clicked"_key);

  EXPECT_EQ(row_count(root_ + ".vbox.tabs.params_tab.p_table"), 1u);
}

// ── Send / Save ──────────────────────────────────────────────────────────

TEST_F(CurlRmiTest, SendEmitsSendRequestedWithCurrentBuilderState) {
  (*elem_at("vbox.toolbar.method"))["value"_key] = int32_t{1}; // POST
  (*elem_at("vbox.toolbar.url"))["value"_key] = std::string{"https://api.example.com/users"};

  fire(id_at("vbox.tabs.headers_tab.h_toolbar.btn_add"), "clicked"_key);
  auto row0 = row_at(root_ + ".vbox.tabs.headers_tab.h_table", 0);
  ASSERT_TRUE(row0);
  (*nth_child(row0, 1))["value"_key] = std::string{"X-Test"};
  (*nth_child(row0, 2))["value"_key] = std::string{"1"};

  auto c = capture_event("send_requested"_key);
  fire(id_at("vbox.toolbar.btn_send"), "clicked"_key);
  wait_for(c->got);
  ASSERT_TRUE(c->got);
  EXPECT_EQ(c->payload.as<std::string>("method"_key), "POST");
  EXPECT_EQ(c->payload.as<std::string>("url"_key), "https://api.example.com/users");

  const auto* headers_f = c->payload.findField<dynamic_ptr>("headers"_key);
  ASSERT_NE(headers_f, nullptr);
  ASSERT_TRUE(*headers_f);
  auto& f0 = (*headers_f)->at(0);
  ASSERT_TRUE(f0.is<dynamic_ptr>());
  auto entry = f0.as<dynamic_ptr>();
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->as<std::string>("key"_key), "X-Test");
  EXPECT_EQ(entry->as<std::string>("value"_key), "1");
}

TEST_F(CurlRmiTest, SaveWithBlankNameDoesNotEmit) {
  auto c = capture_event("save_request_requested"_key);
  fire(id_at("vbox.save_bar.btn_save"), "clicked"_key);
  wait_for(c->got);
  EXPECT_FALSE(c->got);
}

TEST_F(CurlRmiTest, SaveEmitsSaveRequestedWithNameAndCollection) {
  (*elem_at("vbox.save_bar.save_name"))["value"_key] = std::string{"Get Users"};
  (*elem_at("vbox.save_bar.save_collection"))["value"_key] = std::string{"Users API"};

  auto c = capture_event("save_request_requested"_key);
  fire(id_at("vbox.save_bar.btn_save"), "clicked"_key);
  wait_for(c->got);
  ASSERT_TRUE(c->got);
  EXPECT_EQ(c->payload.as<std::string>("name"_key), "Get Users");
  EXPECT_EQ(c->payload.as<std::string>("collection"_key), "Users API");
}

// ── Body / Auth mode visibility ──────────────────────────────────────────

TEST_F(CurlRmiTest, BodyModeJsonShowsRawBoxOnly) {
  fire(id_at("vbox.tabs.body_tab.mode"), "changed"_key, [] { dynamic d; d["value"_key] = int32_t{2}; return d; }());
  EXPECT_TRUE(elem_at("vbox.tabs.body_tab.raw_box")->as<bool>("visible"_key));
  EXPECT_FALSE(elem_at("vbox.tabs.body_tab.form_box")->as<bool>("visible"_key));
}

TEST_F(CurlRmiTest, BodyModeFormShowsFormBoxOnly) {
  fire(id_at("vbox.tabs.body_tab.mode"), "changed"_key, [] { dynamic d; d["value"_key] = int32_t{3}; return d; }());
  EXPECT_FALSE(elem_at("vbox.tabs.body_tab.raw_box")->as<bool>("visible"_key));
  EXPECT_TRUE(elem_at("vbox.tabs.body_tab.form_box")->as<bool>("visible"_key));
}

TEST_F(CurlRmiTest, AuthModeBearerShowsBearerBoxOnly) {
  fire(id_at("vbox.tabs.auth_tab.mode"), "changed"_key, [] { dynamic d; d["value"_key] = int32_t{2}; return d; }());
  EXPECT_FALSE(elem_at("vbox.tabs.auth_tab.basic_box")->as<bool>("visible"_key));
  EXPECT_TRUE(elem_at("vbox.tabs.auth_tab.bearer_box")->as<bool>("visible"_key));
}

// ── Response rendering ──────────────────────────────────────────────────

TEST_F(CurlRmiTest, UpdateResponseRendersSuccessStatusAndHeaders) {
  dynamic args;
  args["ok"_key] = true;
  args["status_code"_key] = int32_t{200};
  args["status_text"_key] = std::string{"OK"};
  args["time_ms"_key] = 42.0f;
  args["size_bytes"_key] = 128.0f;
  args["headers"_key] = make_kv_array({{"Content-Type", "application/json", true}});
  args["body_file"_key] = std::string{};
  args["body_is_json"_key] = true;
  call("update_response"_key, std::move(args));

  auto response_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_response");
  ASSERT_FALSE(response_root.empty());
  auto status = srv_->last_session->ui_objects.at(response_root + ".vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("200"), std::string::npos);
  EXPECT_EQ(row_count(response_root + ".vbox.tabs.headers_tab.h_table"), 1u);
}

TEST_F(CurlRmiTest, UpdateResponseFailureShowsError) {
  dynamic args;
  args["ok"_key] = false;
  args["error"_key] = std::string{"Could not resolve host"};
  call("update_response"_key, std::move(args));

  auto response_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_response");
  ASSERT_FALSE(response_root.empty());
  auto status = srv_->last_session->ui_objects.at(response_root + ".vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("Could not resolve host"), std::string::npos);
}

// ── Request builder pre-fill ────────────────────────────────────────────

TEST_F(CurlRmiTest, UpdateRequestBuilderPrefillsFieldsAndVisibility) {
  dynamic args;
  args["method"_key] = std::string{"POST"};
  args["url"_key] = std::string{"https://api.example.com/login"};
  args["params"_key] = make_kv_array({});
  args["headers"_key] = make_kv_array({{"Accept", "application/json", true}});
  args["body_mode"_key] = std::string{"json"};
  args["body_text"_key] = std::string{"{\"user\":\"a\"}"};
  args["form_fields"_key] = make_kv_array({});
  args["auth_mode"_key] = std::string{"bearer"};
  args["auth_username"_key] = std::string{};
  args["auth_password"_key] = std::string{};
  args["auth_token"_key] = std::string{"tok123"};
  call("update_request_builder"_key, std::move(args));

  EXPECT_EQ(elem_at("vbox.toolbar.method")->as<int32_t>("value"_key), 1);
  EXPECT_EQ(elem_at("vbox.toolbar.url")->as<std::string>("value"_key), "https://api.example.com/login");
  EXPECT_EQ(row_count(root_ + ".vbox.tabs.headers_tab.h_table"), 1u);
  EXPECT_TRUE(elem_at("vbox.tabs.body_tab.raw_box")->as<bool>("visible"_key));
  EXPECT_TRUE(elem_at("vbox.tabs.auth_tab.bearer_box")->as<bool>("visible"_key));
  EXPECT_EQ(elem_at("vbox.tabs.auth_tab.bearer_box.token")->as<std::string>("value"_key), "tok123");
}

// ── History / Collections / Environments ────────────────────────────────

TEST_F(CurlRmiTest, UpdateHistoryPopulatesTableAndLoadEmits) {
  dynamic args;
  dynamic arr;
  auto e = std::make_shared<dynamic>();
  (*e)["id"_key] = std::string{"h1"};
  (*e)["method"_key] = std::string{"GET"};
  (*e)["url"_key] = std::string{"https://x/y"};
  (*e)["status_code"_key] = int32_t{200};
  (*e)["ok"_key] = true;
  (*e)["time_ms"_key] = 10.0f;
  (*e)["timestamp"_key] = std::string{"2026-01-01 00:00:00"};
  arr[0] = dynamic_ptr{e};
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  call("update_history"_key, std::move(args));

  auto history_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_history");
  ASSERT_FALSE(history_root.empty());
  ASSERT_EQ(row_count(history_root + ".vbox.table"), 1u);

  auto row0 = row_at(history_root + ".vbox.table", 0);
  auto load_id = menu_item_id_in_row(row0, "Load");
  ASSERT_TRUE(load_id.id);

  auto c = capture_event("load_history_requested"_key);
  fire_at(history_root, load_id, "clicked"_key);
  wait_for(c->got);
  ASSERT_TRUE(c->got);
  EXPECT_EQ(c->payload.as<std::string>("id"_key), "h1");
}

TEST_F(CurlRmiTest, CollectionsDeleteGoesThroughConfirm) {
  dynamic args;
  dynamic arr;
  auto e = std::make_shared<dynamic>();
  (*e)["id"_key] = std::string{"r1"};
  (*e)["collection"_key] = std::string{"Default"};
  (*e)["name"_key] = std::string{"Get Users"};
  (*e)["method"_key] = std::string{"GET"};
  (*e)["url"_key] = std::string{"https://x/users"};
  arr[0] = dynamic_ptr{e};
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  call("update_collections"_key, std::move(args));

  auto collections_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_collections");
  ASSERT_FALSE(collections_root.empty());
  auto row0 = row_at(collections_root + ".vbox.table", 0);
  auto delete_id = menu_item_id_in_row(row0, "Delete");
  ASSERT_TRUE(delete_id.id);
  fire_at(collections_root, delete_id, "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  auto c = capture_event("delete_request_requested"_key);
  fire_at(confirm_root, yes_id, "clicked"_key);
  wait_for(c->got);
  ASSERT_TRUE(c->got);
  EXPECT_EQ(c->payload.as<std::string>("id"_key), "r1");
}

TEST_F(CurlRmiTest, EnvironmentsEditEmitsSelectEnvironmentRequested) {
  dynamic args;
  dynamic arr;
  auto e = std::make_shared<dynamic>();
  (*e)["id"_key] = std::string{"e1"};
  (*e)["name"_key] = std::string{"Local"};
  (*e)["var_count"_key] = int32_t{2};
  arr[0] = dynamic_ptr{e};
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  call("update_environments"_key, std::move(args));

  auto environments_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_environments");
  ASSERT_FALSE(environments_root.empty());

  auto env_combo = elem_at("vbox.toolbar.env");
  EXPECT_NE(env_combo->as<std::string>("items"_key).find("Local"), std::string::npos);

  auto row0 = row_at(environments_root + ".vbox.table", 0);
  auto edit_id = menu_item_id_in_row(row0, "Edit");
  ASSERT_TRUE(edit_id.id);

  auto c = capture_event("select_environment_requested"_key);
  fire_at(environments_root, edit_id, "clicked"_key);
  wait_for(c->got);
  ASSERT_TRUE(c->got);
  EXPECT_EQ(c->payload.as<std::string>("id"_key), "e1");
}

TEST_F(CurlRmiTest, UpdateEnvironmentVarsPopulatesVarsTable) {
  dynamic args;
  args["environment_id"_key] = std::string{"e1"};
  args["name"_key] = std::string{"Local"};
  args["vars"_key] = make_kv_array({{"base_url", "http://localhost:8080", true}});
  call("update_environment_vars"_key, std::move(args));

  auto environments_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_environments");
  ASSERT_FALSE(environments_root.empty());
  EXPECT_EQ(row_count(environments_root + ".vbox.vars_table"), 1u);
  auto label = srv_->last_session->ui_objects.at(environments_root + ".vbox.editor_label")->as<std::string>("text"_key);
  EXPECT_NE(label.find("Local"), std::string::npos);
}

// ── Teardown ─────────────────────────────────────────────────────────────

TEST_F(CurlRmiTest, ClosingWindowEmitsClosedAndTearsDownAllRoots) {
  auto response_root = find_root_with_prefix(srv_->last_session->ui_objects, root_ + "_response");
  ASSERT_FALSE(response_root.empty());

  auto window_id = srv_->last_session->ui_objects.at(root_)->as<bison::key_t>("__wish_id"_key);
  auto c = capture_event("closed"_key);
  fire(window_id, "closed"_key);
  wait_for(c->got);
  ASSERT_TRUE(c->got);
  EXPECT_FALSE(srv_->last_session->ui_objects.count(response_root));
}
