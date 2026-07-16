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
#include <filesystem>
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

struct fake_entry {
  std::string name;
  std::string type; ///< "file" or "dir"
  std::string size;
  std::string modified;
};

// Builds the {path, files: [{name, type, size, modified}, ...]} shape that
// do_update_local_listing() expects -- matches the reference client
// (modules/bdg/desktop/file_explorer/client/file_explorer.cpp)'s
// report_local_listing().
dynamic make_local_listing_args(const std::string& path, const std::vector<fake_entry>& entries) {
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

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class FileExplorerLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(FileExplorerLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "FileExplorer"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "FileExplorer"_key);
}

TEST_F(FileExplorerLocalTest, DefaultTitle) {
  auto obj = dynamic::instantiate("wish"_key, "FileExplorer"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "File Explorer");
}

TEST_F(FileExplorerLocalTest, DefaultStatus) {
  auto obj = dynamic::instantiate("wish"_key, "FileExplorer"_key);
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
// "__fileexplorer_", no dot -- i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__fileexplorer_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class FileExplorerWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "FileExplorer"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(FileExplorerWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __fileexplorer_... root key in session.objects";
}

TEST_F(FileExplorerWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(FileExplorerWindowTest, TreeContainsBothPanels) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.left.left_path"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.left.left_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.right.right_path"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.right.right_table"));
}

TEST_F(FileExplorerWindowTest, TreeContainsTransferControls) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.middle.upload"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.middle.download"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.right.right_header.open_explorer"));
}

TEST_F(FileExplorerWindowTest, TreeContainsStatusAndProgress) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.status"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.transfer_progress"));
}

TEST_F(FileExplorerWindowTest, SandboxAutoPopulatesOnInitWithoutHanging) {
  // Regression test for the navigate_sandbox() self-deadlock: on_init()
  // calls navigate_sandbox("", sess().resource_dir, ...) from inside RMI
  // dispatch. If that ever regresses to acquiring context_rlock
  // unconditionally again, this instantiate() call hangs forever instead
  // of returning -- the test's own timeout catches that.
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());

  // Sandbox root has no ".." row (it has no parent within the sandbox), and
  // populate_resource_dir() seeds the fresh temp dir with embedded assets,
  // so the table isn't necessarily empty -- just confirm it built without
  // hanging or throwing.
  auto it = srv_->last_session->ui_objects.find(root + ".main.panels.right.right_table");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());
  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root + ".main.status")->as<std::string>("text"_key), "Ready.");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root + ".main.panels.right.right_path")->as<std::string>("value"_key), "/");
}

// ── RMI methods + on_set() ─────────────────────────────────────────────────────

class FileExplorerRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "FileExplorer"_key).get());
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

  dynamic update_local_listing(const std::string& path, const std::vector<fake_entry>& entries) {
    return proxy_->call("update_local_listing"_key, make_local_listing_args(path, entries)).get();
  }

  dynamic refresh_sandbox() {
    return proxy_->call("refresh_sandbox"_key, dynamic{}).get();
  }

  // dynamic::size() only counts numeric-indexed ("array") entries.
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

TEST_F(FileExplorerRmiTest, UpdateLocalListingPopulatesLeftTable) {
  update_local_listing(
      "/home/user",
      {{"docs", "dir", "", "2026-01-01 00:00"}, {"notes.txt", "file", "1.2 KB", "2026-01-02 00:00"}});

  EXPECT_EQ(table_row_count(root_ + ".main.panels.left.left_table"), 2u);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_path")->as<std::string>("value"_key),
      "/home/user");
}

TEST_F(FileExplorerRmiTest, UpdateLocalListingReplacesPreviousEntries) {
  update_local_listing("/a", {{"one.txt", "file", "1 B", ""}});
  ASSERT_EQ(table_row_count(root_ + ".main.panels.left.left_table"), 1u);

  update_local_listing("/b", {{"two.txt", "file", "2 B", ""}, {"three.txt", "file", "3 B", ""}});
  EXPECT_EQ(table_row_count(root_ + ".main.panels.left.left_table"), 2u);
}

TEST_F(FileExplorerRmiTest, UpdateLocalListingShowsTypeIconInNameCell) {
  // fill_table() (file_explorer.cpp) now builds the Name cell via
  // make_name_cell() (file_browser_utils.cpp), the same helper file_dialog.cpp
  // uses -- a HorizontalLayout wrapping a type icon Image ahead of the name
  // Label, Windows-Explorer style. Confirm both the wrapper shape and that
  // icon_for_entry()'s per-extension mapping is actually wired up here too.
  update_local_listing(
      "/home/user",
      {{"docs", "dir", "", ""}, {"photo.png", "file", "1 KB", ""}, {"unknown.xyz", "file", "1 B", ""}});

  auto name_cell_icon_src = [&](size_t row_idx) -> std::string {
    auto it = srv_->last_session->ui_objects.find(root_ + ".main.panels.left.left_table");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    auto& row = *(*cf)->at(row_idx).as<dynamic_ptr>();
    auto* rcf = row.findField<dynamic_ptr>("children"_key);
    if (!rcf || !*rcf)
      return {};
    auto& name_cell = *(*rcf)->at(size_t{0}).as<dynamic_ptr>();
    auto* icf = name_cell.findField<dynamic_ptr>("children"_key);
    if (!icf || !*icf)
      return {};
    auto& icon_img = *(*icf)->at(size_t{0}).as<dynamic_ptr>();
    return icon_img.as<std::string>("src"_key);
  };

  EXPECT_EQ(name_cell_icon_src(0), "res/icons/folder.png"); // "docs", type "dir"
  EXPECT_EQ(name_cell_icon_src(1), "res/icons/image.png");  // "photo.png"
  EXPECT_EQ(name_cell_icon_src(2), "res/icons/file.png");   // "unknown.xyz" -- no mapping
}

TEST_F(FileExplorerRmiTest, RefreshSandboxRepopulatesRightTable) {
  // Regression test for the navigate_sandbox() self-deadlock on the
  // do_refresh_sandbox() dispatch call path specifically.
  refresh_sandbox();
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Ready.");
}

TEST_F(FileExplorerRmiTest, SetStatusMirrorsToStatusLabel) {
  dynamic patch;
  patch["status"_key] = std::string{"Uploading..."};
  proxy_->set(std::move(patch)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Uploading...");
}

TEST_F(FileExplorerRmiTest, SetTransferProgressMirrorsToProgressBar) {
  dynamic patch;
  patch["transfer_progress"_key] = 0.5f;
  proxy_->set(std::move(patch)).get();

  EXPECT_FLOAT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.transfer_progress")->as<float>("value"_key), 0.5f);
}

TEST_F(FileExplorerRmiTest, SetTransferLabelMirrorsToProgressBarLabel) {
  dynamic patch;
  patch["transfer_label"_key] = std::string{"3 of 5"};
  proxy_->set(std::move(patch)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.transfer_progress")->as<std::string>("label"_key),
      "3 of 5");
}

// ── Event routing ─────────────────────────────────────────────────────────────

class FileExplorerEventTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "FileExplorer"_key).get());
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

  // form::emit() defers delivery to the render loop's next frame, so spin
  // briefly for it, same idiom as test_process_explorer.cpp.
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

TEST_F(FileExplorerEventTest, LocalPathBarChangedEmitsOnLocalNavigate) {
  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_local_navigate"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic payload;
  payload["value"_key] = std::string{"/some/local/dir"};
  handler_->on_event(widget_id(".main.panels.left.left_path"), "changed"_key, payload);

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "/some/local/dir");
  EXPECT_EQ(captured.as<std::string>("type"_key), "path");
}

TEST_F(FileExplorerEventTest, LocalRowActivatedOnDirEmitsOnLocalNavigate) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"sub", "dir", "", ""}})).get();

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_local_navigate"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic payload;
  payload["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_activated"_key, payload);

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "sub");
  EXPECT_EQ(captured.as<std::string>("type"_key), "dir");
}

TEST_F(FileExplorerEventTest, UploadClickedWithNoSelectionSetsStatusInsteadOfEmitting) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_upload_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.panels.middle.upload"), "clicked"_key, dynamic{});

  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Select a local file to upload.");
}

TEST_F(FileExplorerEventTest, UploadClickedWithSelectionEmitsOnUploadRequested) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();

  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_upload_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.panels.middle.upload"), "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "a.txt");
  EXPECT_EQ(captured.as<std::string>("local_path"_key), "/home");
}

TEST_F(FileExplorerEventTest, LocalRowSelectedUpdatesSelectedLabel) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();

  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: a.txt");
}

TEST_F(FileExplorerEventTest, LocalSelectionLabelResetsOnListingRefresh) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();
  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  proxy_->call("update_local_listing"_key, make_local_listing_args("/other", {{"b.txt", "file", "1 B", ""}})).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: (none)");
}

TEST_F(FileExplorerEventTest, SandboxRowSelectedUpdatesSelectedLabel) {
  // Clear whatever populate_resource_dir() seeded so "s.txt" is the sandbox
  // root's only entry, making its row index deterministic (0).
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "s.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.right.right_table"), "row_selected"_key, sel);

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_selected")->as<std::string>("text"_key),
      "Selected: s.txt");
}

TEST_F(FileExplorerEventTest, DownloadClickedWithNoSelectionSetsStatusInsteadOfEmitting) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_download_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.panels.middle.download"), "clicked"_key, dynamic{});

  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Select a sandbox file to download.");
}

TEST_F(FileExplorerEventTest, LocalRowSelectedHighlightsSelectedRowOnly) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}}))
      .get();

  dynamic sel;
  sel["index"_key] = int32_t{1};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  auto* children_f =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_table")->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(children_f, nullptr);
  ASSERT_TRUE(*children_f);
  auto row0 = (**children_f)[size_t{0}].as<dynamic_ptr>();
  auto row1 = (**children_f)[size_t{1}].as<dynamic_ptr>();
  ASSERT_TRUE(row0);
  ASSERT_TRUE(row1);
  EXPECT_FALSE(row0->as<bool>("selected"_key));
  EXPECT_TRUE(row1->as<bool>("selected"_key));
}

// ── Overwrite confirmation ─────────────────────────────────────────────────────

// Helper: find the confirm dialog's top-level root key ("__fileexplorer_confirm_N", no dot).
static std::string find_confirm_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__fileexplorer_confirm_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

TEST_F(FileExplorerEventTest, UploadClickedWithExistingSandboxFileShowsConfirmInsteadOfEmitting) {
  // Seed the sandbox with a file of the same name as the local selection.
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "a.txt"); out << "existing"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();
  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_upload_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.panels.middle.upload"), "clicked"_key, dynamic{});

  wait_for(got);
  EXPECT_FALSE(got) << "upload should be held back pending overwrite confirmation";

  std::string confirm_root = find_confirm_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(confirm_root.empty()) << "no confirm dialog root registered";
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{confirm_root}));
}

TEST_F(FileExplorerEventTest, ConfirmOverwriteYesEmitsOnUploadRequested) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "a.txt"); out << "existing"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();
  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);
  handler_->on_event(widget_id(".main.panels.middle.upload"), "clicked"_key, dynamic{});

  std::string confirm_root = find_confirm_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id =
      srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn_yes")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_upload_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(yes_id, "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "a.txt");
}

TEST_F(FileExplorerEventTest, ConfirmOverwriteNoCancelsWithoutEmitting) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "a.txt"); out << "existing"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();
  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);
  handler_->on_event(widget_id(".main.panels.middle.upload"), "clicked"_key, dynamic{});

  std::string confirm_root = find_confirm_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(confirm_root.empty());
  auto no_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn_no")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_upload_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(no_id, "clicked"_key, dynamic{});

  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Upload cancelled.");
}

TEST_F(FileExplorerEventTest, WindowClosedEmitsClosedAndCleansUp) {
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
//
// Reproduces a bug found while debugging file_explorer's upload flow via
// wish's automation module: a second RMI call on the same object, issued
// shortly after an earlier one on the same session, occasionally either
// throws "Method not found" or -- worse -- never resolves at all. Only ever
// observed under bdg::wish::standalone (worker thread + render thread, both
// contending for the session lock), never under the transport-backed
// bison::rmi::client + memory_server_transport pair every other test in
// this file uses. Bounded with wait_for() so a regression here fails this
// test instead of hanging the whole suite.
TEST(FileExplorerStandaloneTest, RepeatedRefreshSandboxCallsDoNotHangOrFail) {
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto explorer = sa.instantiate("wish"_key, "FileExplorer"_key).get();
  ASSERT_TRUE(explorer.valid());

  auto call_ready = [&](const char* label) {
    auto fut = explorer.call("refresh_sandbox"_key, dynamic{});
    auto status = fut.wait_for(std::chrono::seconds(3));
    EXPECT_EQ(status, std::future_status::ready) << label << " did not complete within 3s";
    if (status == std::future_status::ready)
      EXPECT_NO_THROW(fut.get()) << label << " threw";
  };

  for (int i = 0; i < 10; ++i)
    call_ready("refresh_sandbox");

  sa.stop();
}

TEST(FileExplorerStandaloneTest, RepeatedRefreshSandboxCallsDoNotHangOrFailUnderWebRenderer) {
  wish::standalone sa{std::make_unique<wish::web_renderer>("127.0.0.1", 0, 16)};
  sa.start();

  auto explorer = sa.instantiate("wish"_key, "FileExplorer"_key).get();
  ASSERT_TRUE(explorer.valid());

  auto call_ready = [&](const char* label) {
    auto fut = explorer.call("refresh_sandbox"_key, dynamic{});
    auto status = fut.wait_for(std::chrono::seconds(3));
    EXPECT_EQ(status, std::future_status::ready) << label << " did not complete within 3s";
    if (status == std::future_status::ready)
      EXPECT_NO_THROW(fut.get()) << label << " threw";
  };

  for (int i = 0; i < 20; ++i)
    call_ready("refresh_sandbox");

  sa.stop();
}

TEST(FileExplorerStandaloneTest, SetThenCallSequenceDoesNotCorruptNamespace) {
  // Mirrors the exact client-side call sequence around one upload:
  // update_local_listing() once, then a set() patch (progress report),
  // another set() patch (clear progress), then a call() the object has
  // never serviced before (refresh_sandbox) -- reproducing "Method not
  // found" outside of any web/browser/automation involvement.
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto explorer = sa.instantiate("wish"_key, "FileExplorer"_key).get();
  ASSERT_TRUE(explorer.valid());

  dynamic listing_args;
  listing_args["path"_key] = std::string{"/tmp"};
  listing_args["files"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  explorer.call("update_local_listing"_key, std::move(listing_args)).get();

  for (int round = 0; round < 5; ++round) {
    dynamic progress_patch;
    progress_patch["transfer_progress"_key] = 0.5f;
    progress_patch["transfer_label"_key] = std::string{"5 / 10"};
    ASSERT_NO_THROW(explorer.set(std::move(progress_patch)).get()) << "round " << round << " progress set";

    dynamic clear_patch;
    clear_patch["transfer_progress"_key] = 0.0f;
    clear_patch["transfer_label"_key] = std::string{""};
    ASSERT_NO_THROW(explorer.set(std::move(clear_patch)).get()) << "round " << round << " clear set";

    auto fut = explorer.call("refresh_sandbox"_key, dynamic{});
    auto status = fut.wait_for(std::chrono::seconds(3));
    ASSERT_EQ(status, std::future_status::ready) << "refresh_sandbox hung on round " << round;
    EXPECT_NO_THROW(fut.get()) << "refresh_sandbox threw on round " << round;
  }

  sa.stop();
}
