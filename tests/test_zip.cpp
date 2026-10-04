// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <standalone/standalone.hpp>
#include <ui/ui_root.hpp>
#include <web/web_renderer.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
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

struct fake_entry {
  std::string name;
  std::string type; ///< "file" or "dir"
  std::string size;
  std::string modified;
};

// Builds the {path, files: [{name, type, size, modified}, ...]} shape that
// Zip::do_update_listing() expects -- matches the reference client
// (modules/bdg/desktop/zip/client/zip.cpp)'s report_listing().
dynamic make_listing_args(const std::string& path, const std::vector<fake_entry>& entries) {
  dynamic args;
  args["path"_key] = path;

  dynamic files;
  size_t i = 0;
  for (auto& e : entries) {
    auto ep = std::make_shared<dynamic>();
    (*ep)["name"_key] = e.name;
    (*ep)["type"_key] = e.type;
    (*ep)["size"_key] = e.size;
    (*ep)["modified"_key] = e.modified;
    files[i++] = dynamic_ptr{ep};
  }
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};
  return args;
}

struct fake_archive_entry {
  std::string name;
  std::string type; ///< "file" or "dir"
  int32_t uncompressed_size = 0;
  int32_t compressed_size = 0;
};

// Builds the {name, entries: [{name, type, uncompressed_size, compressed_size}]}
// shape Zip::do_show_contents() expects.
dynamic make_contents_args(const std::string& name, const std::vector<fake_archive_entry>& entries) {
  dynamic args;
  args["name"_key] = name;

  dynamic list;
  size_t i = 0;
  for (auto& e : entries) {
    auto ep = std::make_shared<dynamic>();
    (*ep)["name"_key] = e.name;
    (*ep)["type"_key] = e.type;
    (*ep)["uncompressed_size"_key] = e.uncompressed_size;
    (*ep)["compressed_size"_key] = e.compressed_size;
    list[i++] = dynamic_ptr{ep};
  }
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(list))};
  return args;
}

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class ZipLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(ZipLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Zip"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Zip"_key);
}

TEST_F(ZipLocalTest, DefaultTitle) {
  auto obj = dynamic::instantiate("wish"_key, "Zip"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "Zip");
}

TEST_F(ZipLocalTest, DefaultStatus) {
  auto obj = dynamic::instantiate("wish"_key, "Zip"_key);
  auto* f = obj.findField("status"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "Ready.");
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

// Helper: find the root key for the internal form tree -- "__zip_<N>"
// exactly (the Files panel), not a child path ("__zip_0.main..."), a
// secondary panel root ("__zip_0_contents", "__zip_0_actions") nor the
// prompt dialog root ("__zip_prompt_<N>").
static std::string find_form_root(const wish::name_map& objects) {
  static const std::string prefix = "__zip_";
  for (const auto& [k, _] : objects) {
    if (k.size() > prefix.size() && k.rfind(prefix, 0) == 0 &&
        std::all_of(k.begin() + prefix.size(), k.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
      return k;
  }
  return {};
}

static std::string find_root_with_prefix(const wish::name_map& objects, const std::string& prefix) {
  for (const auto& [k, _] : objects) {
    if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class ZipWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "Zip"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(ZipWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __zip_... root key in session.objects";
}

TEST_F(ZipWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(ZipWindowTest, TreeContainsBrowserAndButtons) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.path_input"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.file_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + "_actions.vbox.btn_row.btn_compress"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + "_actions.vbox.btn_row.btn_extract"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + "_actions.vbox.btn_row.btn_view"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + "_actions.vbox.btn_row.btn_refresh"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + "_actions.vbox.status"));
  // Progress lives in the client's shared ProgressBox, not in this form.
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root + "_actions.vbox.progress_bar"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + "_contents.vbox.contents_table"));
}

// Each panel is its own top-level Window, addressable by "__path__" so the
// dock layout can name it.
TEST_F(ZipWindowTest, EachPanelIsATopLevelWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& s = *srv_->last_session;
  for (const std::string key : {root, root + "_contents", root + "_actions"}) {
    auto it = s.top_level_objects.find(bison::key_t{key});
    ASSERT_NE(it, s.top_level_objects.end()) << key;
    EXPECT_EQ(it->second->as<bison::key_t>(dynamic::CLASS), "Window"_key) << key;
    EXPECT_EQ(it->second->as<std::string>("__path__"_key), key) << key;
  }
  EXPECT_EQ(s.ui_objects.at(root)->as<std::string>("title"_key), "Files");
  EXPECT_EQ(s.ui_objects.at(root + "_contents")->as<std::string>("title"_key), "Contents");
  EXPECT_EQ(s.ui_objects.at(root + "_actions")->as<std::string>("title"_key), "Actions");
}

// The panels are seeded into a nested DockSpaceViewport ("zip_dock") whose
// DockLayout names every panel's path.
TEST_F(ZipWindowTest, RegistersDefaultDockLayoutNamingEveryPanel) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());

  wish::ui_element_ptr viewport;
  for (const auto& [k, obj] : srv_->last_session->top_level_objects)
    if (obj->as<bison::key_t>(dynamic::CLASS) == "DockSpaceViewport"_key)
      viewport = obj;
  ASSERT_TRUE(viewport);
  EXPECT_EQ(viewport->as<std::string>("id"_key), "zip_dock");
  EXPECT_EQ(viewport->as<std::string>("title"_key), "Zip");

  // Collect every DockArea's newline-separated "windows" list.
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
  std::vector<std::string> expected{root, root + "_actions", root + "_contents"};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(windows, expected);
}

// The Actions strip is seeded along the top: it is the first child of an
// "up" DockSplit.
TEST_F(ZipWindowTest, DefaultDockLayoutPutsActionsAtTheTop) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());

  wish::ui_element_ptr viewport;
  for (const auto& [k, obj] : srv_->last_session->top_level_objects)
    if (obj->as<bison::key_t>(dynamic::CLASS) == "DockSpaceViewport"_key)
      viewport = obj;
  ASSERT_TRUE(viewport);

  std::string actions_dir;
  std::function<void(const dynamic&)> walk = [&](const dynamic& node) {
    const bool is_split = node.as<bison::key_t>(dynamic::CLASS) == "DockSplit"_key;
    bool first = true;
    if (auto* cf = node.findField<dynamic_ptr>("children"_key); cf && *cf)
      (*cf)->forEach([&](bison::key_t, const field& f) {
        if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
          return;
        const dynamic& child = *f.as<dynamic_ptr>();
        if (is_split && first && child.as<bison::key_t>(dynamic::CLASS) == "DockArea"_key &&
            child.as<std::string>("windows"_key) == root + "_actions")
          actions_dir = node.as<std::string>("dir"_key);
        first = false;
        walk(child);
      });
  };
  walk(*viewport);
  EXPECT_EQ(actions_dir, "up");
}

TEST_F(ZipWindowTest, TableStartsEmptyUntilClientReportsAListing) {
  // Unlike mc's sandbox panel, this form has no filesystem of its
  // own -- the table must not be pre-populated in on_init().
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto it = srv_->last_session->ui_objects.find(root + ".main.file_table");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());
  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  EXPECT_EQ((*cf)->size(), 0u);
}

// ── RMI methods + on_set() ─────────────────────────────────────────────────────

class ZipRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Zip"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  dynamic update_listing(const std::string& path, const std::vector<fake_entry>& entries) {
    return proxy_->call("update_listing"_key, make_listing_args(path, entries)).get();
  }

  size_t table_row_count(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

TEST_F(ZipRmiTest, UpdateListingPopulatesTable) {
  update_listing(
      "/home/user",
      {{"docs", "dir", "", "2026-01-01 00:00"}, {"notes.txt", "file", "1.2 KB", "2026-01-02 00:00"}});

  EXPECT_EQ(table_row_count(root_ + ".main.file_table"), 2u);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.path_input")->as<std::string>("value"_key), "/home/user");
  EXPECT_EQ(srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key), "Ready.");
}

TEST_F(ZipRmiTest, UpdateListingReplacesPreviousEntries) {
  update_listing("/a", {{"one.txt", "file", "1 B", ""}});
  ASSERT_EQ(table_row_count(root_ + ".main.file_table"), 1u);

  update_listing("/b", {{"two.txt", "file", "2 B", ""}, {"three.txt", "file", "3 B", ""}});
  EXPECT_EQ(table_row_count(root_ + ".main.file_table"), 2u);
}

TEST_F(ZipRmiTest, SetStatusMirrorsToStatusLabel) {
  dynamic patch;
  patch["status"_key] = std::string{"Compressing..."};
  proxy_->set(std::move(patch)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key), "Compressing...");
}

TEST_F(ZipRmiTest, ShowContentsFillsContentsPanel) {
  auto result = proxy_->call(
                         "show_contents"_key,
                         make_contents_args(
                             "photos.zip",
                             {{"photos/", "dir", 0, 0},
                              {"photos/a.png", "file", 2000, 1000},
                              {"photos/b.png", "file", 500, 500}}))
                    .get();
  (void)result;

  const std::string contents_root = root_ + "_contents";
  EXPECT_EQ(table_row_count(contents_root + ".vbox.contents_table"), 3u);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(contents_root + ".vbox.summary")->as<std::string>("text"_key),
      "photos.zip: 3 entries, 2.4 KB uncompressed / 1.5 KB compressed");
  // No separate dialog is opened: the panel is filled in place.
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__zip_contents_").empty());
}

TEST_F(ZipRmiTest, ShowContentsReplacesPreviousArchiveRows) {
  proxy_->call("show_contents"_key, make_contents_args("a.zip", {{"x", "file", 1, 1}, {"y", "file", 1, 1}})).get();
  proxy_->call("show_contents"_key, make_contents_args("b.zip", {{"z", "file", 1, 1}})).get();
  EXPECT_EQ(table_row_count(root_ + "_contents.vbox.contents_table"), 1u);
  // The named TableColumn children survive the row replacement.
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_contents.vbox.contents_table.col_ratio"));
}

// ── Event routing ─────────────────────────────────────────────────────────────

class ZipEventTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Zip"_key).get());
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

  bison::key_t widget_id(const std::string& path) const {
    return srv_->last_session->ui_objects.at(root_ + path)->as<bison::key_t>("__wish_id"_key);
  }

  bison::key_t widget_id_at(const std::string& full_path) const {
    return srv_->last_session->ui_objects.at(full_path)->as<bison::key_t>("__wish_id"_key);
  }

  void update_listing(const std::string& path, const std::vector<fake_entry>& entries) {
    proxy_->call("update_listing"_key, make_listing_args(path, entries)).get();
  }

  void select_row(int32_t index) {
    dynamic sel;
    sel["index"_key] = index;
    handler_->on_event(widget_id(".main.file_table"), "row_selected"_key, sel);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  wish::ui_root* handler_{nullptr};
};

TEST_F(ZipEventTest, PathBarChangedEmitsOnNavigateWithTypePath) {
  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  dynamic payload;
  payload["value"_key] = std::string{"/some/local/dir"};
  handler_->on_event(widget_id(".main.path_input"), "changed"_key, payload);

  if (auto ev = srv_->events->wait_for("on_navigate"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "/some/local/dir");
  EXPECT_EQ(captured.as<std::string>("type"_key), "path");
}

TEST_F(ZipEventTest, RowActivatedOnDirEmitsOnNavigateWithTypeDir) {
  update_listing("/home", {{"sub", "dir", "", ""}});

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.file_table"), "row_activated"_key, payload);

  if (auto ev = srv_->events->wait_for("on_navigate"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "sub");
  EXPECT_EQ(captured.as<std::string>("type"_key), "dir");
}

TEST_F(ZipEventTest, RowActivatedOnZipFileEmitsOnViewContentsRequested) {
  update_listing("/home", {{"photos.zip", "file", "1 KB", ""}});

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.file_table"), "row_activated"_key, payload);

  if (auto ev = srv_->events->wait_for("on_view_contents_requested"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("name"_key), "photos.zip");
}

TEST_F(ZipEventTest, RowActivatedOnNonArchiveFileSetsStatusInsteadOfEmitting) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});

  bool got = false;
  size_t since = srv_->events->mark();

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.file_table"), "row_activated"_key, payload);

  got = srv_->events->wait_for("on_view_contents_requested"_key, since).has_value();
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key),
      "Not an archive: notes.txt");
}

TEST_F(ZipEventTest, CompressClickedWithNoSelectionSetsStatus) {
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key),
      "Select one or more files/folders to compress.");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_").empty());
}

TEST_F(ZipEventTest, CompressClickedWithSelectionShowsPromptWithDefaultArchiveName) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  ASSERT_FALSE(prompt_root.empty());
  EXPECT_EQ(srv_->last_session->ui_objects.at(prompt_root)->as<std::string>("title"_key), "Compress");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(prompt_root + ".name_input")->as<std::string>("value"_key),
      "notes.txt.zip");
  EXPECT_EQ(srv_->last_session->ui_objects.at(prompt_root + ".buttons.btn_ok")->as<std::string>("label"_key), "Create");
}

TEST_F(ZipEventTest, ConfirmingCompressPromptEmitsOnCompressRequested) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  ASSERT_FALSE(prompt_root.empty());
  auto ok_id = widget_id_at(prompt_root + ".buttons.btn_ok");

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  handler_->on_event(ok_id, "clicked"_key, dynamic{});

  if (auto ev = srv_->events->wait_for("on_compress_requested"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  auto* names_f = captured.findField<dynamic_ptr>("source_names"_key);
  ASSERT_NE(names_f, nullptr);
  ASSERT_TRUE(*names_f);
  ASSERT_EQ((*names_f)->size(), 1u);
  EXPECT_EQ((*names_f)->at(size_t{0}).as<std::string>(), "notes.txt");
  EXPECT_EQ(captured.as<std::string>("archive_name"_key), "notes.txt.zip");
  EXPECT_EQ(srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key), "Compressing...");
}

TEST_F(ZipEventTest, CompressNameCollidingWithExistingEntryShowsOverwriteConfirmInsteadOfEmitting) {
  // "archive.zip" already exists in the cached listing. Entries are sorted
  // ascending by name by default, so "archive.zip" lands at index 0 and
  // "notes.txt" (the file being compressed) at index 1.
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}, {"archive.zip", "file", "1 KB", ""}});
  select_row(1);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  ASSERT_FALSE(prompt_root.empty());

  dynamic changed;
  changed["value"_key] = std::string{"archive.zip"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);

  bool got = false;
  size_t since = srv_->events->mark();

  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  got = srv_->events->wait_for("on_compress_requested"_key, since).has_value();
  EXPECT_FALSE(got) << "compress should be held back pending overwrite confirmation";

  // Overwrite-confirm is a privately-instantiated MessageBox (see
  // form::instantiate_child_form()) -- "__message_box_..." is that form
  // class's own next_available_key() prefix.
  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{confirm_root}));
}

TEST_F(ZipEventTest, ConfirmOverwriteYesEmitsOnCompressRequested) {
  // Entries are sorted ascending by name by default, so "archive.zip" lands
  // at index 0 and "notes.txt" (the file being compressed) at index 1.
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}, {"archive.zip", "file", "1 KB", ""}});
  select_row(1);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  dynamic changed;
  changed["value"_key] = std::string{"archive.zip"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  // Overwrite-confirm is a privately-instantiated MessageBox (see
  // form::instantiate_child_form()), with a "yes_no" preset ("buttons.btn0"/
  // "btn1" for Yes/No -- see message_box.cpp's kLayoutYesNo).
  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id = widget_id_at(confirm_root + ".buttons.btn0");

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  // The Yes button belongs to the confirm MessageBox's own internal tree,
  // handled by ITS OWN on_event() (top_level_handlers[confirm_root]) -- not
  // zip's -- so this must be looked up separately rather than routed
  // through handler_.
  auto confirm_handler = srv_->last_session->top_level_handlers.find(confirm_root)->second;
  ASSERT_NE(confirm_handler, nullptr);
  confirm_handler->on_event(yes_id, "clicked"_key, dynamic{});

  if (auto ev = srv_->events->wait_for("on_compress_requested"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("archive_name"_key), "archive.zip");
}

TEST_F(ZipEventTest, ConfirmOverwriteNoCancelsWithoutEmitting) {
  // Entries are sorted ascending by name by default, so "archive.zip" lands
  // at index 0 and "notes.txt" (the file being compressed) at index 1.
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}, {"archive.zip", "file", "1 KB", ""}});
  select_row(1);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  dynamic changed;
  changed["value"_key] = std::string{"archive.zip"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto no_id = widget_id_at(confirm_root + ".buttons.btn1");

  bool got = false;
  size_t since = srv_->events->mark();

  // See ConfirmOverwriteYesEmitsOnCompressRequested above for why this goes
  // through the confirm MessageBox's own handler, not handler_.
  auto confirm_handler = srv_->last_session->top_level_handlers.find(confirm_root)->second;
  ASSERT_NE(confirm_handler, nullptr);
  confirm_handler->on_event(no_id, "clicked"_key, dynamic{});

  got = srv_->events->wait_for("on_compress_requested"_key, since).has_value();
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key), "Compress cancelled.");
}

TEST_F(ZipEventTest, CompressPromptRejectsNameEqualToSource) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_compress"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");

  dynamic changed;
  changed["value"_key] = std::string{"notes.txt"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key),
      "Archive name must differ from the source.");
  // The prompt should remain open so the user can fix the name.
  EXPECT_TRUE(srv_->last_session->ui_objects.count(prompt_root));
}

TEST_F(ZipEventTest, ExtractClickedRequiresAZipFileSelection) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_extract"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key),
      "Select a .zip file to extract.");
}

TEST_F(ZipEventTest, ExtractClickedWithZipSelectionShowsPromptWithStrippedDestName) {
  update_listing("/home", {{"Photos.ZIP", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_extract"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  ASSERT_FALSE(prompt_root.empty());
  EXPECT_EQ(srv_->last_session->ui_objects.at(prompt_root)->as<std::string>("title"_key), "Extract");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(prompt_root + ".name_input")->as<std::string>("value"_key), "Photos");
}

TEST_F(ZipEventTest, ConfirmingExtractPromptEmitsOnExtractRequested) {
  update_listing("/home", {{"archive.zip", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_extract"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");
  ASSERT_FALSE(prompt_root.empty());

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  if (auto ev = srv_->events->wait_for("on_extract_requested"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("zip_name"_key), "archive.zip");
  EXPECT_EQ(captured.as<std::string>("dest_name"_key), "archive");
}

TEST_F(ZipEventTest, ExtractDestCollidingWithExistingFileIsRejected) {
  // "out" already exists as a *file* (not a folder) -- extract can't merge
  // into that, unlike the directory-collision case which asks to overwrite.
  update_listing("/home", {{"archive.zip", "file", "1 KB", ""}, {"out", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_extract"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__zip_prompt_");

  dynamic changed;
  changed["value"_key] = std::string{"out"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key),
      "A file with that name already exists.");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}

TEST_F(ZipEventTest, ViewContentsClickedRequiresAZipFileSelection) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_view"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + "_actions.vbox.status")->as<std::string>("text"_key),
      "Select a .zip file to view its contents.");
}

TEST_F(ZipEventTest, ViewContentsClickedEmitsOnViewContentsRequested) {
  update_listing("/home", {{"archive.zip", "file", "1 KB", ""}});
  select_row(0);

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_view"), "clicked"_key, dynamic{});

  if (auto ev = srv_->events->wait_for("on_view_contents_requested"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("name"_key), "archive.zip");
}

TEST_F(ZipEventTest, RefreshClickedEmitsOnNavigateWithCurrentPath) {
  update_listing("/home/user", {});

  bool got = false;
  dynamic captured;
  size_t since = srv_->events->mark();

  handler_->on_event(widget_id("_actions.vbox.btn_row.btn_refresh"), "clicked"_key, dynamic{});

  if (auto ev = srv_->events->wait_for("on_navigate"_key, since)) {
    got = true;
    captured = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "/home/user");
  EXPECT_EQ(captured.as<std::string>("type"_key), "path");
}

TEST_F(ZipEventTest, RowSelectedUpdatesSelectedLabel) {
  update_listing("/home", {{"a.txt", "file", "1 B", ""}});
  select_row(0);

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.selected_label")->as<std::string>("text"_key),
      "Selected: a.txt");
}

TEST_F(ZipEventTest, TableSortedEventSortsRowsDescendingByName) {
  update_listing("/home", {{"zebra.txt", "file", "1 B", ""}, {"apple.txt", "file", "1 B", ""}});

  auto name_cell_text = [&](size_t row_idx) -> std::string {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".main.file_table");
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    auto& row = *(*cf)->at(row_idx).as<dynamic_ptr>();
    auto* rcf = row.findField<dynamic_ptr>("children"_key);
    auto& name_cell = *(*rcf)->at(size_t{0}).as<dynamic_ptr>();
    auto* ncf = name_cell.findField<dynamic_ptr>("children"_key);
    return (*ncf)->at(size_t{1}).as<dynamic_ptr>()->as<std::string>("text"_key);
  };

  ASSERT_EQ(name_cell_text(0), "apple.txt");

  dynamic sort_payload;
  sort_payload["column_id"_key] = int32_t{0};
  sort_payload["ascending"_key] = false;
  handler_->on_event(widget_id(".main.file_table"), "sorted"_key, sort_payload);

  EXPECT_EQ(name_cell_text(0), "zebra.txt");
  EXPECT_EQ(name_cell_text(1), "apple.txt");
}

TEST_F(ZipEventTest, WindowClosedEmitsClosedAndCleansUp) {
  bool got_closed = false;
  size_t since = srv_->events->mark();

  handler_->on_event(widget_id(""), "closed"_key, dynamic{});

  got_closed = srv_->events->wait_for("closed"_key, since).has_value();
  EXPECT_TRUE(got_closed);
  for (const std::string key : {root_, root_ + "_contents", root_ + "_actions"})
    EXPECT_EQ(srv_->last_session->ui_objects.count(key), 0u) << key;
}

// Closing a secondary panel (here: Actions) tears down the whole tool too.
TEST_F(ZipEventTest, SecondaryPanelClosedEmitsClosedAndRemovesEveryPanel) {
  size_t since = srv_->events->mark();
  const std::string actions_root = root_ + "_actions";
  auto h = srv_->last_session->top_level_handlers.find(bison::key_t{actions_root});
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(widget_id_at(actions_root), "closed"_key, dynamic{});

  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since).has_value());
  for (const std::string key : {root_, root_ + "_contents", actions_root})
    EXPECT_EQ(srv_->last_session->ui_objects.count(key), 0u) << key;
}

// ── Standalone dispatch: repeated RMI calls must not hang ─────────────────────

TEST(ZipStandaloneTest, RepeatedUpdateListingCallsDoNotHangOrFail) {
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto tool = sa.instantiate("wish"_key, "Zip"_key).get();
  ASSERT_TRUE(tool.valid());

  auto call_ready = [&](const char* label) {
    auto fut = tool.call("update_listing"_key, make_listing_args("/tmp", {{"a.txt", "file", "1 B", ""}}));
    auto status = fut.wait_for(std::chrono::seconds(3));
    EXPECT_EQ(status, std::future_status::ready) << label << " did not complete within 3s";
    if (status == std::future_status::ready)
      EXPECT_NO_THROW(fut.get()) << label << " threw";
  };

  for (int i = 0; i < 10; ++i)
    call_ready("update_listing");

  sa.stop();
}

TEST(ZipStandaloneTest, RepeatedUpdateListingCallsDoNotHangOrFailUnderWebRenderer) {
  wish::standalone sa{std::make_unique<wish::web_renderer>("127.0.0.1", 0, 16)};
  sa.start();

  auto tool = sa.instantiate("wish"_key, "Zip"_key).get();
  ASSERT_TRUE(tool.valid());

  auto call_ready = [&](const char* label) {
    auto fut = tool.call("update_listing"_key, make_listing_args("/tmp", {{"a.txt", "file", "1 B", ""}}));
    auto status = fut.wait_for(std::chrono::seconds(3));
    EXPECT_EQ(status, std::future_status::ready) << label << " did not complete within 3s";
    if (status == std::future_status::ready)
      EXPECT_NO_THROW(fut.get()) << label << " threw";
  };

  for (int i = 0; i < 20; ++i)
    call_ready("update_listing");

  sa.stop();
}
