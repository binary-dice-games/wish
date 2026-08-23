// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

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

class NanoLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(NanoLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Nano"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Nano"_key);
}

TEST_F(NanoLocalTest, DefaultTitleIsNano) {
  auto obj = dynamic::instantiate("wish"_key, "Nano"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Nano");
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

// Helper: find the root key for the internal form tree (starts with "__nano_",
// no dot — i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__nano_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class NanoWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "Nano"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(NanoWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __nano_... root key in session.objects";
}

TEST_F(NanoWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(NanoWindowTest, TreeContainsBtnOpen) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.toolbar.btn_open"));
}

TEST_F(NanoWindowTest, TreeContainsBtnNew) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.toolbar.btn_new"));
}

TEST_F(NanoWindowTest, TreeContainsBtnSave) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.toolbar.btn_save"));
}

TEST_F(NanoWindowTest, TreeContainsTabBar) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.tab_bar"));
}

// ── open_file / tab management ────────────────────────────────────────────────

struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class NanoFilesTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Nano"_key).get());
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

  // The tab's Nth child element (0: language Combo, 1: TextEditor) for the
  // tab stored at children[child_key].
  dynamic_ptr tab_child_at(size_t child_key, size_t tab_child_index) const {
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
    auto& child_f = tcf->as<dynamic_ptr>()->at(tab_child_index);
    if (!child_f.is<dynamic_ptr>())
      return {};
    return child_f.as<dynamic_ptr>();
  }

  dynamic_ptr lang_combo_at(size_t child_key) const { return tab_child_at(child_key, 0); }
  dynamic_ptr editor_at(size_t child_key) const { return tab_child_at(child_key, 1); }

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

  bison::key_t editor_id_at(size_t child_key) const {
    auto ed = editor_at(child_key);
    if (!ed)
      return {};
    return ed->as<bison::key_t>("__wish_id"_key);
  }

  std::string tab_label_at(size_t child_key) const {
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
    return tab_f.as<dynamic_ptr>()->as<std::string>("label"_key);
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

  void simulate_tab_selected(size_t child_key) {
    auto tab_id = tab_id_at(child_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(tab_id, "selected"_key, dynamic{});
  }

  void simulate_editor_changed(size_t child_key) {
    auto ed_id = editor_id_at(child_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(ed_id, "changed"_key, dynamic{});
  }

  dynamic confirm_close(bool save) {
    dynamic args;
    args["save"_key] = save;
    return proxy_->call("confirm_close"_key, std::move(args)).get();
  }

  void simulate_lang_combo_changed(size_t child_key, int32_t value) {
    auto combo = lang_combo_at(child_key);
    ASSERT_TRUE(combo);
    auto combo_id = combo->as<bison::key_t>("__wish_id"_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    dynamic payload;
    payload["value"_key] = value;
    h->second->on_event(combo_id, "changed"_key, payload);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(NanoFilesTest, OpenFileCreatesTab) {
  seed_sandbox_file("hello.py", "print('hi')");
  open_file("hello.py");

  EXPECT_EQ(tab_count(), 1u);
  auto editor = editor_at(0);
  ASSERT_TRUE(editor);
  EXPECT_EQ(editor->as<std::string>("file_path"_key), "hello.py");
  EXPECT_EQ(editor->as<std::string>("language"_key), "python");
}

TEST_F(NanoFilesTest, OpenFileSeedsLangComboFromExtension) {
  seed_sandbox_file("hello.py", "print('hi')");
  open_file("hello.py");

  auto combo = lang_combo_at(0);
  ASSERT_TRUE(combo);
  auto* items_f = combo->findField<std::string>("items"_key);
  ASSERT_NE(items_f, nullptr);
  EXPECT_NE(items_f->find("python"), std::string::npos);

  // "python" is index 7 in nano.cpp's kLanguages table.
  EXPECT_EQ(combo->as<int32_t>("value"_key), 7);
}

TEST_F(NanoFilesTest, ChangingLangComboRetargetsEditorWithoutMarkingDirty) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  ASSERT_EQ(editor_at(0)->as<std::string>("language"_key), "none");

  // Index 1 is "cpp" in nano.cpp's kLanguages table.
  simulate_lang_combo_changed(0, 1);

  EXPECT_EQ(editor_at(0)->as<std::string>("language"_key), "cpp");
  EXPECT_EQ(tab_label_at(0), "a.txt"); // unaffected: a display-only change, not a content edit
}

TEST_F(NanoFilesTest, OpenFileEmitsOnFileOpened) {
  seed_sandbox_file("notes.txt", "hello");
  open_file("notes.txt", "notes.txt");

  ASSERT_TRUE(wait_for_event("on_file_opened"_key));
  auto evts = events_of("on_file_opened"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "notes.txt");
  EXPECT_EQ(evts[0].payload.as<std::string>("title"_key), "notes.txt");
}

TEST_F(NanoFilesTest, OpenFileTwiceIsIdempotent) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt");
  open_file("a.txt");
  EXPECT_EQ(tab_count(), 1u);
}

TEST_F(NanoFilesTest, OpenSecondFileAddsSecondTab) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.md", "two");
  open_file("a.txt");
  open_file("b.md");
  EXPECT_EQ(tab_count(), 2u);
}

TEST_F(NanoFilesTest, PathTraversalEmitsOnErrorAndNoTab) {
  open_file("../escape.txt");
  EXPECT_EQ(tab_count(), 0u);
  EXPECT_TRUE(wait_for_event("on_error"_key));
  EXPECT_FALSE(has_event("on_file_opened"_key));
}

TEST_F(NanoFilesTest, TabClosedEmitsOnFileClosedAndRemovesTab) {
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

TEST_F(NanoFilesTest, EditingMarksTabDirtyWithAsteriskSuffix) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  EXPECT_EQ(tab_label_at(0), "a.txt");

  simulate_editor_changed(0);
  EXPECT_EQ(tab_label_at(0), "a.txt *");
}

TEST_F(NanoFilesTest, SaveClickedSavesOnlyTheActiveFile) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");

  // The first tab opened is active by default (see nano.cpp's do_open_file
  // comment); dirty both, but only the active one should be saved.
  simulate_editor_changed(0);
  simulate_editor_changed(1);
  ASSERT_EQ(tab_label_at(0), "a.txt *");
  ASSERT_EQ(tab_label_at(1), "b.txt *");

  simulate_btn_click("btn_save");

  ASSERT_TRUE(wait_for_event("on_file_saved"_key));
  auto evts = events_of("on_file_saved"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "a.txt");
  EXPECT_EQ(tab_label_at(0), "a.txt");
  EXPECT_EQ(tab_label_at(1), "b.txt *");
}

TEST_F(NanoFilesTest, SelectingTabChangesWhichFileSaveTargets) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");

  simulate_tab_selected(1);
  simulate_editor_changed(1);
  simulate_btn_click("btn_save");

  ASSERT_TRUE(wait_for_event("on_file_saved"_key));
  auto evts = events_of("on_file_saved"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "b.txt");
}

TEST_F(NanoFilesTest, WindowClosedWithDirtyFileAsksForConfirmationInstead) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  simulate_editor_changed(0);

  simulate_window_closed();

  ASSERT_TRUE(wait_for_event("on_confirm_close"_key));
  EXPECT_FALSE(has_event("closed"_key));
  EXPECT_EQ(tab_count(), 1u); // window was not torn down

  auto evts = events_of("on_confirm_close"_key);
  ASSERT_EQ(evts.size(), 1u);
  auto* paths_f = evts[0].payload.findField<dynamic_ptr>("paths"_key);
  ASSERT_NE(paths_f, nullptr);
  ASSERT_TRUE(*paths_f);
  std::vector<std::string> paths;
  (*paths_f)->forEach([&](bison::key_t, const field& f) {
    if (f.is<std::string>())
      paths.push_back(f.as<std::string>());
  });
  ASSERT_EQ(paths.size(), 1u);
  EXPECT_EQ(paths[0], "a.txt");
}

TEST_F(NanoFilesTest, ConfirmCloseWithSaveTrueFlushesEveryFileThenCloses) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  simulate_editor_changed(0);
  simulate_window_closed();
  ASSERT_TRUE(wait_for_event("on_confirm_close"_key));

  confirm_close(/*save=*/true);

  ASSERT_TRUE(wait_for_event("closed"_key));
  auto closed_evts = events_of("on_file_closed"_key);
  ASSERT_EQ(closed_evts.size(), 1u);
  EXPECT_EQ(closed_evts[0].payload.as<std::string>("path"_key), "a.txt");
}

TEST_F(NanoFilesTest, ConfirmCloseWithSaveFalseSkipsOnlyTheDirtyFiles) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  simulate_editor_changed(0); // a.txt is dirty; b.txt stays clean
  simulate_window_closed();
  ASSERT_TRUE(wait_for_event("on_confirm_close"_key));

  confirm_close(/*save=*/false);

  ASSERT_TRUE(wait_for_event("closed"_key));
  auto closed_evts = events_of("on_file_closed"_key);
  ASSERT_EQ(closed_evts.size(), 1u);
  EXPECT_EQ(closed_evts[0].payload.as<std::string>("path"_key), "b.txt");
}

TEST_F(NanoFilesTest, OpenButtonClickedEmitsOnRequestOpen) {
  simulate_btn_click("btn_open");
  EXPECT_TRUE(wait_for_event("on_request_open"_key));
}

TEST_F(NanoFilesTest, NewButtonClickedEmitsOnRequestNew) {
  simulate_btn_click("btn_new");
  EXPECT_TRUE(wait_for_event("on_request_new"_key));
}

TEST_F(NanoFilesTest, WindowClosedFlushesEveryOpenFileThenCloses) {
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
