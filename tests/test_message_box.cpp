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
  EXPECT_EQ(f->get_as<std::string>(), "none");
}

TEST_F(MessageBoxLocalTest, DefaultButtonsIsOk) {
  auto obj = dynamic::instantiate("wish"_key, "MessageBox"_key);
  auto* f = obj.findField("buttons"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->get_as<std::string>(), "ok");
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

// ── __construct: instantiate()-time params must take effect ──────────────────
//
// bison::rmi::server::handle_instantiate() calls form::init() (which runs
// on_init(), building the tree from the object's *current* field values --
// still prototype defaults at that point) strictly before it applies
// instantiate()'s params via the "__construct" hook. Without registering
// "__construct", those params would be silently dropped and never reach
// on_init() at all. These tests catch a regression of that wiring, notably
// for "buttons"/"icon" which change the tree's *structure*, not just a
// pass-through field like title/message on other forms.

class MessageBoxConstructTest : public ::testing::Test {
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

TEST_F(MessageBoxConstructTest, ConstructParamsSetTitleAndMessage) {
  dynamic params;
  params["title"_key] = std::string{"Confirm"};
  params["message"_key] = std::string{"Are you sure?"};
  auto proxy = client_->instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "Confirm");
  EXPECT_EQ(snapshot.as<std::string>("message"_key), "Are you sure?");

  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& win = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(win->findField("title"_key)->as<std::string>(), "Confirm");
}

TEST_F(MessageBoxConstructTest, ConstructParamsBuildYesNoCancelButtonRow) {
  dynamic params;
  params["buttons"_key] = std::string{"yes_no_cancel"};
  client_->instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();

  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_TRUE(objs.count(root + ".buttons.btn0"));
  ASSERT_TRUE(objs.count(root + ".buttons.btn1"));
  ASSERT_TRUE(objs.count(root + ".buttons.btn2"));
  EXPECT_EQ(objs.at(root + ".buttons.btn0")->findField("label"_key)->as<std::string>(), "Yes");
  EXPECT_EQ(objs.at(root + ".buttons.btn1")->findField("label"_key)->as<std::string>(), "No");
  EXPECT_EQ(objs.at(root + ".buttons.btn2")->findField("label"_key)->as<std::string>(), "Cancel");
}

TEST_F(MessageBoxConstructTest, ConstructParamsSetIconSrcAndMessage) {
  dynamic params;
  params["icon"_key] = std::string{"warning"};
  params["message"_key] = std::string{"Careful!"};
  client_->instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();

  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_TRUE(objs.count(root + ".body.icon"));
  EXPECT_EQ(objs.at(root + ".body.icon")->findField("src"_key)->as<std::string>(), "res/icons/msgbox_warning.png");
  ASSERT_TRUE(objs.count(root + ".body.message"));
  EXPECT_EQ(objs.at(root + ".body.message")->findField("text"_key)->as<std::string>(), "Careful!");
}

TEST_F(MessageBoxConstructTest, IconIsTintedToTextColor) {
  // The msgbox_*.png icons are white/monochrome, like the file-type icons
  // in file_browser_utils.cpp's make_name_cell() -- without
  // __tint_to_text_color__ they're invisible against the light theme's
  // white background (see render_image()'s handling in
  // imgui_ui_renderer.cpp).
  dynamic params;
  params["icon"_key] = std::string{"info"};
  client_->instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();

  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_TRUE(objs.count(root + ".body.icon"));
  EXPECT_TRUE(objs.at(root + ".body.icon")->findField("__tint_to_text_color__"_key)->as<bool>());
}

TEST_F(MessageBoxConstructTest, DefaultIconLeavesSrcEmpty) {
  // icon defaults to "none" -- the Image child should exist (still reserves
  // its declared 32x32 via render_image()'s Dummy() fallback) but with no
  // src to load.
  client_->instantiate("wish"_key, "MessageBox"_key).get();

  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_TRUE(objs.count(root + ".body.icon"));
  EXPECT_EQ(objs.at(root + ".body.icon")->findField("src"_key)->as<std::string>(), "");
}

TEST_F(MessageBoxConstructTest, ClickingButtonFromConstructedYesNoCancelEmitsCorrectResult) {
  dynamic params;
  params["buttons"_key] = std::string{"yes_no_cancel"};
  client_->instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();

  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());

  bison::key_t last_event{hash_t{0}};
  dynamic last_payload;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic payload) {
    last_event = event;
    last_payload = std::move(payload);
    if (prev)
      prev(bison::key_t{}, event, dynamic{});
  };

  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root + ".buttons.btn1"); // "No"
  ASSERT_NE(it, objs.end());
  auto btn_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
  auto h = srv_->last_session->top_level_handlers.find(root);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(btn_id, "clicked"_key, dynamic{});

  // form::emit() only enqueues into pending_events; delivery happens on the
  // render loop's next frame (see test_file_dialog.cpp's wait_for_event for
  // the same idiom), so poll briefly rather than asserting synchronously.
  auto t0 = std::chrono::steady_clock::now();
  while (last_event.id == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2000))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  EXPECT_EQ(last_event, "on_result"_key);
  EXPECT_EQ(last_payload.as<std::string>("button"_key), "no");
  // The internal Window is *not* removed synchronously on click: closing a
  // modal popup requires ImGui to actually observe the close (see
  // render_window()'s "__request_close__" handling in imgui_ui_renderer.cpp)
  // across a real frame, which this null_renderer-based harness never runs.
  // The window is still present, now flagged to close on the next render.
  ASSERT_FALSE(find_form_root(srv_->last_session->ui_objects).empty());
  auto* request_close_f = objs.at(root)->findField("__request_close__"_key);
  ASSERT_NE(request_close_f, nullptr);
  EXPECT_TRUE(request_close_f->as<bool>());
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

TEST_F(MessageBoxEventsTest, ClickingOkRequestsModalClose) {
  simulate_btn_click("btn0");
  ASSERT_TRUE(wait_for_event("on_result"_key));
  // Actual removal is deferred until ImGui confirms the popup closed (see
  // render_window()'s "__request_close__" handling in imgui_ui_renderer.cpp,
  // and MessageBoxConstructTest.ClickingButtonFromConstructedYesNoCancelEmitsCorrectResult's
  // comment) -- this null_renderer-based harness never runs a real frame, so
  // the window stays present, now flagged to close on the next render.
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_FALSE(find_form_root(objs).empty());
  auto* request_close_f = objs.at(root_)->findField("__request_close__"_key);
  ASSERT_NE(request_close_f, nullptr);
  EXPECT_TRUE(request_close_f->as<bool>());
}
