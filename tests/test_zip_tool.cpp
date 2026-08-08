// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <standalone/standalone.hpp>
#include <ui/ui_root.hpp>
#include <web/web_renderer.hpp>

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

namespace {

struct fake_entry {
  std::string name;
  std::string type; ///< "file" or "dir"
  std::string size;
  std::string modified;
};

// Builds the {path, files: [{name, type, size, modified}, ...]} shape that
// ZipTool::do_update_listing() expects -- matches the reference client
// (modules/bdg/desktop/zip_tool/client/zip_tool.cpp)'s report_listing().
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
// shape ZipTool::do_show_contents() expects.
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

class ZipToolLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(ZipToolLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "ZipTool"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "ZipTool"_key);
}

TEST_F(ZipToolLocalTest, DefaultTitle) {
  auto obj = dynamic::instantiate("wish"_key, "ZipTool"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "Zip Tool");
}

TEST_F(ZipToolLocalTest, DefaultStatus) {
  auto obj = dynamic::instantiate("wish"_key, "ZipTool"_key);
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

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

// Helper: find the root key for the internal form tree (starts with
// "__ziptool_", no dot -- i.e. it is the top-level entry not a child path).
// Excludes the prompt/confirm/contents sub-dialog roots, which use their own
// "__ziptool_prompt_"/"__ziptool_confirm_"/"__ziptool_contents_" prefixes.
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__ziptool_", 0) == 0 && k.find('.') == std::string::npos && k.find("_prompt_") == std::string::npos &&
        k.find("_confirm_") == std::string::npos && k.find("_contents_") == std::string::npos)
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

class ZipToolWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "ZipTool"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(ZipToolWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __ziptool_... root key in session.objects";
}

TEST_F(ZipToolWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(ZipToolWindowTest, TreeContainsBrowserAndButtons) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.path_input"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.file_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.btn_row.btn_compress"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.btn_row.btn_extract"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.btn_row.btn_view"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.btn_row.btn_refresh"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.status"));
}

TEST_F(ZipToolWindowTest, TableStartsEmptyUntilClientReportsAListing) {
  // Unlike FileExplorer's sandbox panel, this form has no filesystem of its
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

class ZipToolRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "ZipTool"_key).get());
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

TEST_F(ZipToolRmiTest, UpdateListingPopulatesTable) {
  update_listing(
      "/home/user",
      {{"docs", "dir", "", "2026-01-01 00:00"}, {"notes.txt", "file", "1.2 KB", "2026-01-02 00:00"}});

  EXPECT_EQ(table_row_count(root_ + ".main.file_table"), 2u);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.path_input")->as<std::string>("value"_key), "/home/user");
  EXPECT_EQ(srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Ready.");
}

TEST_F(ZipToolRmiTest, UpdateListingReplacesPreviousEntries) {
  update_listing("/a", {{"one.txt", "file", "1 B", ""}});
  ASSERT_EQ(table_row_count(root_ + ".main.file_table"), 1u);

  update_listing("/b", {{"two.txt", "file", "2 B", ""}, {"three.txt", "file", "3 B", ""}});
  EXPECT_EQ(table_row_count(root_ + ".main.file_table"), 2u);
}

TEST_F(ZipToolRmiTest, SetStatusMirrorsToStatusLabel) {
  dynamic patch;
  patch["status"_key] = std::string{"Compressing..."};
  proxy_->set(std::move(patch)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Compressing...");
}

TEST_F(ZipToolRmiTest, ShowContentsBuildsContentsDialogWithRows) {
  auto result = proxy_->call(
                         "show_contents"_key,
                         make_contents_args(
                             "photos.zip",
                             {{"photos/", "dir", 0, 0},
                              {"photos/a.png", "file", 2000, 1000},
                              {"photos/b.png", "file", 500, 500}}))
                    .get();
  (void)result;

  std::string contents_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_contents_");
  ASSERT_FALSE(contents_root.empty());
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{contents_root}));
  EXPECT_EQ(table_row_count(contents_root + ".vbox.contents_table"), 3u);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(contents_root)->as<std::string>("title"_key), "Contents: photos.zip");
}

// ── Event routing ─────────────────────────────────────────────────────────────

class ZipToolEventTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "ZipTool"_key).get());
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

  // form::emit() defers delivery to the render loop's next frame, so spin
  // briefly for it, same idiom as test_file_explorer.cpp.
  void wait_for(bool& flag) const {
    auto t0 = std::chrono::steady_clock::now();
    while (!flag && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  wish::ui_root* handler_{nullptr};
};

TEST_F(ZipToolEventTest, PathBarChangedEmitsOnNavigateWithTypePath) {
  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_navigate"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic payload;
  payload["value"_key] = std::string{"/some/local/dir"};
  handler_->on_event(widget_id(".main.path_input"), "changed"_key, payload);

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "/some/local/dir");
  EXPECT_EQ(captured.as<std::string>("type"_key), "path");
}

TEST_F(ZipToolEventTest, RowActivatedOnDirEmitsOnNavigateWithTypeDir) {
  update_listing("/home", {{"sub", "dir", "", ""}});

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_navigate"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.file_table"), "row_activated"_key, payload);

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "sub");
  EXPECT_EQ(captured.as<std::string>("type"_key), "dir");
}

TEST_F(ZipToolEventTest, RowActivatedOnZipFileEmitsOnViewContentsRequested) {
  update_listing("/home", {{"photos.zip", "file", "1 KB", ""}});

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_view_contents_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.file_table"), "row_activated"_key, payload);

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("name"_key), "photos.zip");
}

TEST_F(ZipToolEventTest, RowActivatedOnNonArchiveFileSetsStatusInsteadOfEmitting) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_view_contents_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.file_table"), "row_activated"_key, payload);

  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Not an archive: notes.txt");
}

TEST_F(ZipToolEventTest, CompressClickedWithNoSelectionSetsStatus) {
  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Select a file or folder to compress.");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_").empty());
}

TEST_F(ZipToolEventTest, CompressClickedWithSelectionShowsPromptWithDefaultArchiveName) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  ASSERT_FALSE(prompt_root.empty());
  EXPECT_EQ(srv_->last_session->ui_objects.at(prompt_root)->as<std::string>("title"_key), "Compress");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(prompt_root + ".name_input")->as<std::string>("value"_key),
      "notes.txt.zip");
  EXPECT_EQ(srv_->last_session->ui_objects.at(prompt_root + ".buttons.btn_ok")->as<std::string>("label"_key), "Create");
}

TEST_F(ZipToolEventTest, ConfirmingCompressPromptEmitsOnCompressRequested) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  ASSERT_FALSE(prompt_root.empty());
  auto ok_id = widget_id_at(prompt_root + ".buttons.btn_ok");

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_compress_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(ok_id, "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("source_name"_key), "notes.txt");
  EXPECT_EQ(captured.as<std::string>("archive_name"_key), "notes.txt.zip");
  EXPECT_EQ(srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Compressing...");
}

TEST_F(ZipToolEventTest, CompressNameCollidingWithExistingEntryShowsOverwriteConfirmInsteadOfEmitting) {
  // "archive.zip" already exists in the cached listing.
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}, {"archive.zip", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  ASSERT_FALSE(prompt_root.empty());

  dynamic changed;
  changed["value"_key] = std::string{"archive.zip"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_compress_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  wait_for(got);
  EXPECT_FALSE(got) << "compress should be held back pending overwrite confirmation";

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_confirm_");
  ASSERT_FALSE(confirm_root.empty());
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{confirm_root}));
}

TEST_F(ZipToolEventTest, ConfirmOverwriteYesEmitsOnCompressRequested) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}, {"archive.zip", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  dynamic changed;
  changed["value"_key] = std::string{"archive.zip"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_confirm_");
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id = widget_id_at(confirm_root + ".buttons.btn_yes");

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_compress_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(yes_id, "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("archive_name"_key), "archive.zip");
}

TEST_F(ZipToolEventTest, ConfirmOverwriteNoCancelsWithoutEmitting) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}, {"archive.zip", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  dynamic changed;
  changed["value"_key] = std::string{"archive.zip"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_confirm_");
  ASSERT_FALSE(confirm_root.empty());
  auto no_id = widget_id_at(confirm_root + ".buttons.btn_no");

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_compress_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(no_id, "clicked"_key, dynamic{});

  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Compress cancelled.");
}

TEST_F(ZipToolEventTest, CompressPromptRejectsNameEqualToSource) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_compress"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");

  dynamic changed;
  changed["value"_key] = std::string{"notes.txt"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Archive name must differ from the source.");
  // The prompt should remain open so the user can fix the name.
  EXPECT_TRUE(srv_->last_session->ui_objects.count(prompt_root));
}

TEST_F(ZipToolEventTest, ExtractClickedRequiresAZipFileSelection) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id(".main.btn_row.btn_extract"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Select a .zip file to extract.");
}

TEST_F(ZipToolEventTest, ExtractClickedWithZipSelectionShowsPromptWithStrippedDestName) {
  update_listing("/home", {{"Photos.ZIP", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id(".main.btn_row.btn_extract"), "clicked"_key, dynamic{});

  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  ASSERT_FALSE(prompt_root.empty());
  EXPECT_EQ(srv_->last_session->ui_objects.at(prompt_root)->as<std::string>("title"_key), "Extract");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(prompt_root + ".name_input")->as<std::string>("value"_key), "Photos");
}

TEST_F(ZipToolEventTest, ConfirmingExtractPromptEmitsOnExtractRequested) {
  update_listing("/home", {{"archive.zip", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_extract"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");
  ASSERT_FALSE(prompt_root.empty());

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_extract_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("zip_name"_key), "archive.zip");
  EXPECT_EQ(captured.as<std::string>("dest_name"_key), "archive");
}

TEST_F(ZipToolEventTest, ExtractDestCollidingWithExistingFileIsRejected) {
  // "out" already exists as a *file* (not a folder) -- extract can't merge
  // into that, unlike the directory-collision case which asks to overwrite.
  update_listing("/home", {{"archive.zip", "file", "1 KB", ""}, {"out", "file", "1 KB", ""}});
  select_row(0);
  handler_->on_event(widget_id(".main.btn_row.btn_extract"), "clicked"_key, dynamic{});
  std::string prompt_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_prompt_");

  dynamic changed;
  changed["value"_key] = std::string{"out"};
  handler_->on_event(widget_id_at(prompt_root + ".name_input"), "changed"_key, changed);
  handler_->on_event(widget_id_at(prompt_root + ".buttons.btn_ok"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "A file with that name already exists.");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_confirm_").empty());
}

TEST_F(ZipToolEventTest, ViewContentsClickedRequiresAZipFileSelection) {
  update_listing("/home", {{"notes.txt", "file", "1 KB", ""}});
  select_row(0);

  handler_->on_event(widget_id(".main.btn_row.btn_view"), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Select a .zip file to view its contents.");
}

TEST_F(ZipToolEventTest, ViewContentsClickedEmitsOnViewContentsRequested) {
  update_listing("/home", {{"archive.zip", "file", "1 KB", ""}});
  select_row(0);

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_view_contents_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.btn_row.btn_view"), "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("path"_key), "/home");
  EXPECT_EQ(captured.as<std::string>("name"_key), "archive.zip");
}

TEST_F(ZipToolEventTest, RefreshClickedEmitsOnNavigateWithCurrentPath) {
  update_listing("/home/user", {});

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_navigate"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.btn_row.btn_refresh"), "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "/home/user");
  EXPECT_EQ(captured.as<std::string>("type"_key), "path");
}

TEST_F(ZipToolEventTest, RowSelectedUpdatesSelectedLabel) {
  update_listing("/home", {{"a.txt", "file", "1 B", ""}});
  select_row(0);

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.selected_label")->as<std::string>("text"_key),
      "Selected: a.txt");
}

TEST_F(ZipToolEventTest, TableSortedEventSortsRowsDescendingByName) {
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

TEST_F(ZipToolEventTest, ContentsCloseButtonRequestsClose) {
  proxy_->call(
           "show_contents"_key, make_contents_args("archive.zip", {{"a.txt", "file", 10, 5}}))
      .get();
  std::string contents_root = find_root_with_prefix(srv_->last_session->ui_objects, "__ziptool_contents_");
  ASSERT_FALSE(contents_root.empty());
  auto close_id = widget_id_at(contents_root + ".vbox.btn_row.btn_close");

  // Clicking Close only requests the ImGui popup close; the actual removal
  // is driven by the Window's own "closed" event once the render loop
  // confirms it -- see form::request_close_at()'s doc comment. Simulate
  // that confirmation directly here, mirroring test_file_dialog.cpp's idiom.
  handler_->on_event(close_id, "clicked"_key, dynamic{});
  auto window_id = widget_id_at(contents_root);
  handler_->on_event(window_id, "closed"_key, dynamic{});

  EXPECT_EQ(srv_->last_session->ui_objects.count(contents_root), 0u);
}

TEST_F(ZipToolEventTest, WindowClosedEmitsClosedAndCleansUp) {
  bool got_closed = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got_closed = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(""), "closed"_key, dynamic{});

  wait_for(got_closed);
  EXPECT_TRUE(got_closed);
  EXPECT_EQ(srv_->last_session->ui_objects.count(root_), 0u);
}

// ── Standalone dispatch: repeated RMI calls must not hang ─────────────────────

TEST(ZipToolStandaloneTest, RepeatedUpdateListingCallsDoNotHangOrFail) {
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto tool = sa.instantiate("wish"_key, "ZipTool"_key).get();
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

TEST(ZipToolStandaloneTest, RepeatedUpdateListingCallsDoNotHangOrFailUnderWebRenderer) {
  wish::standalone sa{std::make_unique<wish::web_renderer>("127.0.0.1", 0, 16)};
  sa.start();

  auto tool = sa.instantiate("wish"_key, "ZipTool"_key).get();
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
