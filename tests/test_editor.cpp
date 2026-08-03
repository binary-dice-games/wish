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

constexpr const char* kValidUiTwoButtons = R"({
  "type": "Window",
  "title": "Mock",
  "children": {
    "main": {
      "type": "VerticalLayout",
      "children": {
        "ok": { "type": "Button", "label": "OK" },
        "cancel": { "type": "Button", "label": "Cancel" }
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
// "__editor_", no dot, and not the "_mock"/"_help" suffixes used for the
// preview and Help-window roots respectively).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__editor_", 0) == 0 && k.find('.') == std::string::npos && k.find("_mock") == std::string::npos &&
        k.find("_help") == std::string::npos)
      return k;
  }
  return {};
}

// Helper: find the Help window's own root key (starts with "__editor_", no
// dot, ends with "_help").
static std::string find_help_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__editor_", 0) == 0 && k.find('.') == std::string::npos && k.size() > 5 &&
        k.compare(k.size() - 5, 5, "_help") == 0)
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
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.editor_row.source"));
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

TEST_F(EditorWindowTest, HelpWindowIsRegisteredAsSeparateTopLevelWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  std::string help_root = root + "_help";

  ASSERT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{help_root}));
  auto& help_obj = srv_->last_session->ui_objects.at(help_root);
  EXPECT_EQ(help_obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
  EXPECT_EQ(help_obj->as<std::string>("title"_key), "Help");
  // Per the answer to "should the Help window be closable" -- not closable,
  // so there's no way to lose it with no reopen affordance in the editor.
  EXPECT_FALSE(help_obj->get_as<bool>("closable"_key, false));
  EXPECT_NE(help_root, root); // distinct root from the main chrome window
}

TEST_F(EditorWindowTest, HelpWindowTreeContainsFieldsTable) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  std::string help_root = root + "_help";
  EXPECT_TRUE(srv_->last_session->ui_objects.count(help_root + ".vbox.class_name"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(help_root + ".vbox.class_desc"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(help_root + ".vbox.fields"));
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
    help_root_ = root_ + "_help";

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

  dynamic set_source(const std::string& path, const std::string& display_path = "") {
    dynamic args;
    args["path"_key] = path;
    if (!display_path.empty())
      args["display_path"_key] = display_path;
    return proxy_->call("set_source"_key, std::move(args)).get();
  }

  dynamic mark_saved() {
    return proxy_->call("mark_saved"_key, dynamic{}).get();
  }

  bison::key_t chrome_widget_id(const std::string& dot_suffix) const {
    auto it = srv_->last_session->ui_objects.find(dot_suffix.empty() ? root_ : root_ + "." + dot_suffix);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<bison::key_t>("__wish_id"_key);
  }

  void simulate_chrome_event(bison::key_t widget_id, bison::key_t event, const dynamic& payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(widget_id, event, payload);
  }

  std::string path_label_text() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.path_label");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<std::string>("text"_key);
  }

  bool confirm_panel_visible() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.confirm");
    if (it == srv_->last_session->ui_objects.end())
      return false;
    return it->second->as<bool>("visible"_key);
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

  void simulate_mock_event(bison::key_t widget_id, bison::key_t event, const dynamic& payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{mock_root_});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(widget_id, event, payload);
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

  // Computes (line, column) for the first occurrence of `needle` in
  // `content` and simulates a "cursor_moved" event there, same idiom as the
  // pre-existing highlight tests below.
  void move_cursor_to(const std::string& content, const std::string& needle) {
    size_t offset = content.find(needle);
    ASSERT_NE(offset, std::string::npos);
    int32_t line = 0, col = 0;
    for (size_t i = 0; i < offset; ++i) {
      if (content[i] == '\n') {
        ++line;
        col = 0;
      } else {
        ++col;
      }
    }
    dynamic payload;
    payload["line"_key] = line;
    payload["column"_key] = col;
    simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "cursor_moved"_key, payload);
  }

  bool help_window_registered() const {
    return srv_->last_session->top_level_objects.count(bison::key_t{help_root_}) != 0
        && srv_->last_session->ui_objects.count(help_root_) != 0;
  }

  std::string help_class_name_text() const {
    auto it = srv_->last_session->ui_objects.find(help_root_ + ".vbox.class_name");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<std::string>("text"_key);
  }

  std::string help_class_desc_text() const {
    auto it = srv_->last_session->ui_objects.find(help_root_ + ".vbox.class_desc");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<std::string>("text"_key);
  }

  // First cell's `text` (the field-name column) of every TableRow currently
  // in the Help window's field table, in row order.
  std::vector<std::string> help_row_field_names() const {
    std::vector<std::string> names;
    auto it = srv_->last_session->ui_objects.find(help_root_ + ".vbox.fields");
    if (it == srv_->last_session->ui_objects.end())
      return names;
    it->second->for_each_child_ordered([&](bison::key_t, wish::ui_element& row) {
      if (row.as<bison::key_t>(dynamic::CLASS) != "TableRow"_key)
        return;
      bool first = true;
      row.for_each_child_ordered([&](bison::key_t, wish::ui_element& cell) {
        if (first) {
          names.push_back(cell.as<std::string>("text"_key));
          first = false;
        }
      });
    });
    return names;
  }

  // Each TableRow's own `__wish_id` currently in the Help window's field
  // table, in row order -- lets a test assert row identity is (or isn't)
  // stable across two update_help_panel() calls.
  std::vector<bison::key_t> help_row_ids() const {
    std::vector<bison::key_t> ids;
    auto it = srv_->last_session->ui_objects.find(help_root_ + ".vbox.fields");
    if (it == srv_->last_session->ui_objects.end())
      return ids;
    it->second->for_each_child_ordered([&](bison::key_t, wish::ui_element& row) {
      if (row.as<bison::key_t>(dynamic::CLASS) == "TableRow"_key)
        ids.push_back(row.as<bison::key_t>("__wish_id"_key));
    });
    return ids;
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  std::string mock_root_;
  std::string help_root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(EditorSourceTest, ReparseReusesRootWishId) {
  seed_sandbox_file("v1.json", kValidUi);
  set_source("v1.json");
  ASSERT_TRUE(mock_registered());
  auto first_root_id = srv_->last_session->ui_objects.at(mock_root_)->as<bison::key_t>("__wish_id"_key);

  // A second, still-valid reparse must keep the same root id -- ImGui keys a
  // window's position/size/focus state off it (see mock_window_id_'s doc
  // comment), so a changed id would reset the user's dragged position/size
  // and steal focus back to the preview window on every edit.
  seed_sandbox_file("v2.json", kValidUi);
  set_source("v2.json");
  ASSERT_TRUE(mock_registered());
  auto second_root_id = srv_->last_session->ui_objects.at(mock_root_)->as<bison::key_t>("__wish_id"_key);

  EXPECT_EQ(first_root_id.id, second_root_id.id);
}

TEST_F(EditorSourceTest, ReparseWithChangedTitleStillReusesRootWishId) {
  // Same guarantee as ReparseReusesRootWishId, but across a reparse that
  // also changes the root Window's own "title" field -- this is the
  // scenario with_id()'s "###" fix (imgui_ui_renderer.cpp) specifically
  // targets: with the old "##" convention, ImGui's window ID hash included
  // the visible title text, so editing the title alone (wish_id unchanged)
  // still produced a "new" ImGui window, resetting position/size and
  // stealing focus. wish_id staying stable here is the precondition that
  // fix relies on; with_id() itself is exercised by test_imgui_renderer.cpp.
  constexpr const char* kTitledV1 = R"({"type": "Window", "title": "First Title",
    "children": {"main": {"type": "VerticalLayout", "children": {"ok": {"type": "Button", "label": "OK"}}}}})";
  constexpr const char* kTitledV2 = R"({"type": "Window", "title": "Second Title",
    "children": {"main": {"type": "VerticalLayout", "children": {"ok": {"type": "Button", "label": "OK"}}}}})";

  seed_sandbox_file("t1.json", kTitledV1);
  set_source("t1.json");
  ASSERT_TRUE(mock_registered());
  auto first_root_id = srv_->last_session->ui_objects.at(mock_root_)->as<bison::key_t>("__wish_id"_key);

  seed_sandbox_file("t2.json", kTitledV2);
  set_source("t2.json");
  ASSERT_TRUE(mock_registered());
  auto second_root_id = srv_->last_session->ui_objects.at(mock_root_)->as<bison::key_t>("__wish_id"_key);

  EXPECT_EQ(first_root_id.id, second_root_id.id);
  EXPECT_EQ(srv_->last_session->ui_objects.at(mock_root_)->as<std::string>("title"_key), "Second Title");
}

TEST_F(EditorSourceTest, EventLogCapsAtMaxRowsAndEvictsOldest) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json");
  auto ok_id = mock_widget_id("main.ok");
  ASSERT_NE(ok_id.id, 0u);

  constexpr int kMax = 200; // must match editor::kMaxLogRows
  for (int i = 0; i < kMax + 10; ++i)
    simulate_mock_event(ok_id, "clicked"_key);

  auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.log");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());
  auto* cf = it->second->findField("children"_key);
  ASSERT_TRUE(cf && cf->is<dynamic_ptr>() && cf->as<dynamic_ptr>());
  auto& children = *cf->as<dynamic_ptr>();

  // Child keys 0..(kMax+10-1) were assigned in append order; only the most
  // recent kMax rows should still be present.
  size_t row_count = 0;
  for (size_t k = 0; k < static_cast<size_t>(kMax + 10); ++k) {
    try {
      auto& f = children.at(k);
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
        ++row_count;
    } catch (const std::exception&) {
      // Evicted -- expected for the oldest keys.
    }
  }
  EXPECT_EQ(row_count, static_cast<size_t>(kMax));

  // The very first row (child_key 0) should be gone: erase() leaves the slot
  // present but empty rather than removing the key outright, so check that
  // it's no longer a live row instead of expecting at() to throw.
  auto& evicted = children.at(size_t{0});
  EXPECT_FALSE(evicted.is<dynamic_ptr>() && evicted.as<dynamic_ptr>()) << "oldest row should have been evicted";
}

TEST_F(EditorSourceTest, MockWidgetEventPayloadIsLogged) {
  constexpr const char* kSliderUi = R"({
    "type": "Window",
    "children": {
      "volume": { "type": "SliderInt", "label": "Volume", "value": 50, "min": 0, "max": 100 }
    }
  })";
  seed_sandbox_file("ui.json", kSliderUi);
  set_source("ui.json");
  ASSERT_TRUE(mock_registered());

  auto volume_id = mock_widget_id("volume");
  ASSERT_NE(volume_id.id, 0u);
  dynamic payload;
  payload["value"_key] = int32_t{75};
  simulate_mock_event(volume_id, "changed"_key, payload);

  auto row = log_row_text(0);
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(*row, "volume changed {value=75}");
}

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

TEST_F(EditorSourceTest, CursorMovedHighlightsEnclosingMockWidget) {
  std::string content = kValidUiTwoButtons;
  seed_sandbox_file("ui.json", content);
  set_source("ui.json");
  ASSERT_TRUE(mock_registered());

  auto ok_id = mock_widget_id("main.ok");
  auto cancel_id = mock_widget_id("main.cancel");
  ASSERT_NE(ok_id.id, 0u);
  ASSERT_NE(cancel_id.id, 0u);

  auto highlighted = [&](const std::string& mock_suffix) {
    return srv_->last_session->ui_objects.at(mock_root_ + "." + mock_suffix)
        ->get_as<bool>("__wish_highlight__"_key, false);
  };

  auto move_cursor_to = [&](const std::string& needle) {
    size_t offset = content.find(needle);
    ASSERT_NE(offset, std::string::npos);
    int32_t line = 0, col = 0;
    for (size_t i = 0; i < offset; ++i) {
      if (content[i] == '\n') {
        ++line;
        col = 0;
      } else {
        ++col;
      }
    }
    dynamic payload;
    payload["line"_key] = line;
    payload["column"_key] = col;
    simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "cursor_moved"_key, payload);
  };

  move_cursor_to("\"label\": \"OK\"");
  EXPECT_TRUE(highlighted("main.ok"));
  EXPECT_FALSE(highlighted("main.cancel"));

  move_cursor_to("\"label\": \"Cancel\"");
  EXPECT_FALSE(highlighted("main.ok"));
  EXPECT_TRUE(highlighted("main.cancel"));
}

TEST_F(EditorSourceTest, HighlightSurvivesReparseOfSamePath) {
  std::string content = kValidUiTwoButtons;
  seed_sandbox_file("ui.json", content);
  set_source("ui.json");
  ASSERT_TRUE(mock_registered());

  size_t offset = content.find("\"label\": \"OK\"");
  ASSERT_NE(offset, std::string::npos);
  int32_t line = 0, col = 0;
  for (size_t i = 0; i < offset; ++i) {
    if (content[i] == '\n') {
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
  dynamic payload;
  payload["line"_key] = line;
  payload["column"_key] = col;
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "cursor_moved"_key, payload);
  ASSERT_TRUE(
      srv_->last_session->ui_objects.at(mock_root_ + ".main.ok")->get_as<bool>("__wish_highlight__"_key, false));

  // An in-editor edit (via a "changed" event, mirroring InEditorChangeMarksModified)
  // tears down and rebuilds the whole preview subtree -- the highlight must be
  // reapplied to the same dot-path in the freshly-rebuilt tree, not lost.
  seed_sandbox_file("ui.json", content);
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "changed"_key);
  ASSERT_TRUE(mock_registered());

  EXPECT_TRUE(
      srv_->last_session->ui_objects.at(mock_root_ + ".main.ok")->get_as<bool>("__wish_highlight__"_key, false));
}

TEST_F(EditorSourceTest, CursorMovedInsideButtonPopulatesHelpFieldTable) {
  std::string content = kValidUiTwoButtons;
  seed_sandbox_file("ui.json", content);
  set_source("ui.json");
  ASSERT_TRUE(help_window_registered());

  move_cursor_to(content, "\"label\": \"OK\"");

  EXPECT_EQ(help_class_name_text(), "Button");
  EXPECT_FALSE(help_class_desc_text().empty());
  auto names = help_row_field_names();
  EXPECT_FALSE(names.empty());
  EXPECT_NE(std::find(names.begin(), names.end(), "Label"), names.end());
}

TEST_F(EditorSourceTest, CursorMovedOutsideAnyElementClearsHelpPanel) {
  std::string content = kValidUiTwoButtons;
  seed_sandbox_file("ui.json", content);
  set_source("ui.json");
  ASSERT_TRUE(help_window_registered());

  move_cursor_to(content, "\"label\": \"OK\"");
  ASSERT_FALSE(help_row_field_names().empty());

  // Right after the very last character (the final closing brace): no
  // enclosing element at all (see scan_cursor_context()'s
  // RightAfterClosingBraceIsUnknown case). Computed directly from
  // content.size() rather than move_cursor_to()'s needle search, since a
  // single "}" character occurs many times earlier in the file too.
  int32_t line = 0, col = 0;
  for (char c : content) {
    if (c == '\n') {
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
  dynamic payload;
  payload["line"_key] = line;
  payload["column"_key] = col;
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "cursor_moved"_key, payload);

  EXPECT_TRUE(help_class_name_text().empty());
  EXPECT_TRUE(help_row_field_names().empty());
}

TEST_F(EditorSourceTest, CursorMovingWithinSameEnclosingTypeDoesNotReallocateRows) {
  // "OK" and "Cancel" are both Buttons -- moving between them should skip
  // rebuilding the field table entirely (same enclosing_type), so the row
  // ids stay exactly the same instead of being torn down and recreated.
  std::string content = kValidUiTwoButtons;
  seed_sandbox_file("ui.json", content);
  set_source("ui.json");
  ASSERT_TRUE(help_window_registered());

  move_cursor_to(content, "\"label\": \"OK\"");
  auto first_ids = help_row_ids();
  ASSERT_FALSE(first_ids.empty());

  move_cursor_to(content, "\"label\": \"Cancel\"");
  auto second_ids = help_row_ids();

  EXPECT_EQ(first_ids, second_ids);
}

TEST_F(EditorSourceTest, SourceEditorSavedEmitsOnSourceSaved) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json");

  auto source_id = srv_->last_session->ui_objects.at(root_ + ".vbox.editor_row.source")->as<bison::key_t>("__wish_id"_key);
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

// ── Filename label / unsaved-changes confirmation ─────────────────────────────

TEST_F(EditorSourceTest, SetSourceUpdatesFilenameLabel) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json", "/local/path/ui.json");
  EXPECT_EQ(path_label_text(), "Filename: /local/path/ui.json");
}

TEST_F(EditorSourceTest, InEditorChangeMarksModified) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json", "/local/path/ui.json");

  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "changed"_key);

  EXPECT_EQ(path_label_text(), "Filename: /local/path/ui.json [MODIFIED]");
}

TEST_F(EditorSourceTest, ClosingWithUnsavedChangesShowsConfirmInsteadOfClosing) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json", "/local/path/ui.json");
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "changed"_key);
  ASSERT_EQ(path_label_text(), "Filename: /local/path/ui.json [MODIFIED]");

  simulate_chrome_event(chrome_widget_id(""), "closed"_key);

  EXPECT_TRUE(confirm_panel_visible());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_)); // not torn down
  EXPECT_TRUE(mock_registered()); // preview untouched
  EXPECT_FALSE(has_event("closed"_key));
}

TEST_F(EditorSourceTest, ConfirmCancelHidesPanelAndKeepsEditorOpen) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json", "/local/path/ui.json");
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "changed"_key);
  simulate_chrome_event(chrome_widget_id(""), "closed"_key);
  ASSERT_TRUE(confirm_panel_visible());

  simulate_chrome_event(chrome_widget_id("vbox.confirm.confirm_row.confirm_cancel"), "clicked"_key);

  EXPECT_FALSE(confirm_panel_visible());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_FALSE(has_event("closed"_key));
}

TEST_F(EditorSourceTest, ConfirmDiscardClosesDespiteUnsavedChanges) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json", "/local/path/ui.json");
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "changed"_key);
  simulate_chrome_event(chrome_widget_id(""), "closed"_key);
  ASSERT_TRUE(confirm_panel_visible());

  simulate_chrome_event(chrome_widget_id("vbox.confirm.confirm_row.confirm_discard"), "clicked"_key);

  EXPECT_TRUE(wait_for_event("closed"_key));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_));
  EXPECT_FALSE(has_event("on_source_saved"_key));
}

TEST_F(EditorSourceTest, ConfirmSaveEmitsOnSourceSavedThenMarkSavedCompletesClose) {
  seed_sandbox_file("ui.json", kValidUi);
  set_source("ui.json", "/local/path/ui.json");
  simulate_chrome_event(chrome_widget_id("vbox.editor_row.source"), "changed"_key);
  simulate_chrome_event(chrome_widget_id(""), "closed"_key);
  ASSERT_TRUE(confirm_panel_visible());

  simulate_chrome_event(chrome_widget_id("vbox.confirm.confirm_row.confirm_save"), "clicked"_key);

  ASSERT_TRUE(wait_for_event("on_source_saved"_key));
  EXPECT_FALSE(has_event("closed"_key)); // waiting on mark_saved()
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));

  mark_saved();

  EXPECT_TRUE(wait_for_event("closed"_key));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_));
}
