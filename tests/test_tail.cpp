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

static std::string cell_color(const dynamic_ptr& row, size_t column) {
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
  auto* color_f = cell->findField<std::string>("text_color"_key);
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

TEST_F(TailTest, LevelPrefixedLineIsClassifiedError) {
  push_line("ERROR: disk full", "app.log");
  auto row = first_row(all_table());
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(cell_text(row, 1), "ERROR");
  EXPECT_EQ(cell_text(row, 4), "disk full");
  // patterns.json's "error" level_rules color -- see resources/embedded/patterns.json.
  EXPECT_EQ(cell_color(row, 1), "#FF6961FF");
  EXPECT_EQ(cell_color(row, 4), "#FF6961FF");
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

TEST_F(TailTest, FilterAppliesProspectivelyOnly) {
  push_line("keep me", "a.log");
  set_filter("nomatch");
  push_line("also kept? no", "a.log"); // does not match "nomatch" -- excluded
  push_line("this line has nomatch in it", "a.log"); // matches -- included

  // The line pushed *before* the filter was set is unaffected by it.
  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 2u);
}

TEST_F(TailTest, InvalidFilterRegexLeavesPreviousFilterActive) {
  set_filter("keep");
  set_filter("("); // invalid -- unbalanced group
  push_line("keep this", "a.log"); // matches the still-active "keep" filter
  push_line("drop this", "a.log"); // does not match

  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 1u);
  auto row = first_row(all_table());
  EXPECT_EQ(cell_text(row, 4), "keep this");
}

TEST_F(TailTest, ClearFilterButtonRestoresUnfilteredIngestion) {
  set_filter("nomatch");
  push_line("dropped", "a.log");
  simulate_btn_click("btn_clear_filter");
  push_line("kept after clear", "a.log");

  EXPECT_EQ(count_children_of_class(all_table(), "TableRow"_key), 1u);
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
