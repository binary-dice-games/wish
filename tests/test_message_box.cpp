// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class MessageBoxLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(MessageBoxLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "MessageBox"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "MessageBox"_key);
}

TEST_F(MessageBoxLocalTest, DefaultTitleIsMessage) {
  auto obj = dynamic::instantiate("wish"_key, "MessageBox"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "Message");
}

TEST_F(MessageBoxLocalTest, DefaultMessageIsEmpty) {
  auto obj = dynamic::instantiate("wish"_key, "MessageBox"_key);
  auto* f = obj.findField("message"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "");
}

TEST_F(MessageBoxLocalTest, DefaultIconIsNone) {
  auto obj = dynamic::instantiate("wish"_key, "MessageBox"_key);
  auto* f = obj.findField("icon"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "none");
}

TEST_F(MessageBoxLocalTest, DefaultButtonsIsOk) {
  auto obj = dynamic::instantiate("wish"_key, "MessageBox"_key);
  auto* f = obj.findField("buttons"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "ok");
}

// ── Session-capturing server for internal-tree tests ──────────────────────────

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

// Helper: find the root key for the internal form tree (starts with
// "__message_box_", no dot — i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__message_box_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class MessageBoxWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
  }

  void TearDown() override {
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(MessageBoxWindowTest, SessionObjectsHasFormRoot) {
  client_->instantiate("wish"_key, "MessageBox"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  EXPECT_FALSE(root.empty()) << "No __message_box_... root key in session.objects";
}

TEST_F(MessageBoxWindowTest, FormRootIsModalWindow) {
  client_->instantiate("wish"_key, "MessageBox"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
  auto* modal_f = obj->findField("modal"_key);
  ASSERT_NE(modal_f, nullptr);
  EXPECT_TRUE(modal_f->as<bool>());
}

TEST_F(MessageBoxWindowTest, FormRootIsNotClosable) {
  // A message box only closes via a button click, not a title-bar X.
  client_->instantiate("wish"_key, "MessageBox"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  auto* closable_f = obj->findField("closable"_key);
  EXPECT_FALSE(closable_f && closable_f->as<bool>());
}

TEST_F(MessageBoxWindowTest, DefaultOkPresetHasOneButton) {
  client_->instantiate("wish"_key, "MessageBox"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  EXPECT_TRUE(objs.count(root + ".buttons.btn0"));
  EXPECT_FALSE(objs.count(root + ".buttons.btn1"));
  EXPECT_EQ(objs.at(root + ".buttons.btn0")->findField("label"_key)->as<std::string>(), "OK");
}

// ── RMI fixture — checks server round-trips ───────────────────────────────────

class MessageBoxRMITest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<wish::server>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
  }

  void TearDown() override {
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  memory_server_transport transport_;
  std::unique_ptr<wish::server> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(MessageBoxRMITest, InstantiateReturnsValidProxy) {
  auto proxy = client_->instantiate("wish"_key, "MessageBox"_key).get();
  EXPECT_TRUE(proxy.valid());
}

TEST_F(MessageBoxRMITest, SetTitleRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "MessageBox"_key).get();
  dynamic params;
  params["title"_key] = std::string{"Confirm"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "Confirm");
}

TEST_F(MessageBoxRMITest, SetMessageRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "MessageBox"_key).get();
  dynamic params;
  params["message"_key] = std::string{"Are you sure?"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("message"_key), "Are you sure?");
}

// ── Event emission ────────────────────────────────────────────────────────────

struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class MessageBoxEventsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "MessageBox"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());

    // Wrap emit_event to capture high-level events emitted by form::emit().
    auto prev = std::move(srv_->last_session->emit_event);
    events_ = std::make_shared<std::vector<CapturedEvent>>();
    auto evts = events_;
    srv_->last_session->emit_event = [prev, evts](bison::key_t id, bison::key_t event, dynamic payload) {
      if (event == "on_result"_key)
        evts->push_back({event, payload});
      if (prev)
        prev(id, event, std::move(payload));
    };
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  void simulate_btn_click(const std::string& btn_key) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".buttons." + btn_key);
    ASSERT_NE(it, objs.end()) << "button not found: " << btn_key;
    auto btn_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(btn_id, "clicked"_key, dynamic{});
  }

  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto has = [&] {
      for (auto& e : *events_)
        if (e.name.id == name.id)
          return true;
      return false;
    };
    auto t0 = std::chrono::steady_clock::now();
    while (!has() && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has();
  }

  const CapturedEvent* find_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name.id == name.id)
        return &e;
    return nullptr;
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(MessageBoxEventsTest, ClickingOkEmitsOnResultWithOkButton) {
  simulate_btn_click("btn0");
  ASSERT_TRUE(wait_for_event("on_result"_key));
  auto* ev = find_event("on_result"_key);
  EXPECT_EQ(ev->payload.as<std::string>("button"_key), "ok");
}

TEST_F(MessageBoxEventsTest, ClickingOkRemovesInternalWindow) {
  simulate_btn_click("btn0");
  ASSERT_TRUE(wait_for_event("on_result"_key));
  EXPECT_TRUE(find_form_root(srv_->last_session->ui_objects).empty());
}
