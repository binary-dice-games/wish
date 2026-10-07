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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

struct fake_entry {
  std::string name;
  std::string type; ///< "file", "dir" or "rest"
  std::string size; ///< decimal bytes
  int32_t items = 0;
};

// The {root, path, size, files, dirs, entries, <treemap arrays>} shape
// Du::do_show_directory() expects -- matches the reference client
// (modules/bdg/desktop/du/client/du.cpp)'s show_directory().
//
// The treemap is the fixed subtree every test uses:
//   0 root | 1 docs (dir) { 3 a.txt, 4 deep (dir) { 5 c.md } } | 2 big.bin
dynamic make_directory_args(const std::string& root, const std::string& path, const std::vector<fake_entry>& entries) {
  dynamic args;
  args["root"_key] = root;
  args["path"_key] = path;
  args["size"_key] = std::string{"8000"};
  args["files"_key] = int32_t{4};
  args["dirs"_key] = int32_t{2};

  dynamic list;
  size_t i = 0;
  for (auto& e : entries) {
    auto ep = std::make_shared<dynamic>();
    (*ep)["name"_key] = e.name;
    (*ep)["type"_key] = e.type;
    (*ep)["size"_key] = e.size;
    (*ep)["items"_key] = e.items;
    list[i++] = dynamic_ptr{ep};
  }
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(list))};

  args["parents"_key] = std::vector<int32_t>{-1, 0, 0, 1, 1, 4};
  args["sizes"_key] = std::vector<float>{8000, 6000, 2000, 4000, 2000, 2000};
  args["colors"_key] = std::vector<int32_t>{1, 2, 3, 4, 5, 6};
  args["kinds"_key] = std::vector<int32_t>{1, 1, 0, 0, 1, 0};
  args["labels"_key] = std::string{"proj\ndocs\nbig.bin\na.txt\ndeep\nc.md"};
  args["details"_key] = std::string{"7.8 KB\n5.9 KB\n2.0 KB\n3.9 KB\n2.0 KB\n2.0 KB"};
  return args;
}

const std::vector<fake_entry> kEntries{
    {"big.bin", "file", "2000", 0},
    {"docs", "dir", "6000", 3},
};

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

// The Files panel's root key: "__du_<N>" exactly, not a child path nor the
// Treemap panel's "__du_<N>_treemap".
std::string find_form_root(const wish::name_map& objects) {
  static const std::string prefix = "__du_";
  for (const auto& [k, _] : objects) {
    if (k.size() > prefix.size() && k.rfind(prefix, 0) == 0 &&
        std::all_of(k.begin() + prefix.size(), k.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
      return k;
  }
  return {};
}

// Text of the first non-empty Label below @p node, depth first in child order.
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

TEST(DuLocalTest, PrototypeDefaults) {
  bdg::wish::register_all();
  auto obj = dynamic::instantiate("wish"_key, "Du"_key);
  ASSERT_NE(obj.findField(dynamic::CLASS), nullptr);
  EXPECT_EQ(obj.findField(dynamic::CLASS)->as<bison::key_t>(), "Du"_key);
  // The untyped findField() also resolves prototype defaults.
  ASSERT_NE(obj.findField("title"_key), nullptr);
  EXPECT_EQ(obj.findField("title"_key)->as<std::string>(), "Disk Usage");
  ASSERT_NE(obj.findField("status"_key), nullptr);
  EXPECT_EQ(obj.findField("status"_key)->as<std::string>(), "Ready.");
  ASSERT_NE(obj.findField("scanning"_key), nullptr);
  EXPECT_FALSE(obj.findField("scanning"_key)->as<bool>());
}

// ── Form fixture ──────────────────────────────────────────────────────────────

class DuTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Du"_key).get());
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

  void show(const std::string& path = "", const std::vector<fake_entry>& entries = kEntries) {
    proxy_->call("show_directory"_key, make_directory_args("/home/u/proj", path, entries)).get();
  }

  /// The table's data rows in display order, as their Name cell's label.
  std::vector<std::string> row_names() {
    std::vector<std::pair<int32_t, std::string>> rows;
    auto* cf = element(".main.table")->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      const dynamic& row = *f.as<dynamic_ptr>();
      if (row.as<bison::key_t>(dynamic::CLASS) != "TableRow"_key)
        return;
      // Cell 0 is make_name_cell()'s icon + Label layout; its Label is the
      // first one in the row.
      std::string name = first_label(row);
      rows.emplace_back(row.as<int32_t>("order"_key), name);
    });
    std::sort(rows.begin(), rows.end());
    std::vector<std::string> names;
    for (auto& r : rows)
      names.push_back(r.second);
    return names;
  }

  /// Name of the one selected row, "" if none.
  std::string selected_row() {
    std::string selected;
    auto* cf = element(".main.table")->findField<dynamic_ptr>("children"_key);
    auto names = row_names();
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      const dynamic& row = *f.as<dynamic_ptr>();
      if (row.as<bison::key_t>(dynamic::CLASS) == "TableRow"_key && row.as<bool>("selected"_key))
        selected = names.at(static_cast<size_t>(row.as<int32_t>("order"_key)));
    });
    return selected;
  }

  void fire(const std::string& path, bison::key_t event, dynamic payload = {}) {
    handler_->on_event(widget_id(path), event, payload);
  }

  void fire_treemap(bison::key_t event, int32_t index) {
    dynamic payload;
    payload["index"_key] = index;
    handler_->on_event(
        srv_->last_session->ui_objects.at(root_ + "_treemap.map")->as<bison::key_t>("__wish_id"_key), event, payload);
  }

  void fire_row(bison::key_t event, int32_t index) {
    dynamic payload;
    payload["index"_key] = index;
    fire(".main.table", event, std::move(payload));
  }

  wish::ui_element_ptr& treemap() { return srv_->last_session->ui_objects.at(root_ + "_treemap.map"); }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  wish::ui_root* handler_{nullptr};
};

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(DuTest, BuildsFilesAndTreemapPanels) {
  auto& s = *srv_->last_session;
  for (const std::string key : {root_, root_ + "_treemap"}) {
    auto it = s.top_level_objects.find(bison::key_t{key});
    ASSERT_NE(it, s.top_level_objects.end()) << key;
    EXPECT_EQ(it->second->as<bison::key_t>(dynamic::CLASS), "Window"_key) << key;
    EXPECT_EQ(it->second->as<std::string>("__path__"_key), key) << key;
  }
  EXPECT_TRUE(s.ui_objects.count(root_ + ".main.toolbar.path_input"));
  EXPECT_TRUE(s.ui_objects.count(root_ + ".main.toolbar.btn_scan"));
  EXPECT_TRUE(s.ui_objects.count(root_ + ".main.toolbar.btn_up"));
  EXPECT_TRUE(s.ui_objects.count(root_ + ".main.table"));
  EXPECT_EQ(treemap()->as<bison::key_t>(dynamic::CLASS), "Treemap"_key);
  EXPECT_TRUE(row_names().empty());
}

// Files is seeded as a fixed-width panel on the left of the tool's own
// dockspace, the Treemap filling the rest.
TEST_F(DuTest, DockLayoutPutsFilesInAFixedWidthLeftPanel) {
  wish::ui_element_ptr viewport;
  for (const auto& [k, obj] : srv_->last_session->top_level_objects)
    if (obj->as<bison::key_t>(dynamic::CLASS) == "DockSpaceViewport"_key)
      viewport = obj;
  ASSERT_TRUE(viewport);
  EXPECT_EQ(viewport->as<std::string>("id"_key), "du_dock");
  EXPECT_EQ(viewport->as<std::string>("title"_key), "Disk Usage");

  const dynamic* split = nullptr;
  std::vector<std::string> areas; // each DockArea's windows, in child order
  std::function<void(const dynamic&)> walk = [&](const dynamic& node) {
    if (node.as<bison::key_t>(dynamic::CLASS) == "DockSplit"_key)
      split = &node;
    if (node.as<bison::key_t>(dynamic::CLASS) == "DockArea"_key)
      areas.push_back(node.as<std::string>("windows"_key));
    if (auto* cf = node.findField<dynamic_ptr>("children"_key); cf && *cf)
      (*cf)->forEach([&](bison::key_t, const field& f) {
        if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
          walk(*f.as<dynamic_ptr>());
      });
  };
  walk(*viewport);
  ASSERT_NE(split, nullptr);
  EXPECT_EQ(split->as<std::string>("dir"_key), "left");
  EXPECT_GT(split->as<float>("size"_key), 0.0f);
  EXPECT_EQ(areas, (std::vector<std::string>{root_, root_ + "_treemap"}));
}

// ── show_directory() ──────────────────────────────────────────────────────────

TEST_F(DuTest, ShowDirectoryFillsTableLargestFirst) {
  show();
  EXPECT_EQ(row_names(), (std::vector<std::string>{"docs", "big.bin"}));
  EXPECT_EQ(element(".main.toolbar.path_input")->as<std::string>("value"_key), "/home/u/proj");
  EXPECT_EQ(text_of(".main.summary"), "7.8 KB in 4 files, 2 folders");
  EXPECT_EQ(proxy_->get().get().as<std::string>("path"_key), "/home/u/proj");
}

TEST_F(DuTest, ShowDirectoryForwardsTreemapData) {
  show();
  auto* parents = treemap()->findField<std::vector<int32_t>>("parents"_key);
  ASSERT_NE(parents, nullptr);
  EXPECT_EQ(*parents, (std::vector<int32_t>{-1, 0, 0, 1, 1, 4}));
  auto* sizes = treemap()->findField<std::vector<float>>("sizes"_key);
  ASSERT_NE(sizes, nullptr);
  EXPECT_EQ(sizes->size(), 6u);
  EXPECT_EQ(treemap()->as<std::string>("labels"_key), "proj\ndocs\nbig.bin\na.txt\ndeep\nc.md");
  EXPECT_EQ(treemap()->as<int32_t>("selected"_key), -1);
}

TEST_F(DuTest, SubfolderGetsAnUpRowAndJoinedPath) {
  show("docs/deep");
  EXPECT_EQ(row_names(), (std::vector<std::string>{"..", "docs", "big.bin"}));
  EXPECT_EQ(element(".main.toolbar.path_input")->as<std::string>("value"_key), "/home/u/proj/docs/deep");
}

TEST_F(DuTest, RestRowStaysLastWhateverTheSortOrder) {
  show("", {{"(40 smaller items)", "rest", "5000", 40}, {"a", "file", "10", 0}, {"b", "file", "20", 0}});
  EXPECT_EQ(row_names(), (std::vector<std::string>{"b", "a", "(40 smaller items)"}));

  dynamic sort;
  sort["column_id"_key] = int32_t{0};
  sort["ascending"_key] = true;
  fire(".main.table", "sorted"_key, std::move(sort));
  EXPECT_EQ(row_names(), (std::vector<std::string>{"a", "b", "(40 smaller items)"}));
}

// ── Scan requests ─────────────────────────────────────────────────────────────

TEST_F(DuTest, PathBarEnterRequestsAScan) {
  size_t since = srv_->events->mark();
  dynamic payload;
  payload["value"_key] = std::string{"/var/log"};
  fire(".main.toolbar.path_input", "changed"_key, std::move(payload));

  auto ev = srv_->events->wait_for("on_scan_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "/var/log");
}

TEST_F(DuTest, RescanButtonRescansTheScannedRootNotTheSubfolder) {
  show("docs");
  size_t since = srv_->events->mark();
  fire(".main.toolbar.btn_scan", "clicked"_key);

  auto ev = srv_->events->wait_for("on_scan_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "/home/u/proj");
}

TEST_F(DuTest, ScanningTurnsRescanIntoStop) {
  show();
  dynamic patch;
  patch["scanning"_key] = true;
  patch["status"_key] = std::string{"Scanning: 10 items"};
  proxy_->set(std::move(patch)).get();
  // set() is one-way; a round trip guarantees the server has applied it.
  proxy_->get().get();
  EXPECT_EQ(element(".main.toolbar.btn_scan")->as<std::string>("label"_key), "Stop");
  EXPECT_EQ(text_of(".main.status"), "Scanning: 10 items");

  size_t since = srv_->events->mark();
  fire(".main.toolbar.btn_scan", "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("on_cancel_requested"_key, since));
  EXPECT_FALSE(srv_->events->saw("on_scan_requested"_key, since));

  dynamic done;
  done["scanning"_key] = false;
  proxy_->set(std::move(done)).get();
  proxy_->get().get();
  EXPECT_EQ(element(".main.toolbar.btn_scan")->as<std::string>("label"_key), "Rescan");
}

// ── Navigation ────────────────────────────────────────────────────────────────

TEST_F(DuTest, ActivatingAFolderRowNavigatesIntoIt) {
  show("docs", {{"deep", "dir", "2000", 1}, {"a.txt", "file", "4000", 0}});
  // Row 0 is "..", then a.txt (largest), then deep.
  ASSERT_EQ(row_names(), (std::vector<std::string>{"..", "a.txt", "deep"}));

  size_t since = srv_->events->mark();
  fire_row("row_activated"_key, 2);
  auto ev = srv_->events->wait_for("on_navigate"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "docs/deep");
}

TEST_F(DuTest, ActivatingUpRowAndUpButtonNavigateToTheParent) {
  show("docs/deep");
  size_t since = srv_->events->mark();
  fire_row("row_activated"_key, 0);
  auto ev = srv_->events->wait_for("on_navigate"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "docs");

  show("docs");
  since = srv_->events->mark();
  fire(".main.toolbar.btn_up", "clicked"_key);
  ev = srv_->events->wait_for("on_navigate"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "");
}

TEST_F(DuTest, UpAtTheScannedRootOnlyExplains) {
  show();
  size_t since = srv_->events->mark();
  fire(".main.toolbar.btn_up", "clicked"_key);
  EXPECT_NE(text_of(".main.status").find("Already at the scanned folder"), std::string::npos);
  EXPECT_FALSE(srv_->events->saw("on_navigate"_key, since));
}

TEST_F(DuTest, ActivatingAFileRowSelectsInsteadOfNavigating) {
  show();
  size_t since = srv_->events->mark();
  fire_row("row_activated"_key, 1); // big.bin
  EXPECT_EQ(selected_row(), "big.bin");
  EXPECT_FALSE(srv_->events->saw("on_navigate"_key, since));
}

// ── Selection <-> treemap ─────────────────────────────────────────────────────

TEST_F(DuTest, SelectingARowOutlinesItsTreemapNode) {
  show();
  fire_row("row_selected"_key, 0); // docs -> node 1
  EXPECT_EQ(selected_row(), "docs");
  EXPECT_EQ(treemap()->as<int32_t>("selected"_key), 1);
  EXPECT_EQ(text_of(".main.status"), "/home/u/proj/docs  -  5.9 KB");

  fire_row("row_selected"_key, 1); // big.bin -> node 2
  EXPECT_EQ(treemap()->as<int32_t>("selected"_key), 2);
}

TEST_F(DuTest, ClickingADeepTreemapNodeSelectsItsTopLevelRow) {
  show();
  fire_treemap("clicked"_key, 5); // c.md, inside docs/deep
  EXPECT_EQ(selected_row(), "docs");
  // The outline stays on the clicked node itself, not its top-level folder.
  EXPECT_EQ(treemap()->as<int32_t>("selected"_key), 5);
  EXPECT_EQ(text_of(".main.status"), "/home/u/proj/docs/deep/c.md  -  2.0 KB");
}

TEST_F(DuTest, DoubleClickingATreemapNodeOpensItsTopLevelFolder) {
  show();
  size_t since = srv_->events->mark();
  fire_treemap("activated"_key, 5);
  auto ev = srv_->events->wait_for("on_navigate"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "docs");
}

TEST_F(DuTest, TreemapClicksOnTheRootOrOutOfRangeAreIgnored) {
  show();
  size_t since = srv_->events->mark();
  fire_treemap("clicked"_key, 0);
  fire_treemap("activated"_key, 99);
  fire_treemap("clicked"_key, -1);
  EXPECT_EQ(selected_row(), "");
  EXPECT_EQ(treemap()->as<int32_t>("selected"_key), -1);
  EXPECT_FALSE(srv_->events->saw("on_navigate"_key, since));
}

// ── Closing ───────────────────────────────────────────────────────────────────

TEST_F(DuTest, ClosingEitherPanelClosesTheTool) {
  size_t since = srv_->events->mark();
  handler_->on_event(
      srv_->last_session->ui_objects.at(root_ + "_treemap")->as<bison::key_t>("__wish_id"_key), "closed"_key, dynamic{});
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since));
}
