// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <algorithm>
#include <functional>
#include <chrono>
#include <deque>
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
  /// Every event the session emits (see session_event_recorder.hpp).
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

// Helper: find the root key for the internal form tree -- "__nano_<N>"
// exactly (the Toolbar), not a child path ("__nano_0.toolbar...") nor a
// secondary panel root ("__nano_0_search", "__nano_0_doc_0").
static std::string find_form_root(const wish::name_map& objects) {
  static const std::string prefix = "__nano_";
  for (const auto& [k, _] : objects) {
    if (k.size() > prefix.size() && k.rfind(prefix, 0) == 0 &&
        std::all_of(k.begin() + prefix.size(), k.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
      return k;
  }
  return {};
}

// ── Panels ────────────────────────────────────────────────────────────────────

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

TEST_F(NanoWindowTest, ToolbarHasOpenNewSaveFindButtons) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  for (const char* btn : {"btn_open", "btn_new", "btn_save", "btn_find"})
    EXPECT_TRUE(objs.count(root + ".toolbar." + btn)) << btn;
  EXPECT_EQ(objs.at(root + ".toolbar.current_label")->as<std::string>("text"_key), "No file open");
}

TEST_F(NanoWindowTest, SearchPanelHasNotepadPlusPlusStyleControls) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  const std::string s = root + "_search.vbox.";
  for (const std::string path :
       {"top.options.find_row.find_input", "top.options.checks.chk_whole_word", "top.options.checks.chk_match_case",
           "top.options.mode_row.radio_normal", "top.options.mode_row.radio_extended",
           "top.options.mode_row.radio_regex", "top.buttons.btn_find_current", "top.buttons.btn_find_all",
           "top.buttons.count_row.btn_count", "top.buttons.count_row.btn_clear", "summary", "results_table"})
    EXPECT_TRUE(objs.count(s + path)) << path;
  EXPECT_TRUE(objs.at(s + "top.options.mode_row.radio_normal")->as<bool>("active"_key));
}

// The Toolbar and Search panels are each their own top-level Window,
// addressable by "__path__" so the dock layout can name them.
TEST_F(NanoWindowTest, ToolbarAndSearchAreTopLevelWindows) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& s = *srv_->last_session;
  for (const std::string key : {root, root + "_search"}) {
    auto it = s.top_level_objects.find(bison::key_t{key});
    ASSERT_NE(it, s.top_level_objects.end()) << key;
    EXPECT_EQ(it->second->as<bison::key_t>(dynamic::CLASS), "Window"_key) << key;
    EXPECT_EQ(it->second->as<std::string>("__path__"_key), key) << key;
  }
  EXPECT_EQ(s.ui_objects.at(root)->as<std::string>("title"_key), "Toolbar");
  EXPECT_EQ(s.ui_objects.at(root + "_search")->as<std::string>("title"_key), "Search");
}

// The panels are seeded into a nested DockSpaceViewport ("nano_dock"): the
// Toolbar and Search in their own areas, plus one empty area -- the central
// node file windows dock into.
TEST_F(NanoWindowTest, RegistersDefaultDockLayoutWithEmptyDocumentsArea) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());

  wish::ui_element_ptr viewport;
  for (const auto& [k, obj] : srv_->last_session->top_level_objects)
    if (obj->as<bison::key_t>(dynamic::CLASS) == "DockSpaceViewport"_key)
      viewport = obj;
  ASSERT_TRUE(viewport);
  EXPECT_EQ(viewport->as<std::string>("id"_key), "nano_dock");
  EXPECT_EQ(viewport->as<std::string>("title"_key), "Nano");

  std::vector<std::string> windows;
  std::function<void(const dynamic&)> walk = [&](const dynamic& node) {
    if (node.as<bison::key_t>(dynamic::CLASS) == "DockArea"_key)
      windows.push_back(node.as<std::string>("windows"_key));
    if (auto* cf = node.findField<dynamic_ptr>("children"_key); cf && *cf)
      (*cf)->forEach([&](bison::key_t, const field& f) {
        if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
          walk(*f.as<dynamic_ptr>());
      });
  };
  walk(*viewport);
  std::sort(windows.begin(), windows.end());
  std::vector<std::string> expected{"", root, root + "_search"};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(windows, expected);
}

// ── open_file / file windows ──────────────────────────────────────────────────

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

    events_since_ = srv_->events->mark();
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

  // File windows are keyed "<root>_doc_<n>", n counting opens from 0.
  std::string doc_root(size_t n) const { return root_ + "_doc_" + std::to_string(n); }

  size_t doc_count() const {
    size_t n = 0;
    const std::string prefix = root_ + "_doc_";
    for (const auto& [k, _] : srv_->last_session->ui_objects)
      if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos &&
          srv_->last_session->top_level_objects.count(bison::key_t{k}))
        ++n;
    return n;
  }

  dynamic_ptr obj(const std::string& path) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(path);
    return it != objs.end() ? dynamic_ptr{it->second} : dynamic_ptr{nullptr};
  }

  dynamic_ptr doc_window(size_t n) const { return obj(doc_root(n)); }
  dynamic_ptr editor_at(size_t n) const { return obj(doc_root(n) + ".vbox.editor"); }
  dynamic_ptr lang_combo_at(size_t n) const { return obj(doc_root(n) + ".vbox.lang"); }
  std::string title_at(size_t n) const { return doc_window(n)->as<std::string>("title"_key); }
  std::string search_path(const std::string& rel) const { return root_ + "_search.vbox." + rel; }

  bison::key_t id_of(const dynamic_ptr& p) const { return p->as<bison::key_t>("__wish_id"_key); }

  void fire(const dynamic_ptr& widget, bison::key_t event, const dynamic& payload = {}) {
    ASSERT_TRUE(widget);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id_of(widget), event, payload);
  }

  bool has_event(bison::key_t name) const {
    for (auto& e : events())
      if (e.name == name)
        return true;
    return false;
  }

  // form::emit() defers delivery to the render loop's next frame, so spin
  // briefly for an event (same idiom as test_integration.cpp).
  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto t0 = std::chrono::steady_clock::now();
    while (!has_event(name) && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has_event(name);
  }

  std::vector<CapturedEvent> events_of(bison::key_t name) const {
    std::vector<CapturedEvent> result;
    for (auto& e : events())
      if (e.name == name)
        result.push_back(e);
    return result;
  }

  void simulate_toolbar_closed() { fire(obj(root_), "closed"_key); }
  void simulate_btn_click(const std::string& btn_key) { fire(obj(root_ + ".toolbar." + btn_key), "clicked"_key); }
  void simulate_editor_changed(size_t n) { fire(editor_at(n), "changed"_key); }

  void simulate_lang_combo_changed(size_t n, int32_t value) {
    dynamic payload;
    payload["value"_key] = value;
    fire(lang_combo_at(n), "changed"_key, payload);
  }

  dynamic confirm_close(bool save) {
    dynamic args;
    args["save"_key] = save;
    return proxy_->call("confirm_close"_key, std::move(args)).get();
  }

  int32_t find(const std::string& text, const std::string& mode = "normal", const std::string& scope = "current",
      bool match_case = false, bool whole_word = false) {
    dynamic args;
    args["text"_key] = text;
    args["mode"_key] = mode;
    args["scope"_key] = scope;
    args["match_case"_key] = match_case;
    args["whole_word"_key] = whole_word;
    return proxy_->call("find"_key, std::move(args)).get().as<int32_t>("hits"_key);
  }

  // The results table's rows as "<File>|<Line>|<Text>", in display order.
  std::vector<std::string> result_rows() const {
    std::vector<std::pair<int32_t, std::string>> rows;
    auto table = obj(search_path("results_table"));
    auto* cf = table->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      auto& row = *f.as<dynamic_ptr>();
      if (row.as<bison::key_t>(dynamic::CLASS) != "TableRow"_key)
        return;
      std::vector<std::string> cells;
      row.as<dynamic_ptr>("children"_key)->forEach([&](bison::key_t, const field& c) {
        if (c.is<dynamic_ptr>() && c.as<dynamic_ptr>())
          cells.push_back(c.as<dynamic_ptr>()->as<std::string>("text"_key));
      });
      rows.emplace_back(row.as<int32_t>("order"_key), cells.at(0) + "|" + cells.at(1) + "|" + cells.at(2));
    });
    std::sort(rows.begin(), rows.end());
    std::vector<std::string> out;
    for (auto& r : rows)
      out.push_back(r.second);
    return out;
  }

  std::string summary() const { return obj(search_path("summary"))->as<std::string>("text"_key); }

  void click_result(int32_t row) {
    dynamic payload;
    payload["index"_key] = row;
    fire(obj(search_path("results_table")), "row_selected"_key, payload);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  // The events the session emitted since SetUp(), refreshed from its
  // thread-safe recorder (session_event_recorder.hpp) on every call. Append-
  // only std::deque, so references/pointers into it stay valid.
  const std::deque<CapturedEvent>& events() const {
    for (auto& e : srv_->events->snapshot(events_since_)) {
      ++events_since_;
      events_cache_.push_back({e.name, std::move(e.payload)});
    }
    return events_cache_;
  }
  mutable size_t events_since_{0};
  mutable std::deque<CapturedEvent> events_cache_;
};

TEST_F(NanoFilesTest, OpenFileCreatesItsOwnWindowTargetingTheDocumentsArea) {
  seed_sandbox_file("hello.py", "print('hi')");
  open_file("hello.py");

  ASSERT_EQ(doc_count(), 1u);
  auto win = doc_window(0);
  ASSERT_TRUE(win);
  EXPECT_EQ(win->as<bison::key_t>(dynamic::CLASS), "Window"_key);
  EXPECT_EQ(win->as<std::string>("title"_key), "hello.py");
  EXPECT_EQ(win->as<std::string>("dock_target"_key), "nano_dock");
  EXPECT_EQ(win->as<std::string>("__path__"_key), doc_root(0));
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
  EXPECT_EQ(title_at(0), "a.txt"); // a display-only change, not a content edit
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

TEST_F(NanoFilesTest, OpenFileTwiceBringsTheExistingWindowToTheFront) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt");
  int32_t before = doc_window(0)->get_as<int32_t>("focus_request"_key, 0);
  open_file("a.txt");
  EXPECT_EQ(doc_count(), 1u);
  EXPECT_EQ(doc_window(0)->get_as<int32_t>("focus_request"_key, 0), before + 1);
}

TEST_F(NanoFilesTest, OpenSecondFileAddsSecondWindow) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.md", "two");
  open_file("a.txt");
  open_file("b.md");
  EXPECT_EQ(doc_count(), 2u);
  EXPECT_EQ(editor_at(1)->as<std::string>("language"_key), "markdown");
}

TEST_F(NanoFilesTest, PathTraversalEmitsOnErrorAndNoWindow) {
  open_file("../escape.txt");
  EXPECT_EQ(doc_count(), 0u);
  EXPECT_TRUE(wait_for_event("on_error"_key));
  EXPECT_FALSE(has_event("on_file_opened"_key));
}

TEST_F(NanoFilesTest, FileWindowClosedEmitsOnFileClosedAndRemovesIt) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt");
  ASSERT_EQ(doc_count(), 1u);

  fire(doc_window(0), "closed"_key);

  EXPECT_EQ(doc_count(), 0u);
  EXPECT_FALSE(obj(doc_root(0)));
  EXPECT_TRUE(obj(root_)); // the editor itself stays open
  ASSERT_TRUE(wait_for_event("on_file_closed"_key));
  auto evts = events_of("on_file_closed"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "a.txt");
  EXPECT_FALSE(has_event("closed"_key));
}

TEST_F(NanoFilesTest, EditingMarksWindowTitleDirtyWithAsteriskSuffix) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  EXPECT_EQ(title_at(0), "a.txt");

  simulate_editor_changed(0);
  EXPECT_EQ(title_at(0), "a.txt *");
}

TEST_F(NanoFilesTest, MostRecentlyOpenedFileIsCurrent) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  EXPECT_EQ(obj(root_ + ".toolbar.current_label")->as<std::string>("text"_key), "Current: b.txt");
}

TEST_F(NanoFilesTest, SaveClickedSavesOnlyTheCurrentFile) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  simulate_editor_changed(0);
  simulate_editor_changed(1);

  simulate_btn_click("btn_save");

  ASSERT_TRUE(wait_for_event("on_file_saved"_key));
  auto evts = events_of("on_file_saved"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "b.txt");
  EXPECT_EQ(title_at(0), "a.txt *");
  EXPECT_EQ(title_at(1), "b.txt");
}

TEST_F(NanoFilesTest, FocusingAFileWindowMakesItCurrent) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");

  fire(doc_window(0), "focused"_key);
  simulate_editor_changed(0);
  simulate_btn_click("btn_save");

  ASSERT_TRUE(wait_for_event("on_file_saved"_key));
  auto evts = events_of("on_file_saved"_key);
  ASSERT_EQ(evts.size(), 1u);
  EXPECT_EQ(evts[0].payload.as<std::string>("path"_key), "a.txt");
  EXPECT_EQ(obj(root_ + ".toolbar.current_label")->as<std::string>("text"_key), "Current: a.txt");
}

TEST_F(NanoFilesTest, ClosingTheCurrentFileMakesTheLastOpenedRemainingOneCurrent) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  fire(doc_window(1), "closed"_key);
  EXPECT_EQ(obj(root_ + ".toolbar.current_label")->as<std::string>("text"_key), "Current: a.txt");
  fire(doc_window(0), "closed"_key);
  EXPECT_EQ(obj(root_ + ".toolbar.current_label")->as<std::string>("text"_key), "No file open");
}

TEST_F(NanoFilesTest, CtrlSInAnEditorSavesThatFile) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  simulate_editor_changed(0);
  fire(editor_at(0), "saved"_key);
  ASSERT_TRUE(wait_for_event("on_file_saved"_key));
  EXPECT_EQ(title_at(0), "a.txt");
}

TEST_F(NanoFilesTest, ToolbarClosedWithDirtyFileAsksForConfirmationInstead) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  simulate_editor_changed(0);

  simulate_toolbar_closed();

  ASSERT_TRUE(wait_for_event("on_confirm_close"_key));
  EXPECT_FALSE(has_event("closed"_key));
  EXPECT_EQ(doc_count(), 1u); // nothing was torn down

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

TEST_F(NanoFilesTest, ConfirmCloseWithSaveTrueFlushesEveryFileThenClosesEveryPanel) {
  seed_sandbox_file("a.txt", "one");
  open_file("a.txt", "a.txt");
  simulate_editor_changed(0);
  simulate_toolbar_closed();
  ASSERT_TRUE(wait_for_event("on_confirm_close"_key));

  confirm_close(/*save=*/true);

  ASSERT_TRUE(wait_for_event("closed"_key));
  auto closed_evts = events_of("on_file_closed"_key);
  ASSERT_EQ(closed_evts.size(), 1u);
  EXPECT_EQ(closed_evts[0].payload.as<std::string>("path"_key), "a.txt");
  for (const std::string key : {root_, root_ + "_search", doc_root(0)})
    EXPECT_FALSE(obj(key)) << key;
}

TEST_F(NanoFilesTest, ConfirmCloseWithSaveFalseSkipsOnlyTheDirtyFiles) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  simulate_editor_changed(0); // a.txt is dirty; b.txt stays clean
  simulate_toolbar_closed();
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

TEST_F(NanoFilesTest, ToolbarClosedFlushesEveryOpenFileThenCloses) {
  seed_sandbox_file("a.txt", "one");
  seed_sandbox_file("b.txt", "two");
  open_file("a.txt");
  open_file("b.txt");

  simulate_toolbar_closed();

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
  for (size_t i = 0; i < events().size(); ++i) {
    if (events()[i].name == "on_file_closed"_key)
      last_file_closed_idx = i;
    if (events()[i].name == "closed"_key)
      closed_idx = i;
  }
  EXPECT_GT(closed_idx, last_file_closed_idx);
}

// ── Search panel ──────────────────────────────────────────────────────────────

TEST_F(NanoFilesTest, SearchPanelCloseHidesItAndFindShowsItAgain) {
  auto panel = obj(root_ + "_search");
  fire(panel, "closed"_key);
  EXPECT_FALSE(panel->get_as<bool>("visible"_key, true));
  EXPECT_TRUE(obj(root_)); // closing Search does not close the editor
  EXPECT_FALSE(has_event("closed"_key));

  int32_t before = panel->get_as<int32_t>("focus_request"_key, 0);
  simulate_btn_click("btn_find");
  EXPECT_TRUE(panel->get_as<bool>("visible"_key, false));
  EXPECT_EQ(panel->get_as<int32_t>("focus_request"_key, 0), before + 1);
}

TEST_F(NanoFilesTest, FindAllInCurrentDocumentListsOneRowPerMatchingLine) {
  seed_sandbox_file("a.txt", "alpha beta\nno match\nbeta beta\n");
  open_file("a.txt", "a.txt");

  EXPECT_EQ(find("beta"), 3);
  EXPECT_EQ(result_rows(), (std::vector<std::string>{"a.txt|1|alpha beta", "a.txt|3|beta beta"}));
  EXPECT_EQ(summary(), "Search \"beta\" (3 hits in 1 file of 1 searched)");
}

TEST_F(NanoFilesTest, FindAllInCurrentDocumentOnlySearchesTheCurrentFile) {
  seed_sandbox_file("a.txt", "needle\n");
  seed_sandbox_file("b.txt", "needle\nneedle\n");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  fire(doc_window(0), "focused"_key);

  EXPECT_EQ(find("needle"), 1);
  EXPECT_EQ(result_rows(), (std::vector<std::string>{"a.txt|1|needle"}));
}

TEST_F(NanoFilesTest, FindAllInAllOpenedDocumentsSearchesEveryFile) {
  seed_sandbox_file("a.txt", "needle\n");
  seed_sandbox_file("b.txt", "nothing\n");
  seed_sandbox_file("c.txt", "x\nneedle\n");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  open_file("c.txt", "c.txt");

  EXPECT_EQ(find("needle", "normal", "all"), 2);
  EXPECT_EQ(result_rows(), (std::vector<std::string>{"a.txt|1|needle", "c.txt|2|needle"}));
  EXPECT_EQ(summary(), "Search \"needle\" (2 hits in 2 files of 3 searched)");
}

TEST_F(NanoFilesTest, FindAllButtonsUseThePanelsFields) {
  seed_sandbox_file("a.txt", "Foo\n");
  seed_sandbox_file("b.txt", "foo\n");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  auto input = obj(search_path("top.options.find_row.find_input"));
  (*input)["value"_key] = std::string{"foo"};
  (*obj(search_path("top.options.checks.chk_match_case")))["value"_key] = true;

  fire(obj(search_path("top.buttons.btn_find_all")), "clicked"_key);
  EXPECT_EQ(result_rows(), (std::vector<std::string>{"b.txt|1|foo"}));

  fire(obj(search_path("top.buttons.count_row.btn_clear")), "clicked"_key);
  EXPECT_TRUE(result_rows().empty());
  EXPECT_EQ(summary(), "");
}

TEST_F(NanoFilesTest, MatchCaseAndWholeWordNarrowTheMatches) {
  seed_sandbox_file("a.txt", "Cat cat concat\n");
  open_file("a.txt");
  EXPECT_EQ(find("cat"), 3);
  EXPECT_EQ(find("cat", "normal", "current", /*match_case=*/true), 2);
  EXPECT_EQ(find("cat", "normal", "current", /*match_case=*/false, /*whole_word=*/true), 2);
  EXPECT_EQ(find("cat", "normal", "current", /*match_case=*/true, /*whole_word=*/true), 1);
}

TEST_F(NanoFilesTest, RegularExpressionModeMatchesAPattern) {
  seed_sandbox_file("a.txt", "id=12\nid=x\nid=345\n");
  open_file("a.txt", "a.txt");
  EXPECT_EQ(find("id=[0-9]+", "regex"), 2);
  EXPECT_EQ(result_rows(), (std::vector<std::string>{"a.txt|1|id=12", "a.txt|3|id=345"}));
  EXPECT_TRUE(obj(search_path("top.options.mode_row.radio_regex"))->as<bool>("active"_key));
  EXPECT_FALSE(obj(search_path("top.options.mode_row.radio_normal"))->as<bool>("active"_key));
}

TEST_F(NanoFilesTest, InvalidRegularExpressionReportsTheError) {
  seed_sandbox_file("a.txt", "x\n");
  open_file("a.txt");
  EXPECT_EQ(find("(", "regex"), 0);
  EXPECT_EQ(summary().rfind("Invalid regular expression", 0), 0u) << summary();
}

TEST_F(NanoFilesTest, ExtendedModeUnderstandsEscapes) {
  seed_sandbox_file("a.txt", "a\tb\nab\n");
  open_file("a.txt", "a.txt");
  EXPECT_EQ(find("a\\tb", "extended"), 1);
  EXPECT_EQ(find("a\\tb"), 0); // Normal mode: a literal backslash-t
  EXPECT_EQ(find("a\\x09b", "extended"), 1);
}

TEST_F(NanoFilesTest, SearchSeesUnsavedEditsWrittenThroughToTheSandbox) {
  seed_sandbox_file("a.txt", "old\n");
  open_file("a.txt");
  seed_sandbox_file("a.txt", "new text\n"); // what the TextEditor writes on every edit
  EXPECT_EQ(find("new"), 1);
  EXPECT_EQ(find("old"), 0);
}

TEST_F(NanoFilesTest, CountReportsMatchesWithoutChangingTheResults) {
  seed_sandbox_file("a.txt", "x x x\n");
  open_file("a.txt", "a.txt");
  ASSERT_EQ(find("x"), 3);
  auto rows = result_rows();

  (*obj(search_path("top.options.find_row.find_input")))["value"_key] = std::string{"x x"};
  fire(obj(search_path("top.buttons.count_row.btn_count")), "clicked"_key);
  EXPECT_EQ(summary(), "Count: 1 match in \"a.txt\"");
  EXPECT_EQ(result_rows(), rows);
}

TEST_F(NanoFilesTest, FindWithNothingOpenSaysSo) {
  EXPECT_EQ(find("x"), 0);
  EXPECT_EQ(summary(), "No file open.");
}

TEST_F(NanoFilesTest, ClickingAResultSelectsTheMatchInItsFileAndBringsItToTheFront) {
  seed_sandbox_file("a.txt", "first\n\xC3\xA9t\xC3\xA9 needle\n"); // "été needle": é is 2 bytes
  seed_sandbox_file("b.txt", "other\n");
  open_file("a.txt", "a.txt");
  open_file("b.txt", "b.txt");
  ASSERT_EQ(find("needle", "normal", "all"), 1);
  int32_t focus_before = doc_window(0)->get_as<int32_t>("focus_request"_key, 0);

  click_result(0);

  auto editor = editor_at(0);
  EXPECT_EQ(editor->get_as<int32_t>("goto_line"_key, 0), 2);
  EXPECT_EQ(editor->get_as<int32_t>("goto_column"_key, -1), 4); // characters, not bytes
  EXPECT_EQ(editor->get_as<int32_t>("goto_length"_key, 0), 6);
  EXPECT_EQ(editor->get_as<int32_t>("goto_request"_key, 0), 1);
  EXPECT_EQ(doc_window(0)->get_as<int32_t>("focus_request"_key, 0), focus_before + 1);
  EXPECT_EQ(obj(root_ + ".toolbar.current_label")->as<std::string>("text"_key), "Current: a.txt");

  click_result(0); // the same row again re-issues the request
  EXPECT_EQ(editor->get_as<int32_t>("goto_request"_key, 0), 2);
}

TEST_F(NanoFilesTest, ClickingAResultForAClosedFileSaysSo) {
  seed_sandbox_file("a.txt", "needle\n");
  open_file("a.txt", "a.txt");
  ASSERT_EQ(find("needle"), 1);
  fire(doc_window(0), "closed"_key);
  click_result(0);
  EXPECT_EQ(summary(), "\"a.txt\" is no longer open.");
}
