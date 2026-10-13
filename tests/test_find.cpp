// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <standalone/standalone.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

struct fake_row {
  std::string name;
  std::string path;
  bool dir = false;
};

const std::vector<fake_row> kRows{
    {"docs", "/home/u", true},
    {"a.txt", "/home/u/docs"},
    {"b.png", "/home/u"},
};

// The {generation, total, folders, entries} shape find_form::do_show_results()
// expects -- matches the reference client (modules/bdg/desktop/find/client/
// find.cpp)'s run_search().
dynamic make_results(int32_t generation, const std::vector<fake_row>& rows, int32_t total = -1) {
  dynamic list;
  size_t i = 0;
  for (auto& r : rows) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = r.name;
    (*e)["path"_key] = r.path;
    (*e)["type"_key] = std::string{r.dir ? "$$FIND_TYPE_FOLDER" : "$$FIND_TYPE_FILE"};
    (*e)["size"_key] = std::string{"1 KB"};
    (*e)["modified"_key] = std::string{"2026-10-12 10:00"};
    (*e)["dir"_key] = r.dir;
    list[i++] = dynamic_ptr{e};
  }
  dynamic args;
  args["generation"_key] = generation;
  args["total"_key] = total < 0 ? static_cast<int32_t>(rows.size()) : total;
  args["folders"_key] = int32_t{1};
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(list))};
  return args;
}

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::context* last_session{nullptr};
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

std::string find_form_root(const wish::name_map& objects) {
  static const std::string prefix = "__find_";
  for (const auto& [k, _] : objects) {
    if (k.size() > prefix.size() && k.rfind(prefix, 0) == 0 &&
        std::all_of(k.begin() + prefix.size(), k.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
      return k;
  }
  return {};
}

std::string first_label(const dynamic& node) {
  if (node.as<bison::key_t>(dynamic::CLASS) == "Label"_key)
    return node.as<std::string>("text"_key);
  std::string found;
  if (auto* cf = node.findField<dynamic_ptr>("children"_key); cf && *cf)
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (found.empty() && f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
        found = first_label(*f.as<dynamic_ptr>());
    });
  return found;
}

} // namespace

// ── Prototype defaults ────────────────────────────────────────────────────────

TEST(FindLocalTest, PrototypeDefaults) {
  bdg::wish::register_all();
  auto obj = dynamic::instantiate("wish"_key, "Find"_key);
  ASSERT_NE(obj.findField(dynamic::CLASS), nullptr);
  EXPECT_EQ(obj.findField(dynamic::CLASS)->as<bison::key_t>(), "Find"_key);
  ASSERT_NE(obj.findField("title"_key), nullptr);
  EXPECT_EQ(obj.findField("title"_key)->as<std::string>(), "$$FIND_TITLE");
  ASSERT_NE(obj.findField("db_files"_key), nullptr);
  EXPECT_EQ(obj.findField("db_files"_key)->as<int32_t>(), -1);
}

// ── Form fixture ──────────────────────────────────────────────────────────────

class FindTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Find"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());
    handler_ = srv_->last_session->top_level_handlers.find(root_)->second;
    ASSERT_NE(handler_, nullptr);
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  wish::ui_element_ptr& element(const std::string& path) { return srv_->last_session->ui_objects.at(root_ + path); }
  bison::key_t widget_id(const std::string& path) { return element(path)->as<bison::key_t>("__wish_id"_key); }
  std::string text_of(const std::string& path) { return element(path)->as<std::string>("text"_key); }

  void fire(const std::string& path, bison::key_t event, dynamic payload = {}) {
    handler_->on_event(widget_id(path), event, payload);
  }

  /// Types @p query and returns the on_search it emitted.
  std::optional<dynamic> type(const std::string& query) {
    size_t since = srv_->events->mark();
    dynamic payload;
    payload["value"_key] = query;
    fire(".main.bar.query", "changed"_key, std::move(payload));
    return srv_->events->wait_for("on_search"_key, since);
  }

  void show(int32_t generation, const std::vector<fake_row>& rows = kRows, int32_t total = -1) {
    proxy_->call("show_results"_key, make_results(generation, rows, total)).get();
  }

  std::vector<const dynamic*> rows() {
    std::vector<std::pair<int32_t, const dynamic*>> out;
    auto* cf = element(".main.table")->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      const dynamic& row = *f.as<dynamic_ptr>();
      if (row.as<bison::key_t>(dynamic::CLASS) == "TableRow"_key)
        out.emplace_back(row.as<int32_t>("order"_key), &row);
    });
    std::sort(out.begin(), out.end());
    std::vector<const dynamic*> v;
    for (auto& p : out)
      v.push_back(p.second);
    return v;
  }

  std::vector<std::string> row_names() {
    std::vector<std::string> names;
    for (auto* r : rows())
      names.push_back(first_label(*r));
    return names;
  }

  std::vector<bool> row_selection() {
    std::vector<bool> sel;
    for (auto* r : rows())
      sel.push_back(r->as<bool>("selected"_key));
    return sel;
  }

  void select_row(int32_t index, bool ctrl = false, bool shift = false, bison::key_t event = "row_selected"_key) {
    dynamic payload;
    payload["index"_key] = index;
    payload["ctrl"_key] = ctrl;
    payload["shift"_key] = shift;
    fire(".main.table", event, std::move(payload));
  }

  void click(const std::string& path) { fire(path, "clicked"_key); }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  wish::ui_root* handler_{nullptr};
};

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(FindTest, BuildsFsearchLikeWindow) {
  auto& s = *srv_->last_session;
  for (const char* path :
       {".menu.file.update", ".menu.edit.copy_full_path", ".menu.view.statusbar", ".menu.search.match_case",
        ".menu.search.search_in_path", ".menu.search.regex", ".menu.help.syntax", ".main.bar.query",
        ".main.bar.filter", ".main.table", ".main.statusbar.status", ".main.statusbar.db"})
    EXPECT_TRUE(s.ui_objects.count(root_ + path)) << path;
  EXPECT_EQ(element("")->as<std::string>("title"_key), "Find");
  EXPECT_EQ(element(".main.table")->as<int32_t>("columns"_key), 5);
  EXPECT_EQ(element(".main.bar.filter")->as<std::string>("items"_key),
            "All\nFolders\nFiles\nApplications\nArchives\nAudio\nDocuments\nPictures\nVideos");
  EXPECT_EQ(text_of(".main.statusbar.status"), "Loading database...");
  EXPECT_FALSE(element(".main.statusbar.case")->as<bool>("visible"_key));
  EXPECT_TRUE(row_names().empty());
}

// ── Searching ─────────────────────────────────────────────────────────────────

TEST_F(FindTest, TypingEmitsOnSearchWithState) {
  auto ev = type("report");
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("query"_key), "report");
  EXPECT_EQ(ev->as<int32_t>("filter"_key), 0);
  EXPECT_FALSE(ev->as<bool>("match_case"_key));
  EXPECT_EQ(ev->as<int32_t>("sort_column"_key), 0);
  EXPECT_TRUE(ev->as<bool>("ascending"_key));

  auto ev2 = type("report.pdf");
  ASSERT_TRUE(ev2);
  EXPECT_GT(ev2->as<int32_t>("generation"_key), ev->as<int32_t>("generation"_key));
}

TEST_F(FindTest, FilterModesAndSortReachTheSearch) {
  size_t since = srv_->events->mark();
  dynamic f;
  f["value"_key] = int32_t{7};
  fire(".main.bar.filter", "changed"_key, std::move(f));
  auto ev = srv_->events->wait_for("on_search"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<int32_t>("filter"_key), 7);
  EXPECT_TRUE(element(".main.statusbar.filter")->as<bool>("visible"_key));
  EXPECT_EQ(text_of(".main.statusbar.filter"), "Pictures");

  since = srv_->events->mark();
  click(".menu.search.regex");
  ev = srv_->events->wait_for("on_search"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_TRUE(ev->as<bool>("regex"_key));
  EXPECT_TRUE(element(".menu.search.regex")->as<bool>("checked"_key));
  EXPECT_TRUE(element(".main.statusbar.regex")->as<bool>("visible"_key));

  since = srv_->events->mark();
  dynamic sort;
  sort["column_id"_key] = int32_t{3};
  sort["ascending"_key] = false;
  fire(".main.table", "sorted"_key, std::move(sort));
  ev = srv_->events->wait_for("on_search"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<int32_t>("sort_column"_key), 3);
  EXPECT_FALSE(ev->as<bool>("ascending"_key));
  EXPECT_TRUE(ev->as<bool>("regex"_key));
}

TEST_F(FindTest, ShowResultsFillsTableAndStatus) {
  auto ev = type("a");
  ASSERT_TRUE(ev);
  show(ev->as<int32_t>("generation"_key));
  EXPECT_EQ(row_names(), (std::vector<std::string>{"docs", "a.txt", "b.png"}));
  EXPECT_EQ(text_of(".main.statusbar.status"), "3 Items");
  // Name, Path, Type (translated), Size, Modified, then the context menu.
  auto* first = rows().at(0);
  std::vector<std::string> cells;
  std::vector<std::pair<int32_t, std::string>> ordered;
  first->findField<dynamic_ptr>("children"_key)->get()->forEach([&](bison::key_t, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    const dynamic& c = *f.as<dynamic_ptr>();
    if (c.as<bison::key_t>(dynamic::CLASS) == "Label"_key)
      ordered.emplace_back(c.as<int32_t>("order"_key), c.as<std::string>("text"_key));
  });
  std::sort(ordered.begin(), ordered.end());
  for (auto& o : ordered)
    cells.push_back(o.second);
  EXPECT_EQ(cells, (std::vector<std::string>{"/home/u", "Folder", "1 KB", "2026-10-12 10:00"}));
}

TEST_F(FindTest, StaleResultsAreDropped) {
  auto ev1 = type("a");
  auto ev2 = type("ab");
  ASSERT_TRUE(ev1 && ev2);
  show(ev1->as<int32_t>("generation"_key));
  EXPECT_TRUE(row_names().empty());
  show(ev2->as<int32_t>("generation"_key), {{"ab.txt", "/x"}});
  EXPECT_EQ(row_names(), (std::vector<std::string>{"ab.txt"}));
  EXPECT_EQ(text_of(".main.statusbar.status"), "1 Item");
}

TEST_F(FindTest, TruncatedResultsSayHowManyMatched) {
  auto ev = type("");
  ASSERT_TRUE(ev);
  show(ev->as<int32_t>("generation"_key), kRows, 52345);
  EXPECT_EQ(text_of(".main.statusbar.status"), "52,345 Items (showing the first 3)");
}

TEST_F(FindTest, InvalidQueryShowsTheError) {
  auto ev = type("size:huge");
  ASSERT_TRUE(ev);
  auto args = make_results(ev->as<int32_t>("generation"_key), {});
  args["error"_key] = std::string{"size:huge"};
  proxy_->call("show_results"_key, std::move(args)).get();
  EXPECT_EQ(text_of(".main.statusbar.status"), "Invalid query: size:huge");
}

// ── Database state ────────────────────────────────────────────────────────────

TEST_F(FindTest, DatabaseStateReachesTheStatusBar) {
  dynamic patch;
  patch["indexing"_key] = true;
  patch["status"_key] = std::string{"Indexing: 10 files"};
  proxy_->set(std::move(patch)).get();
  proxy_->get().get(); // set() is one-way; a round trip guarantees it applied
  EXPECT_EQ(text_of(".main.statusbar.status"), "Indexing: 10 files");

  dynamic done;
  done["indexing"_key] = false;
  done["status"_key] = std::string{};
  done["db_files"_key] = int32_t{1234};
  done["db_folders"_key] = int32_t{56};
  proxy_->set(std::move(done)).get();
  proxy_->get().get();
  EXPECT_EQ(text_of(".main.statusbar.db"), "1,234 files, 56 folders");

  size_t since = srv_->events->mark();
  proxy_->call("search_again"_key, dynamic{}).get();
  EXPECT_TRUE(srv_->events->wait_for("on_search"_key, since));

  since = srv_->events->mark();
  click(".menu.file.update");
  EXPECT_TRUE(srv_->events->wait_for("on_update_database"_key, since));
}

// ── Selection and opening ─────────────────────────────────────────────────────

TEST_F(FindTest, ClickCtrlAndShiftSelect) {
  auto ev = type("");
  ASSERT_TRUE(ev);
  show(ev->as<int32_t>("generation"_key));

  select_row(0);
  EXPECT_EQ(row_selection(), (std::vector<bool>{true, false, false}));
  EXPECT_EQ(text_of(".main.statusbar.selection"), "1 / 3 selected");
  EXPECT_EQ(element(".menu.edit.copy_full_path")->as<std::string>("copy_text"_key), "/home/u/docs");
  EXPECT_TRUE(element(".menu.edit.copy_full_path")->as<bool>("enabled"_key));

  select_row(2, /*ctrl=*/true);
  EXPECT_EQ(row_selection(), (std::vector<bool>{true, false, true}));
  EXPECT_EQ(element(".menu.edit.copy_name")->as<std::string>("copy_text"_key), "docs\nb.png");

  select_row(0, false, /*shift=*/true);
  EXPECT_EQ(row_selection(), (std::vector<bool>{true, true, true}));

  click(".menu.edit.deselect");
  EXPECT_EQ(row_selection(), (std::vector<bool>{false, false, false}));
  EXPECT_FALSE(element(".main.statusbar.selection")->as<bool>("visible"_key));
}

TEST_F(FindTest, DoubleClickAndMenusOpen) {
  auto ev = type("");
  ASSERT_TRUE(ev);
  show(ev->as<int32_t>("generation"_key));

  size_t since = srv_->events->mark();
  select_row(1, false, false, "row_activated"_key);
  auto open = srv_->events->wait_for("on_open"_key, since);
  ASSERT_TRUE(open);
  EXPECT_EQ(open->as<std::string>("paths"_key), "/home/u/docs/a.txt");

  select_row(2, /*ctrl=*/true);
  since = srv_->events->mark();
  click(".menu.file.open_folder");
  auto folder = srv_->events->wait_for("on_open_folder"_key, since);
  ASSERT_TRUE(folder);
  EXPECT_EQ(folder->as<std::string>("paths"_key), "/home/u/docs/a.txt\n/home/u/b.png");
}

TEST_F(FindTest, ModesAreRememberedPerFormAndCloseEmitsClosed) {
  click(".menu.view.statusbar");
  EXPECT_FALSE(element(".main.statusbar")->as<bool>("visible"_key));
  EXPECT_FALSE(element(".menu.view.statusbar")->as<bool>("checked"_key));
  click(".menu.view.filter");
  EXPECT_FALSE(element(".main.bar.filter")->as<bool>("visible"_key));

  size_t since = srv_->events->mark();
  click(".menu.file.close");
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since));
}
