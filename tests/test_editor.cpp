// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <fstream>
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

constexpr const char* kValidUi = R"({
  "type": "Window",
  "title": "Mock",
  "children": {
    "main": {
      "type": "VerticalLayout",
      "children": {
        "ok": { "type": "Button", "label": "OK" }
      }
    }
  }
})";

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class EditorLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(EditorLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Editor"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Editor"_key);
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
// "__editor_", no dot, and not the "_mock" suffix used for the preview root).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__editor_", 0) == 0 && k.find('.') == std::string::npos && k.rfind("_mock") == std::string::npos)
      return k;
  }
  return {};
}

// ── Chrome construction ───────────────────────────────────────────────────────

class EditorWindowTest : public ::testing::Test {
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

  std::string instantiate_and_get_root() {
    client_->instantiate("wish"_key, "Editor"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(EditorWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __editor_... root key in session.objects";
}

TEST_F(EditorWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(EditorWindowTest, TreeContainsSource) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.source"));
}

TEST_F(EditorWindowTest, TreeContainsBanner) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.banner"));
}

TEST_F(EditorWindowTest, TreeContainsLog) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.log"));
}

// ── set_source / reparse / event log ──────────────────────────────────────────

struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class EditorSourceTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Editor"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());
    mock_root_ = root_ + "_mock";

    auto prev = std::move(srv_->last_session->emit_event);
    events_ = std::make_shared<std::vector<CapturedEvent>>();
    auto evts = events_;
    srv_->last_session->emit_event = [prev, evts](bison::key_t id, bison::key_t event, dynamic payload) {
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

  void seed_sandbox_file(const std::string& name, const std::string& content) {
    std::ofstream out(srv_->last_session->resource_dir / name, std::ios::binary);
    out << content;
  }

  dynamic set_source(const std::string& path) {
    dynamic args;
    args["path"_key] = path;
    return proxy_->call("set_source"_key, std::move(args)).get();
  }

  std::string banner_text() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.banner");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<std::string>("text"_key);
  }

  bool mock_registered() const {
    return srv_->last_session->top_level_objects.count(bison::key_t{mock_root_}) != 0
        && srv_->last_session->ui_objects.count(mock_root_) != 0;
  }

  bison::key_t mock_widget_id(const std::string& dot_suffix) const {
    auto it = srv_->last_session->ui_objects.find(mock_root_ + "." + dot_suffix);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<bison::key_t>("__wish_id"_key);
  }

  void simulate_mock_event(bison::key_t widget_id, bison::key_t event) {
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{mock_root_});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(widget_id, event, dynamic{});
  }

  // The `text` field of the second cell in the log table's row at `index`
  // (0-based, in append order). Mirrors notepad's editor_at() helper.
  std::optional<std::string> log_row_text(size_t index) const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.log");
    if (it == srv_->last_session->ui_objects.end())
      return std::nullopt;
    auto* cf = it->second->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>() || !cf->as<dynamic_ptr>())
      return std::nullopt;
    auto& children = *cf->as<dynamic_ptr>();
    auto& row_f = children.at(index);
    if (!row_f.is<dynamic_ptr>() || !row_f.as<dynamic_ptr>())
      return std::nullopt;
    auto& row = *row_f.as<dynamic_ptr>();
    auto* rcf = row.findField("children"_key);
    if (!rcf || !rcf->is<dynamic_ptr>() || !rcf->as<dynamic_ptr>())
      return std::nullopt;
    auto& cell_f = rcf->as<dynamic_ptr>()->at(size_t{1});
    if (!cell_f.is<dynamic_ptr>() || !cell_f.as<dynamic_ptr>())
      return std::nullopt;
    return cell_f.as<dynamic_ptr>()->as<std::string>("text"_key);
  }

  bool has_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name == name)
        return true;
    return false;
  }

  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto t0 = std::chrono::steady_clock::now();
    while (!has_event(name) && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has_event(name);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  std::string mock_root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(EditorSourceTest, ValidJsonInstantiatesPreview) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json");

  EXPECT_TRUE(mock_registered());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(mock_root_ + ".main.ok"));
  EXPECT_EQ(banner_text(), "");
}

TEST_F(EditorSourceTest, InvalidJsonSetsBannerAndKeepsPreviousPreview) {
  seed_sandbox_file("good.json", kValidUi);
  set_source("good.json");
  ASSERT_TRUE(mock_registered());
  ASSERT_TRUE(srv_->last_session->ui_objects.count(mock_root_ + ".main.ok"));

  seed_sandbox_file("bad.json", "{ this is not valid json");
  set_source("bad.json");

  EXPECT_NE(banner_text(), "");
  // The previous (still valid) preview must be untouched -- no flicker.
  EXPECT_TRUE(mock_registered());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(mock_root_ + ".main.ok"));
}

TEST_F(EditorSourceTest, ReparsingValidJsonClearsBanner) {
  seed_sandbox_file("bad.json", "{ nope");
  set_source("bad.json");
  ASSERT_NE(banner_text(), "");

  seed_sandbox_file("good.json", kValidUi);
  set_source("good.json");
  EXPECT_EQ(banner_text(), "");
}

TEST_F(EditorSourceTest, MockWidgetEventIsLogged) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json");
  ASSERT_TRUE(mock_registered());

  auto ok_id = mock_widget_id("main.ok");
  ASSERT_NE(ok_id.id, 0u);
  simulate_mock_event(ok_id, "clicked"_key);

  auto row = log_row_text(0);
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(*row, "main.ok clicked");
}

TEST_F(EditorSourceTest, SourceEditorSavedEmitsOnSourceSaved) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json");

  auto source_id = srv_->last_session->ui_objects.at(root_ + ".vbox.source")->as<bison::key_t>("__wish_id"_key);
  auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(source_id, "saved"_key, dynamic{});

  EXPECT_TRUE(wait_for_event("on_source_saved"_key));
}

TEST_F(EditorSourceTest, WindowClosedRemovesChromeAndPreview) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json");
  ASSERT_TRUE(mock_registered());

  auto win_id = srv_->last_session->ui_objects.at(root_)->as<bison::key_t>("__wish_id"_key);
  auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(win_id, "closed"_key, dynamic{});

  EXPECT_TRUE(wait_for_event("closed"_key));
  EXPECT_FALSE(mock_registered());
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_));
}
