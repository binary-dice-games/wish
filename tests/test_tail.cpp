// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class TailLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(TailLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Tail"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Tail"_key);
}

TEST_F(TailLocalTest, DefaultTitleIsTail) {
  auto obj = dynamic::instantiate("wish"_key, "Tail"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Tail");
}

// ── Session-capturing server, mirroring test_nano.cpp's fixture ────────────

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

static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__tail_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Tree-walking helpers for rows/tabs added outside the dot-path map ─────────
// Dynamically-added TableRow/TabItem elements (see tail.cpp's append_row/
// ensure_tag_tab) are never registered in ui_objects by dot-path -- only the
// statically JSON-declared chrome is -- so, mirroring test_nano.cpp's
// editor_at()/tab_id_at(), these walk each parent's own "children" field.
//
// NOTE: `dynamic_ptr`'s default constructor is `dynamic_ptr(key_t klass =
// 0U)` (see bison_common.hpp), which allocates a fresh, non-null `dynamic`
// object -- NOT a null pointer like a default-constructed std::shared_ptr.
// Every "not found" sentinel below must therefore be constructed explicitly
// as `dynamic_ptr{nullptr}` (binding to the inherited
// `shared_ptr(nullptr_t)` overload); relying on `dynamic_ptr{}`/a
// default-constructed local being falsy is a bug.

static size_t count_children_of_class(const dynamic_ptr& parent, bison::key_t klass) {
  size_t count = 0;
  if (!parent)
    return count;
  auto* cf = parent->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return count;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto child = f.as<dynamic_ptr>();
    if (child && child->as<bison::key_t>(dynamic::CLASS) == klass)
      ++count;
  });
  return count;
}

static dynamic_ptr find_tab_item(const dynamic_ptr& tab_bar, const std::string& label) {
  dynamic_ptr result{nullptr};
  if (!tab_bar)
    return result;
  auto* cf = tab_bar->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return result;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (result || !f.is<dynamic_ptr>())
      return;
    auto tab = f.as<dynamic_ptr>();
    if (tab && tab->as<bison::key_t>(dynamic::CLASS) == "TabItem"_key && tab->as<std::string>("label"_key) == label)
      result = tab;
  });
  return result;
}

static dynamic_ptr find_tab_table(const dynamic_ptr& tab_bar, const std::string& label) {
  dynamic_ptr result{nullptr};
  if (!tab_bar)
    return result;
  auto* cf = tab_bar->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return result;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (result || !f.is<dynamic_ptr>())
      return;
    auto tab = f.as<dynamic_ptr>();
    if (!tab || tab->as<bison::key_t>(dynamic::CLASS) != "TabItem"_key)
      return;
    if (tab->as<std::string>("label"_key) != label)
      return;
    auto* tcf = tab->findField<dynamic_ptr>("children"_key);
    if (tcf && *tcf && (*tcf)->size() > 0) {
      auto& table_f = (*tcf)->at(size_t{0});
      if (table_f.is<dynamic_ptr>())
        result = table_f.as<dynamic_ptr>();
    }
  });
  return result;
}

static dynamic_ptr first_row(const dynamic_ptr& table) {
  dynamic_ptr result{nullptr};
  if (!table)
    return result;
  auto* cf = table->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return result;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (result || !f.is<dynamic_ptr>())
      return;
    auto child = f.as<dynamic_ptr>();
    if (child && child->as<bison::key_t>(dynamic::CLASS) == "TableRow"_key)
      result = child;
  });
  return result;
}

// Rows are keyed 0, 1, 2, ... in insertion order (log_table_state::
// next_child_key), so indexing the children map directly (rather than
// forEach-ing to the Nth TableRow) recovers insertion order without relying
// on forEach's own iteration order. Only valid while every row ever added
// is still present -- once eviction (see evict_to_cap()) erases an early
// key, later indices no longer line up with visual row position (use
// ordered_rows() below instead for anything that evicts).
static dynamic_ptr row_at(const dynamic_ptr& table, size_t index) {
  dynamic_ptr result{nullptr};
  if (!table)
    return result;
  auto* cf = table->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf || (*cf)->size() <= index)
    return result;
  auto& f = (*cf)->at(index);
  if (!f.is<dynamic_ptr>())
    return result;
  return f.as<dynamic_ptr>();
}

// Every remaining TableRow child of @p table, oldest first, in the same
// order render_table()'s for_each_child_ordered() would visit them --
// i.e. via the "__children_order__" cache ui_element::refresh_children_
// order() builds (see ui_element.cpp), not raw children-map key order.
// Unlike row_at(), this stays correct across eviction: a gap left by an
// erased key is simply absent from the cache, so indices into the
// returned vector always match visual row position.
static std::vector<dynamic_ptr> ordered_rows(const dynamic_ptr& table) {
  std::vector<dynamic_ptr> result;
  if (!table)
    return result;
  auto* order_f = table->findField<dynamic_ptr>("__children_order__"_key);
  auto* children_f = table->findField<dynamic_ptr>("children"_key);
  if (!order_f || !*order_f || !children_f || !*children_f)
    return result;
  (*order_f)->forEach([&](bison::key_t, const field& f) {
    if (!f.is<int32_t>())
      return;
    bison::key_t child_key{static_cast<bison::hash_t>(f.as<int32_t>())};
    auto* child_f = (*children_f)->findField(child_key);
    if (!child_f || !child_f->is<dynamic_ptr>())
      return;
    auto child = child_f->as<dynamic_ptr>();
    if (child && child->as<bison::key_t>(dynamic::CLASS) == "TableRow"_key)
      result.push_back(child);
  });
  return result;
}

// TableRow's "visible" field is inherited from Element (default true) --
// see tail::append_row()/reapply_filter_visibility() for how the filter
// drives it, and render_table()'s own visible check for how a false value
// skips the row entirely at render time.
static bool row_visible(const dynamic_ptr& row) {
  if (!row)
    return false;
  auto* v = row->findField<bool>("visible"_key);
  return v ? *v : true;
}

// Column order matches append_row(): 0=time, 1=level, 2=tag, 3=source, 4=message.
static std::string cell_text(const dynamic_ptr& row, size_t column) {
  if (!row)
    return {};
  auto* cf = row->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf || (*cf)->size() <= column)
    return {};
  auto& f = (*cf)->at(column);
  if (!f.is<dynamic_ptr>())
    return {};
  auto cell = f.as<dynamic_ptr>();
  return cell ? cell->as<std::string>("text"_key) : std::string{};
}

// @p color_key is "text_color_light"_key or "text_color_dark"_key -- tail
// stores both on a classified cell (see tail::append_row()) and leaves the
// actual light/dark pick to render_label() at render time.
static std::string cell_color(const dynamic_ptr& row, size_t column, bison::key_t color_key) {
  if (!row)
    return {};
  auto* cf = row->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf || (*cf)->size() <= column)
    return {};
  auto& f = (*cf)->at(column);
  if (!f.is<dynamic_ptr>())
    return {};
  auto cell = f.as<dynamic_ptr>();
  if (!cell)
    return {};
  auto* color_f = cell->findField<std::string>(color_key);
  return color_f ? *color_f : std::string{};
}

// ── Fixture ─────────────────────────────────────────────────────────────────

class TailTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Tail"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());

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

  struct CapturedEvent {
    bison::key_t name;
    dynamic payload;
  };

  dynamic_ptr all_table() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.tab_bar.tab_all.table_all");
    return it != srv_->last_session->ui_objects.end() ? dynamic_ptr{it->second} : dynamic_ptr{nullptr};
  }

  dynamic_ptr tab_bar() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.tab_bar");
    return it != srv_->last_session->ui_objects.end() ? dynamic_ptr{it->second} : dynamic_ptr{nullptr};
  }

  dynamic_ptr status_label() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.status_label");
    return it != srv_->last_session->ui_objects.end() ? dynamic_ptr{it->second} : dynamic_ptr{nullptr};
  }

  void push_line(const std::string& text, const std::string& source = "") {
    auto entry = std::make_shared<dynamic>();
    (*entry)["text"_key] = text;
    (*entry)["source"_key] = source;
    dynamic entries;
    entries[size_t{0}] = dynamic_ptr{entry};
    dynamic args;
    args["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(entries))};
    proxy_->call("push_lines"_key, std::move(args)).get();
  }

  dynamic set_filter(const std::string& pattern) {
    dynamic args;
    args["pattern"_key] = pattern;
    return proxy_->call("set_filter"_key, std::move(args)).get();
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

  // Mirrors render_input_text()'s own "changed" event shape (see
  // imgui_ui_renderer.cpp): the renderer writes the new value onto the
  // element itself before enqueuing, so tests do the same rather than
  // relying on the handler to do it.
  void simulate_filter_input_changed(const std::string& text) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.toolbar.filter_input");
    ASSERT_NE(it, objs.end());
    (*it->second)["value"_key] = text;
    auto input_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    dynamic payload;
    payload["value"_key] = text;
    h->second->on_event(input_id, "changed"_key, payload);
  }

  // Mirrors render_checkbox()'s own "changed" event shape.
  void simulate_follow_checkbox_changed(bool value) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.toolbar.chk_follow");
    ASSERT_NE(it, objs.end());
    (*it->second)["value"_key] = value;
    auto chk_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    dynamic payload;
    payload["value"_key] = value;
    h->second->on_event(chk_id, "changed"_key, payload);
  }

  dynamic set_line_count(int32_t count) {
    dynamic args;
    args["count"_key] = count;
    return proxy_->call("set_line_count"_key, std::move(args)).get();
  }

  // Mirrors render_input_int()'s own "changed" event shape.
  void simulate_lines_input_changed(int32_t value) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.toolbar.lines_input");
    ASSERT_NE(it, objs.end());
    (*it->second)["value"_key] = value;
    auto input_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    dynamic payload;
    payload["value"_key] = value;
    h->second->on_event(input_id, "changed"_key, payload);
  }

  void simulate_window_closed() {
    auto win_id = srv_->last_session->ui_objects.at(root_)->as<bison::key_t>("__wish_id"_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(win_id, "closed"_key, dynamic{});
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

  // Last captured event named @p name, or nullptr if none arrived. Callers
  // needing the payload should wait_for_event() first.
  const CapturedEvent* last_event(bison::key_t name) const {
    const CapturedEvent* result = nullptr;
    for (auto& e : *events_)
      if (e.name == name)
        result = &e;
    return result;
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

// ── Internal tree ──────────────────────────────────────────────────────────────

TEST_F(TailTest, TreeContainsFilterInput) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.toolbar.filter_input"));
}

TEST_F(TailTest, TreeContainsAllTab) {
  EXPECT_NE(all_table(), nullptr);
}

// ── push_lines / classification ───────────────────────────────────────────────

TEST_F(TailTest, PlainLineAddsRowToAllTable) {
  push_line("just a plain line", "app.log");
  auto table = all_table();
  ASSERT_EQ(count_children_of_class(table, "TableRow"_key), 1u);
  auto row = first_row(table);
  EXPECT_EQ(cell_text(row, 4), "just a plain line");
  EXPECT_EQ(cell_text(row, 3), "app.log");
}

// tail::append_row() stores *both* colors a classified line carries
// (patterns.json's per-level light_color/dark_color) on each cell, as
// "text_color_light"/"text_color_dark" -- it never picks one itself.
// render_label() (imgui_ui_renderer.cpp) picks between them at render time,
// live against style_service::is_light_theme() every frame -- see
// ImguiRendererTest.LabelWithThemeColorsFollowsIsLightTheme
// (test_imgui_renderer.cpp) for that half. Storing both, undecided, means a
// theme change mid-session recolors already-ingested lines on the very next
// frame instead of leaving them stuck with whatever theme was active when
// each line first arrived.
TEST_F(TailTest, LevelPrefixedLineIsClassifiedError) {
  push_line("ERROR: disk full", "app.log");
  auto row = first_row(all_table());
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(cell_text(row, 1), "ERROR");
  EXPECT_EQ(cell_text(row, 4), "disk full");
  // patterns.json's "error" level_rules light_color/dark_color -- see
  // resources/embedded/patterns.json.
  EXPECT_EQ(cell_color(row, 1, "text_color_light"_key), "#D70015FF");
  EXPECT_EQ(cell_color(row, 4, "text_color_light"_key), "#D70015FF");
  EXPECT_EQ(cell_color(row, 1, "text_color_dark"_key), "#FF6961FF");
  EXPECT_EQ(cell_color(row, 4, "text_color_dark"_key), "#FF6961FF");
}

TEST_F(TailTest, TaggedLineCreatesTagTab) {
  push_line("[Renderer] frame took 16ms", "app.log");
  EXPECT_EQ(count_children_of_class(tab_bar(), "TabItem"_key), 2u); // "All" + "[Renderer]"
  auto tag_table = find_tab_table(tab_bar(), "[Renderer]");
  ASSERT_NE(tag_table, nullptr);
  EXPECT_EQ(count_children_of_class(tag_table, "TableRow"_key), 1u);
  // The line also lands in the "All" table, not just its tag's tab.
  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 1u);
}

// Regression test for a column/row key collision in build_log_table(): the
// table's "children" map holds both the 5 TableColumn entries and every
// appended TableRow, and dynamic's numeric operator[](size_t) shares the
// same underlying field map as its string-keyed operator[](key_t) (see
// bison_object.hpp) -- so placing the columns at raw size_t{0}..{4} and
// then appending rows starting from a *numeric* child_key of 0 (see
// log_table_state::next_child_key) silently overwrote each TableColumn
// with a TableRow, losing both the header row and the configured column
// widths after only a handful of lines. build_log_table() now keys its
// columns with string-hashed names (col_time, col_level, ...) instead.
TEST_F(TailTest, TagTabRetainsColumnsAndHeadersAfterManyRows) {
  for (int i = 0; i < 6; ++i)
    push_line("[Renderer] frame " + std::to_string(i), "app.log");

  auto tag_table = find_tab_table(tab_bar(), "[Renderer]");
  ASSERT_NE(tag_table, nullptr);
  EXPECT_TRUE(tag_table->as<bool>("headers"_key));
  EXPECT_EQ(count_children_of_class(tag_table, "TableColumn"_key), 5u);
  EXPECT_EQ(count_children_of_class(tag_table, "TableRow"_key), 6u);
}

// A tag's tab is a permanent part of the session's tab bar once created --
// matching the static "All" tab, there is no user-facing way to remove it
// (see ensure_tag_tab()'s doc comment).
TEST_F(TailTest, TagTabIsNotClosable) {
  push_line("[Renderer] frame took 16ms", "app.log");

  auto tab = find_tab_item(tab_bar(), "[Renderer]");
  ASSERT_NE(tab, nullptr);
  EXPECT_FALSE(tab->as<bool>("closable"_key));
}

TEST_F(TailTest, BracketedLevelWordIsNotTreatedAsATag) {
  push_line("[ERROR] disk full", "app.log");
  // Only the static "All" tab should exist -- "ERROR" is a severity word,
  // not a tag, per log_line_parser's tag-extraction rule.
  EXPECT_EQ(count_children_of_class(tab_bar(), "TabItem"_key), 1u);
  auto row = first_row(all_table());
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(cell_text(row, 1), "ERROR");
}

TEST_F(TailTest, StatusLabelTracksLineCount) {
  push_line("one", "a.log");
  push_line("two", "a.log");
  auto label = status_label();
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->as<std::string>("text"_key), "2 lines");
}

// ── Filtering ──────────────────────────────────────────────────────────────────
// The filter controls row *visibility*, not admission: every ingested line
// always becomes a row (see tail::ingest_line()), and set_filter re-walks
// every already-buffered row to update which ones are shown -- so the
// filter can be edited dynamically to search through lines already on
// screen, unlike `tail -f | grep pattern`.

TEST_F(TailTest, AllLinesAreAddedRegardlessOfFilter) {
  set_filter("nomatch");
  push_line("dropped by filter", "a.log");
  push_line("this has nomatch in it", "a.log");

  // Both lines are still rows -- the filter only hides one of them.
  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 2u);
}

TEST_F(TailTest, SettingFilterHidesNonMatchingRowsRetroactively) {
  push_line("alpha line", "a.log");
  push_line("beta line", "a.log");

  set_filter("alpha"); // set after both lines already arrived

  auto table = all_table();
  EXPECT_TRUE(row_visible(row_at(table, 0)));  // "alpha line"
  EXPECT_FALSE(row_visible(row_at(table, 1))); // "beta line"
}

TEST_F(TailTest, ClearingFilterShowsAllRowsAgain) {
  push_line("alpha line", "a.log");
  push_line("beta line", "a.log");
  set_filter("alpha");
  set_filter("");

  auto table = all_table();
  EXPECT_TRUE(row_visible(row_at(table, 0)));
  EXPECT_TRUE(row_visible(row_at(table, 1)));
}

TEST_F(TailTest, InvalidFilterRegexLeavesPreviousVisibilityActive) {
  set_filter("keep");
  push_line("keep this", "a.log");
  push_line("drop this", "a.log");
  set_filter("("); // invalid -- unbalanced group; previous "keep" filter stays active
  push_line("keep again", "a.log");

  auto table = all_table();
  EXPECT_TRUE(row_visible(row_at(table, 0)));  // "keep this"
  EXPECT_FALSE(row_visible(row_at(table, 1))); // "drop this"
  EXPECT_TRUE(row_visible(row_at(table, 2)));  // "keep again"
}

TEST_F(TailTest, ClearingFilterInputShowsAllRowsAgain) {
  set_filter("nomatch");
  push_line("dropped", "a.log");
  simulate_filter_input_changed(""); // emptying the box clears the filter -- no separate button

  EXPECT_TRUE(row_visible(row_at(all_table(), 0)));
}

TEST_F(TailTest, FilterInputChangeUpdatesVisibilityImmediately) {
  push_line("dropped", "a.log");
  push_line("this has nomatch in it", "a.log");
  simulate_filter_input_changed("nomatch");

  auto table = all_table();
  EXPECT_FALSE(row_visible(row_at(table, 0)));
  EXPECT_TRUE(row_visible(row_at(table, 1)));
}

TEST_F(TailTest, FilterVisibilityAppliesToTagTabsToo) {
  push_line("[Renderer] keep me", "a.log");
  push_line("[Renderer] drop me", "a.log");
  set_filter("keep");

  auto tag_table = find_tab_table(tab_bar(), "[Renderer]");
  ASSERT_NE(tag_table, nullptr);
  EXPECT_TRUE(row_visible(row_at(tag_table, 0)));
  EXPECT_FALSE(row_visible(row_at(tag_table, 1)));
}

// ── Follow checkbox ────────────────────────────────────────────────────────────

TEST_F(TailTest, FollowDefaultsEnabled) {
  auto table = all_table();
  ASSERT_NE(table, nullptr);
  auto* auto_scroll = table->findField<bool>("auto_scroll"_key);
  ASSERT_NE(auto_scroll, nullptr);
  EXPECT_TRUE(*auto_scroll);
}

TEST_F(TailTest, DisablingFollowClearsAutoScrollOnAllTable) {
  simulate_follow_checkbox_changed(false);

  auto table = all_table();
  ASSERT_NE(table, nullptr);
  auto* auto_scroll = table->findField<bool>("auto_scroll"_key);
  ASSERT_NE(auto_scroll, nullptr);
  EXPECT_FALSE(*auto_scroll);
}

TEST_F(TailTest, ReEnablingFollowRestoresAutoScrollOnAllTable) {
  simulate_follow_checkbox_changed(false);
  simulate_follow_checkbox_changed(true);

  auto table = all_table();
  ASSERT_NE(table, nullptr);
  auto* auto_scroll = table->findField<bool>("auto_scroll"_key);
  ASSERT_NE(auto_scroll, nullptr);
  EXPECT_TRUE(*auto_scroll);
}

// A tag tab's table is built on first sighting of its tag (see
// tail::ensure_tag_tab()/build_log_table()) -- it must inherit whatever
// Follow state is current at creation time, not always default to enabled.
TEST_F(TailTest, TagTabCreatedWhileFollowDisabledStartsWithAutoScrollOff) {
  simulate_follow_checkbox_changed(false);
  push_line("[Renderer] frame took 16ms", "app.log");

  auto tag_table = find_tab_table(tab_bar(), "[Renderer]");
  ASSERT_NE(tag_table, nullptr);
  auto* auto_scroll = tag_table->findField<bool>("auto_scroll"_key);
  ASSERT_NE(auto_scroll, nullptr);
  EXPECT_FALSE(*auto_scroll);
}

// ── Lines field / row cap ─────────────────────────────────────────────────────
// The Lines field is a live per-table row cap (default 10, see kLayout's
// "lines_input"): whenever a table would hold more rows than this, the
// oldest are dropped immediately -- whether growth came from a newly
// ingested line (append_row()'s own evict_to_cap() call) or from lowering
// the field itself (do_set_line_count()'s explicit evict_to_cap() calls).
// This form has no filesystem access, so it cannot pull in more history
// on its own when the user *raises* the field from the UI -- on_event()'s
// handling of that additionally clears every table and emits
// 'rescan_requested', which only the real client (untestable here --
// see modules/bdg/desktop/tail/client/tail.cpp) can actually answer by
// re-reading the file.

TEST_F(TailTest, LinesInputDefaultsToTen) {
  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_ + ".vbox.toolbar.lines_input");
  ASSERT_NE(it, objs.end());
  EXPECT_EQ(it->second->as<int32_t>("value"_key), 10);
}

TEST_F(TailTest, PushingMoreLinesThanCapDropsOldestKeepsNewest) {
  set_line_count(3);
  push_line("one", "a.log");
  push_line("two", "a.log");
  push_line("three", "a.log");
  push_line("four", "a.log");

  auto table = all_table();
  auto rows = ordered_rows(table);
  // "one" aged out; "two"/"three"/"four" remain, oldest-first.
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(cell_text(rows[0], 4), "two");
  EXPECT_EQ(cell_text(rows[1], 4), "three");
  EXPECT_EQ(cell_text(rows[2], 4), "four");

  // Eviction never rewinds the all-time arrival count.
  EXPECT_EQ(status_label()->as<std::string>("text"_key), "4 lines");
}

TEST_F(TailTest, SetLineCountUpdatesDisplay) {
  set_line_count(25);
  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_ + ".vbox.toolbar.lines_input");
  ASSERT_NE(it, objs.end());
  EXPECT_EQ(it->second->as<int32_t>("value"_key), 25);
}

TEST_F(TailTest, SetLineCountClampsBelowOneToOne) {
  set_line_count(0);
  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_ + ".vbox.toolbar.lines_input");
  ASSERT_NE(it, objs.end());
  EXPECT_EQ(it->second->as<int32_t>("value"_key), 1);
}

TEST_F(TailTest, SetLineCountClampsAboveCeilingTo2000) {
  // 2000 mirrors tail.cpp's own kMaxBufferedRows -- not exposed via the
  // header, so duplicated here; keep in sync if that constant changes.
  set_line_count(1'000'000);
  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_ + ".vbox.toolbar.lines_input");
  ASSERT_NE(it, objs.end());
  EXPECT_EQ(it->second->as<int32_t>("value"_key), 2000);
}

TEST_F(TailTest, LoweringCapViaSetLineCountEvictsOldestImmediately) {
  push_line("one", "a.log");
  push_line("two", "a.log");
  push_line("three", "a.log");
  ASSERT_EQ(count_children_of_class(all_table(), "TableRow"_key), 3u);

  set_line_count(2); // no new lines arrive -- eviction must happen right here

  auto rows = ordered_rows(all_table());
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(cell_text(rows[0], 4), "two");
  EXPECT_EQ(cell_text(rows[1], 4), "three");
}

// set_line_count is the RMI method (used by the client to reflect its own
// startup -n value); it never clears the table or emits, unlike editing
// the field via the UI (see EditingLinesInputClearsTableAndEmitsRescan-
// Requested below) -- so raising it this way truly has nothing to restore.
TEST_F(TailTest, RaisingCapDoesNotRestoreAlreadyEvictedRows) {
  set_line_count(2);
  push_line("one", "a.log");
  push_line("two", "a.log");
  push_line("three", "a.log"); // "one" evicted immediately (append_row's own cap check)
  ASSERT_EQ(count_children_of_class(all_table(), "TableRow"_key), 2u);

  set_line_count(10); // raising the cap has nothing to restore

  auto rows = ordered_rows(all_table());
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(cell_text(rows[0], 4), "two");
  EXPECT_EQ(cell_text(rows[1], 4), "three");
}

// Editing the field via the UI (unlike the set_line_count RMI method) also
// clears every table and asks the client to rescan -- see on_event()'s
// handling of "lines_input" -- since only the client can actually pull in
// more history when the count is raised. The clear happens unconditionally
// (even when lowering), so the client's freshly re-read lines replace what
// was shown instead of appending after it.
TEST_F(TailTest, EditingLinesInputClearsTableAndEmitsRescanRequested) {
  push_line("one", "a.log");
  push_line("two", "a.log");
  push_line("three", "a.log");
  ASSERT_EQ(count_children_of_class(all_table(), "TableRow"_key), 3u);

  simulate_lines_input_changed(20);

  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 0u);
  ASSERT_TRUE(wait_for_event("rescan_requested"_key));
  auto* ev = last_event("rescan_requested"_key);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->payload.as<int32_t>("line_count"_key), 20);
}

TEST_F(TailTest, EditingLinesInputBelowOneClampsToOneInEventAndDisplay) {
  simulate_lines_input_changed(-5);

  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_ + ".vbox.toolbar.lines_input");
  ASSERT_NE(it, objs.end());
  EXPECT_EQ(it->second->as<int32_t>("value"_key), 1);

  ASSERT_TRUE(wait_for_event("rescan_requested"_key));
  auto* ev = last_event("rescan_requested"_key);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->payload.as<int32_t>("line_count"_key), 1);
}

TEST_F(TailTest, RowCapAppliesToTagTabsIndependently) {
  set_line_count(1);
  push_line("[Renderer] one", "a.log");
  push_line("[Renderer] two", "a.log");

  // The "All" table and the tag table each have their own independent cap
  // (matching how the pre-existing FIFO cap already worked per-table) --
  // both end up holding just the newest line, but that's two separate
  // one-row evictions, not a cap shared across tables.
  auto tag_table = find_tab_table(tab_bar(), "[Renderer]");
  ASSERT_NE(tag_table, nullptr);
  auto tag_rows = ordered_rows(tag_table);
  ASSERT_EQ(tag_rows.size(), 1u);
  EXPECT_EQ(cell_text(tag_rows[0], 4), "[Renderer] two");

  auto all_rows = ordered_rows(all_table());
  ASSERT_EQ(all_rows.size(), 1u);
  EXPECT_EQ(cell_text(all_rows[0], 4), "[Renderer] two");
}

// ── Toolbar / lifecycle ────────────────────────────────────────────────────────

TEST_F(TailTest, ClearAllButtonEmptiesAllTable) {
  push_line("one", "a.log");
  push_line("two", "a.log");
  ASSERT_EQ(count_children_of_class(all_table(), "TableRow"_key), 2u);

  simulate_btn_click("btn_clear");

  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 0u);
  auto label = status_label();
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->as<std::string>("text"_key), "0 lines");
}

TEST_F(TailTest, WindowClosedEmitsClosed) {
  simulate_window_closed();
  EXPECT_TRUE(wait_for_event("closed"_key));
}
