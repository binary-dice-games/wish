// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <registry.hpp>
#include <server.hpp>
#include <context.hpp>
#include <ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class NotepadLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(NotepadLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Notepad"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Notepad"_key);
}

TEST_F(NotepadLocalTest, DefaultTitleIsNotepad) {
  auto obj = dynamic::instantiate("wish"_key, "Notepad"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Notepad");
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

// Helper: find the root key for the internal form tree (starts with "__notepad_",
// no dot — i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__notepad_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class NotepadWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "Notepad"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(NotepadWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __notepad_... root key in session.objects";
}

TEST_F(NotepadWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(NotepadWindowTest, TreeContainsBtnOpen) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.toolbar.btn_open"));
}

TEST_F(NotepadWindowTest, TreeContainsBtnNew) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.toolbar.btn_new"));
}

TEST_F(NotepadWindowTest, TreeContainsBtnSync) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.toolbar.btn_sync"));
}

TEST_F(NotepadWindowTest, TreeContainsTabBar) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.tab_bar"));
}

// ── open_file / tab management ────────────────────────────────────────────────

struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class NotepadFilesTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Notepad"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());

    // Wrap emit_event to capture every high-level event emitted by form::emit().
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

  // Write a file directly into the session's sandbox, as if the client had
  // already called upload_file for it.
  void seed_sandbox_file(const std::string& name, const std::string& content) {
    std::ofstream out(srv_->last_session->resource_dir / name, std::ios::binary);
    out << content;
  }

  dynamic open_file(const std::string& path, const std::string& title = "") {
    dynamic args;
    args["path"_key] = path;
    if (!title.empty())
      args["title"_key] = title;
    return proxy_->call("open_file"_key, std::move(args)).get();
  }

  size_t tab_count() const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.tab_bar");
    if (it == objs.end() || !it->second)
      return 0;
    auto* cf = it->second->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>() || !cf->as<dynamic_ptr>())
      return 0;
    return cf->as<dynamic_ptr>()->size();
  }

  // The sole TextEditor child of the tab stored at children[child_key].
  dynamic_ptr editor_at(size_t child_key) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.tab_bar");
    if (it == objs.end() || !it->second)
      return {};
    auto* cf = it->second->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>() || !cf->as<dynamic_ptr>())
      return {};
    auto& tab_f = cf->as<dynamic_ptr>()->at(child_key);
    if (!tab_f.is<dynamic_ptr>() || !tab_f.as<dynamic_ptr>())
      return {};
    auto& tab = *tab_f.as<dynamic_ptr>();
    auto* tcf = tab.findField("children"_key);
    if (!tcf || !tcf->is<dynamic_ptr>() || !tcf->as<dynamic_ptr>())
      return {};
    auto& editor_f = tcf->as<dynamic_ptr>()->at(size_t{0});
    if (!editor_f.is<dynamic_ptr>())
      return {};
    return editor_f.as<dynamic_ptr>();
  }

  bison::key_t tab_id_at(size_t child_key) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.tab_bar");
    if (it == objs.end() || !it->second)
      return {};
    auto* cf = it->second->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>() || !cf->as<dynamic_ptr>())
      return {};
    auto& tab_f = cf->as<dynamic_ptr>()->at(child_key);
    if (!tab_f.is<dynamic_ptr>() || !tab_f.as<dynamic_ptr>())
      return {};
    return tab_f.as<dynamic_ptr>()->as<bison::key_t>("__wish_id"_key);
  }

  bool has_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name == name)
        return true;
    return false;
  }

  // form::emit() defers delivery to the render loop's next frame (see
  // session.hpp's contract on emit_event), so a form-level event is not
  // necessarily captured yet the instant a simulate_*/open_file() call
  // returns. Spin briefly for it, same idiom as test_integration.cpp's event
  // round-trip test.
  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto t0 = std::chrono::steady_clock::now();
    while (!has_event(name) && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has_event(name);
  }

  std::vector<CapturedEvent> events_of(bison::key_t name) const {
    std::vector<CapturedEvent> result;
    for (auto& e : *events_)
      if (e.name == name)
        result.push_back(e);
    return result;
  }

  void simulate_window_closed() {
    auto win_id = srv_->last_session->ui_objects.at(root_)->as<bison::key_t>("__wish_id"_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(win_id, "closed"_key, dynamic{});
  }

  void simulate_tab_closed(size_t child_key) {
    auto tab_id = tab_id_at(child_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(tab_id, "closed"_key, dynamic{});
  }

  void simulate_btn_click(const std::string& btn_key) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.toolbar." + btn_key);
    ASSERT_NE(it, objs.end()) << "button not found: " << btn_key;
    auto btn_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(btn_id, "clicked"_key, dynamic{});
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(NotepadFilesTest, OpenFileCreatesTab) {
  seed_sandbox_file("hello.py", "print('hi')");
  open_file("hello.py");

  EXPECT_EQ(tab_count(), 1u);
  auto editor = editor_at(0);
  ASSERT_TRUE(editor);
  EXPECT_EQ(editor->as<std::string>("file_path"_key), "hello.py");
  EXPECT_EQ(editor->as<std::string>("language"_key), "python");
}

TEST_F(NotepadFilesTest, OpenFileEmitsOnFileOpened) {
  seed_sandbox_file("notes.txt", "hello");
  open_file("notes.txt", "notes.txt");

  ASSERT_TRUE(wait_for_event("on_file_opened"_key));
  auto evts = events_of("on_file_opened"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "notes.txt");
  EXPECT_EQ(evts[0].payload.as<std::string>("title"_key), "notes.txt");
}

TEST_F(NotepadFilesTest, OpenFileTwiceIsIdempotent) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt");
  open_file("a.txt");
  EXPECT_EQ(tab_count(), 1u);
}

TEST_F(NotepadFilesTest, OpenSecondFileAddsSecondTab) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.md", "two");
  open_file("a.txt");
  open_file("b.md");
  EXPECT_EQ(tab_count(), 2u);
}

TEST_F(NotepadFilesTest, PathTraversalEmitsOnErrorAndNoTab) {
  open_file("../escape.txt");
  EXPECT_EQ(tab_count(), 0u);
  EXPECT_TRUE(wait_for_event("on_error"_key));
  EXPECT_FALSE(has_event("on_file_opened"_key));
}

TEST_F(NotepadFilesTest, TabClosedEmitsOnFileClosedAndRemovesTab) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt");
  ASSERT_EQ(tab_count(), 1u);

  simulate_tab_closed(0);

  EXPECT_EQ(tab_count(), 0u);
  ASSERT_TRUE(wait_for_event("on_file_closed"_key));
  auto evts = events_of("on_file_closed"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "a.txt");
}

TEST_F(NotepadFilesTest, SyncClickedEmitsAllOpenPaths) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt");
  open_file("b.txt");

  simulate_btn_click("btn_sync");

  ASSERT_TRUE(wait_for_event("on_sync_requested"_key));
  auto evts = events_of("on_sync_requested"_key);
  ASSERT_EQ(evts.size(), 1u);
  auto* paths_f = evts[0].payload.findField<dynamic_ptr>("paths"_key);
  ASSERT_NE(paths_f, nullptr);
  ASSERT_TRUE(*paths_f);
  std::vector<std::string> paths;
  (*paths_f)->forEach([&](bison::key_t, const field& f) {
    if (f.is<std::string>())
      paths.push_back(f.as<std::string>());
  });
  EXPECT_EQ(paths.size(), 2u);
  EXPECT_NE(std::find(paths.begin(), paths.end(), "a.txt"), paths.end());
  EXPECT_NE(std::find(paths.begin(), paths.end(), "b.txt"), paths.end());
}

TEST_F(NotepadFilesTest, OpenButtonClickedEmitsOnRequestOpen) {
  simulate_btn_click("btn_open");
  EXPECT_TRUE(wait_for_event("on_request_open"_key));
}

TEST_F(NotepadFilesTest, NewButtonClickedEmitsOnRequestNew) {
  simulate_btn_click("btn_new");
  EXPECT_TRUE(wait_for_event("on_request_new"_key));
}

TEST_F(NotepadFilesTest, WindowClosedFlushesEveryOpenFileThenCloses) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt");
  open_file("b.txt");

  simulate_window_closed();

  // Waiting for "closed" (enqueued last, delivered in the same FIFO order)
  // guarantees the earlier on_file_closed events have arrived too.
  ASSERT_TRUE(wait_for_event("closed"_key));

  auto closed_evts = events_of("on_file_closed"_key);
  EXPECT_EQ(closed_evts.size(), 2u);
  std::vector<std::string> paths;
  for (auto& e : closed_evts)
    paths.push_back(e.payload.as<std::string>("path"_key));
  EXPECT_NE(std::find(paths.begin(), paths.end(), "a.txt"), paths.end());
  EXPECT_NE(std::find(paths.begin(), paths.end(), "b.txt"), paths.end());

  // "closed" must fire after every on_file_closed, matching the documented
  // "flush before teardown" contract.
  size_t last_file_closed_idx = 0;
  size_t closed_idx = 0;
  for (size_t i = 0; i < events_->size(); ++i) {
    if ((*events_)[i].name == "on_file_closed"_key)
      last_file_closed_idx = i;
    if ((*events_)[i].name == "closed"_key)
      closed_idx = i;
  }
  EXPECT_GT(closed_idx, last_file_closed_idx);
}
