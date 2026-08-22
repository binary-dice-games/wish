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
// (modules/bdg/desktop/mc/client/mc.cpp)'s
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

// Returns the ContextMenu child of TableRow `row` (fill_table()'s per-row
// ContextMenu is always appended as a child alongside the name/size/modified
// cells -- see mc.cpp), or a null dynamic_ptr if `row` has none (e.g. the
// ".." pseudo-row, which fill_table() deliberately skips).
dynamic_ptr find_row_context_menu(const dynamic& row) {
  auto* rcf = row.findField<dynamic_ptr>("children"_key);
  if (!rcf || !*rcf)
    return nullptr;
  dynamic_ptr found{nullptr};
  (*rcf)->forEach([&](bison::key_t, const field& f) {
    if (found)
      return;
    auto* ep = f.get<dynamic_ptr>();
    if (!ep || !*ep)
      return;
    auto* cls = (*ep)->findField(dynamic::CLASS);
    if (cls && cls->as<bison::key_t>() == "ContextMenu"_key)
      found = *ep;
  });
  return found;
}

// Returns the MenuItem child of `menu` (a ContextMenu) whose "label" field
// equals `label`, or a null dynamic_ptr if none match.
dynamic_ptr find_menu_item(const dynamic& menu, const std::string& label) {
  auto* cf = menu.findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return nullptr;
  dynamic_ptr found{nullptr};
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (found)
      return;
    auto* ep = f.get<dynamic_ptr>();
    if (!ep || !*ep)
      return;
    auto* cls = (*ep)->findField(dynamic::CLASS);
    auto* lbl = (*ep)->findField<std::string>("label"_key);
    if (cls && cls->as<bison::key_t>() == "MenuItem"_key && lbl && *lbl == label)
      found = *ep;
  });
  return found;
}

// Reads the plain-string array under `payload["names"]` -- see git.cpp's
// read_string_array() for the same convention (array entries are raw
// std::string fields, not nested dynamic_ptr objects).
std::vector<std::string> read_names(const dynamic& payload) {
  std::vector<std::string> names;
  if (auto* names_f = payload.findField<dynamic_ptr>("names"_key); names_f && *names_f) {
    (*names_f)->forEach([&](bison::key_t, const field& f) {
      if (f.is<std::string>())
        names.push_back(f.as<std::string>());
    });
  }
  return names;
}

// Simulates a "row_selected" click on `table_id` for row `idx`, optionally
// with Ctrl/Shift held -- mirrors render_table()'s payload shape
// (imgui_ui_renderer.cpp), including that ctrl/shift are omitted from a
// plain click's payload (they default to false server-side).
void click_row(wish::ui_root* handler, bison::key_t table_id, int32_t idx, bool ctrl = false, bool shift = false) {
  dynamic sel;
  sel["index"_key] = idx;
  if (ctrl)
    sel["ctrl"_key] = true;
  if (shift)
    sel["shift"_key] = true;
  handler->on_event(table_id, "row_selected"_key, sel);
}

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class McLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(McLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Mc"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Mc"_key);
}

TEST_F(McLocalTest, DefaultTitle) {
  auto obj = dynamic::instantiate("wish"_key, "Mc"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Mc");
}

TEST_F(McLocalTest, DefaultStatus) {
  auto obj = dynamic::instantiate("wish"_key, "Mc"_key);
  auto* f = obj.findField("status"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->as<std::string>(), "Ready.");
}

// ── Session-capturing server for internal-mc tests ──────────────────────────

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

// Helper: find the root key for the internal form mc (starts with
// "__mc_", no dot -- i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__mc_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class McWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "Mc"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(McWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __mc_... root key in session.objects";
}

TEST_F(McWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(McWindowTest, McContainsBothPanels) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.left.left_path"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.left.left_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.right.right_path"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.right.right_table"));
}

TEST_F(McWindowTest, McContainsTransferControls) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.middle.upload"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.middle.download"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.panels.right.right_header.open_explorer"));
}

TEST_F(McWindowTest, McContainsStatusAndProgress) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.status"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".main.transfer_progress"));
}

TEST_F(McWindowTest, SandboxAutoPopulatesOnInitWithoutHanging) {
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

class McRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Mc"_key).get());
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

TEST_F(McRmiTest, UpdateLocalListingPopulatesLeftTable) {
  update_local_listing(
      "/home/user",
      {{"docs", "dir", "", "2026-01-01 00:00"}, {"notes.txt", "file", "1.2 KB", "2026-01-02 00:00"}});

  EXPECT_EQ(table_row_count(root_ + ".main.panels.left.left_table"), 2u);
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_path")->as<std::string>("value"_key),
      "/home/user");
}

TEST_F(McRmiTest, UpdateLocalListingReplacesPreviousEntries) {
  update_local_listing("/a", {{"one.txt", "file", "1 B", ""}});
  ASSERT_EQ(table_row_count(root_ + ".main.panels.left.left_table"), 1u);

  update_local_listing("/b", {{"two.txt", "file", "2 B", ""}, {"three.txt", "file", "3 B", ""}});
  EXPECT_EQ(table_row_count(root_ + ".main.panels.left.left_table"), 2u);
}

TEST_F(McRmiTest, UpdateLocalListingShowsTypeIconInNameCell) {
  // fill_table() (mc.cpp) now builds the Name cell via
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

TEST_F(McRmiTest, RefreshSandboxRepopulatesRightTable) {
  // Regression test for the navigate_sandbox() self-deadlock on the
  // do_refresh_sandbox() dispatch call path specifically.
  refresh_sandbox();
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Ready.");
}

TEST_F(McRmiTest, SetStatusMirrorsToStatusLabel) {
  dynamic patch;
  patch["status"_key] = std::string{"Uploading..."};
  proxy_->set(std::move(patch)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key), "Uploading...");
}

TEST_F(McRmiTest, SetTransferProgressMirrorsToProgressBar) {
  dynamic patch;
  patch["transfer_progress"_key] = 0.5f;
  proxy_->set(std::move(patch)).get();

  EXPECT_FLOAT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.transfer_progress")->as<float>("value"_key), 0.5f);
}

TEST_F(McRmiTest, SetTransferLabelMirrorsToProgressBarLabel) {
  dynamic patch;
  patch["transfer_label"_key] = std::string{"3 of 5"};
  proxy_->set(std::move(patch)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.transfer_progress")->as<std::string>("label"_key),
      "3 of 5");
}

// ── Disk-usage summary strip ───────────────────────────────────────────────────

TEST_F(McRmiTest, UpdateLocalListingWithDiskStatsPopulatesSummaryLabels) {
  dynamic args = make_local_listing_args("/home/user", {{"a.txt", "file", "1 KB", ""}});
  args["file_count"_key] = int32_t{3};
  args["total_size"_key] = std::string{"12.0 MB"};
  args["disk_used"_key] = std::string{"1.0 GB"};
  args["disk_free"_key] = std::string{"9.0 GB"};
  args["disk_total"_key] = std::string{"10.0 GB"};
  proxy_->call("update_local_listing"_key, std::move(args)).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_stats")->as<std::string>("text"_key),
      "3 files, 12.0 MB");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_disk")->as<std::string>("text"_key),
      "Disk: 1.0 GB used, 9.0 GB free of 10.0 GB");
}

TEST_F(McRmiTest, UpdateLocalListingWithoutDiskStatsLeavesSummaryBlank) {
  // A client that predates this feature omits file_count/total_size/disk_*
  // entirely -- do_update_local_listing() must degrade to a blank strip
  // rather than crashing or showing stale/garbage text.
  update_local_listing("/home/user", {{"a.txt", "file", "1 KB", ""}});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_stats")->as<std::string>("text"_key), "");
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_disk")->as<std::string>("text"_key), "");
}

TEST_F(McRmiTest, SandboxStatsShowsFileCountAndTotalSizeExcludingDirectories) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "a.txt"); out << std::string(1000, 'x'); }
  { std::ofstream out(resource_dir / "b.txt"); out << std::string(2000, 'x'); }
  std::filesystem::create_directories(resource_dir / "sub"); // must not count as a file
  refresh_sandbox();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_stats")->as<std::string>("text"_key),
      "2 files, 2.9 KB");

  auto disk_text = srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_disk")->as<std::string>("text"_key);
  EXPECT_EQ(disk_text.rfind("Disk: ", 0), 0u) << "got: " << disk_text;
}

// ── Row context menu (Properties / Rename / Copy Path) ─────────────────────────

TEST_F(McRmiTest, SandboxRowContextMenuHasExpectedItemsWithSandboxRelativeCopyPath) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "note.txt"); out << "hi"; }
  refresh_sandbox();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  ASSERT_TRUE(row0);

  auto menu = find_row_context_menu(*row0);
  ASSERT_TRUE(menu) << "expected a ContextMenu on the row";

  EXPECT_TRUE(find_menu_item(*menu, "Properties"));
  EXPECT_TRUE(find_menu_item(*menu, "Rename..."));
  auto copy_path = find_menu_item(*menu, "Copy Path");
  ASSERT_TRUE(copy_path);
  EXPECT_EQ(copy_path->as<std::string>("copy_text"_key), "/note.txt");
}

TEST_F(McRmiTest, LocalRowContextMenuCopyPathUsesLocalPath) {
  update_local_listing("/home/user", {{"notes.txt", "file", "1 KB", ""}});

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_table")->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  ASSERT_TRUE(row0);

  auto menu = find_row_context_menu(*row0);
  ASSERT_TRUE(menu);
  auto copy_path = find_menu_item(*menu, "Copy Path");
  ASSERT_TRUE(copy_path);
  EXPECT_EQ(copy_path->as<std::string>("copy_text"_key), "/home/user/notes.txt");
}

// ── Event routing ─────────────────────────────────────────────────────────────

class McEventTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Mc"_key).get());
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

  // Reads the Name column's Label text for row_idx in the Table at
  // `table_path` (e.g. ".main.panels.left.left_table"). Column 0 is a
  // make_name_cell() wrapper -- a HorizontalLayout holding a type icon at
  // children[0] and the name Label at children[1] (see
  // file_browser_utils.hpp's doc comment).
  std::string table_name_cell_text(const std::string& table_path, size_t row_idx) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + table_path);
    if (it == objs.end() || !it->second)
      return {};
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    auto& row = *(*cf)->at(row_idx).as<dynamic_ptr>();
    auto* rcf = row.findField<dynamic_ptr>("children"_key);
    if (!rcf || !*rcf)
      return {};
    auto& name_cell = *(*rcf)->at(size_t{0}).as<dynamic_ptr>();
    auto* ncf = name_cell.findField<dynamic_ptr>("children"_key);
    if (!ncf || !*ncf)
      return {};
    return (*ncf)->at(size_t{1}).as<dynamic_ptr>()->as<std::string>("text"_key);
  }

  // Reads every row's `selected` bool for the Table at `table_path`, in row
  // order -- lets a multi-selection test assert the whole highlighted set
  // at once instead of one row at a time.
  std::vector<bool> row_selected_flags(const std::string& table_path) const {
    std::vector<bool> flags;
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + table_path);
    if (it == objs.end() || !it->second)
      return flags;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return flags;
    for (size_t i = 0; i < (*cf)->size(); ++i)
      flags.push_back((*cf)->at(i).as<dynamic_ptr>()->as<bool>("selected"_key));
    return flags;
  }

  // Simulate a "sorted" event on the Table at `table_path` -- see
  // table.cpp's Table.flags doc comment and imgui_ui_renderer.cpp's
  // render_table(). column_id 0/1/2 = Name/Size/Modified (see kLayout's
  // col_name/col_size/col_modified).
  void simulate_sorted(const std::string& table_path, int32_t column_id, bool ascending) {
    dynamic payload;
    payload["column_id"_key] = column_id;
    payload["ascending"_key] = ascending;
    handler_->on_event(widget_id(table_path), "sorted"_key, payload);
  }

  // form::emit() defers delivery to the render loop's next frame, so spin
  // briefly for it, same idiom as test_top.cpp.
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

TEST_F(McEventTest, LocalPathBarChangedEmitsOnLocalNavigate) {
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

TEST_F(McEventTest, LocalRowActivatedOnDirEmitsOnLocalNavigate) {
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

TEST_F(McEventTest, UploadClickedWithNoSelectionSetsStatusInsteadOfEmitting) {
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

TEST_F(McEventTest, UploadClickedWithSelectionEmitsOnUploadRequested) {
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
  EXPECT_EQ(read_names(captured), std::vector<std::string>{"a.txt"});
  EXPECT_EQ(captured.as<std::string>("local_path"_key), "/home");
}

TEST_F(McEventTest, LocalRowSelectedUpdatesSelectedLabel) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();

  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: a.txt");
}

TEST_F(McEventTest, LocalSelectionLabelResetsOnListingRefresh) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();
  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.left.left_table"), "row_selected"_key, sel);

  proxy_->call("update_local_listing"_key, make_local_listing_args("/other", {{"b.txt", "file", "1 B", ""}})).get();

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: (none)");
}

TEST_F(McEventTest, SandboxRowSelectedUpdatesSelectedLabel) {
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

TEST_F(McEventTest, LocalTableSortedEventSortsRowsDescendingByName) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args(
          "/home", {{"zebra.txt", "file", "1 B", ""}, {"apple.txt", "file", "1 B", ""}}))
      .get();
  // Default order is ascending by Name.
  ASSERT_EQ(table_name_cell_text(".main.panels.left.left_table", 0), "apple.txt");

  simulate_sorted(".main.panels.left.left_table", /*column_id=*/0, /*ascending=*/false);

  EXPECT_EQ(table_name_cell_text(".main.panels.left.left_table", 0), "zebra.txt");
  EXPECT_EQ(table_name_cell_text(".main.panels.left.left_table", 1), "apple.txt");
}

TEST_F(McEventTest, SandboxTableSortedBySizeOrdersNumericallyNotLexicographically) {
  // "19.5 KB" sorts before "5 B" lexicographically ('1' < '5'), but a
  // numeric Size sort must put the smaller file first -- exercises
  // parse_display_size() (file_browser_utils.hpp), not a plain string
  // compare.
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "big.txt"); out << std::string(20000, 'x'); }
  { std::ofstream out(resource_dir / "small.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  simulate_sorted(".main.panels.right.right_table", /*column_id=*/1, /*ascending=*/true);

  EXPECT_EQ(table_name_cell_text(".main.panels.right.right_table", 0), "small.txt");
  EXPECT_EQ(table_name_cell_text(".main.panels.right.right_table", 1), "big.txt");
}

TEST_F(McEventTest, SandboxSortKeepsDotDotPinnedFirst) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  std::filesystem::create_directories(resource_dir / "sub");
  { std::ofstream out(resource_dir / "sub" / "zzz.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  // Navigate into "sub" via the right_path InputText's "changed" event --
  // navigate_sandbox() resolves a typed path the same way, and this makes
  // sandbox_path_ non-empty so it injects the ".." row.
  dynamic changed;
  changed["value"_key] = std::string{"/sub"};
  handler_->on_event(widget_id(".main.panels.right.right_path"), "changed"_key, changed);

  simulate_sorted(".main.panels.right.right_table", /*column_id=*/0, /*ascending=*/false);

  // Descending by name would normally put "zzz.txt" first, but ".." must
  // stay pinned at row 0 regardless of sort column/direction.
  EXPECT_EQ(table_name_cell_text(".main.panels.right.right_table", 0), ".. [Up]");
  EXPECT_EQ(table_name_cell_text(".main.panels.right.right_table", 1), "zzz.txt");
}

TEST_F(McEventTest, DownloadClickedWithNoSelectionSetsStatusInsteadOfEmitting) {
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

TEST_F(McEventTest, LocalRowSelectedHighlightsSelectedRowOnly) {
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

// ── Multi-selection (Ctrl/Shift) ────────────────────────────────────────────────

TEST_F(McEventTest, CtrlClickAddsRowWithoutClearingPreviousSelection) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args(
          "/home", {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}, {"c.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");

  click_row(handler_, table_id, 0);
  click_row(handler_, table_id, 2, /*ctrl=*/true);

  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{true, false, true}));
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: 2 items");
}

TEST_F(McEventTest, CtrlClickOnAlreadySelectedRowTogglesItOff) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");

  click_row(handler_, table_id, 0);
  click_row(handler_, table_id, 1, /*ctrl=*/true);
  click_row(handler_, table_id, 1, /*ctrl=*/true); // toggle b.txt back off

  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{true, false}));
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: a.txt");
}

TEST_F(McEventTest, ShiftClickSelectsContiguousRangeFromAnchor) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args(
          "/home",
          {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}, {"c.txt", "file", "1 B", ""},
           {"d.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");

  click_row(handler_, table_id, 0); // anchor
  click_row(handler_, table_id, 2, /*ctrl=*/false, /*shift=*/true);

  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{true, true, true, false}));
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: 3 items");
}

TEST_F(McEventTest, RepeatedShiftClickRedefinesRangeFromSameAnchor) {
  // Mirrors a Shift+drag sweep: render_table() re-emits row_selected with
  // shift=true for each newly hovered row while the button stays down, so
  // repeated Shift+"clicks" against the same anchor is exactly what a drag
  // extension looks like from the consumer's side.
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args(
          "/home",
          {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}, {"c.txt", "file", "1 B", ""},
           {"d.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");

  click_row(handler_, table_id, 0); // anchor
  click_row(handler_, table_id, 1, /*ctrl=*/false, /*shift=*/true);
  click_row(handler_, table_id, 3, /*ctrl=*/false, /*shift=*/true); // sweep extends further

  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{true, true, true, true}));
}

TEST_F(McEventTest, ShiftClickWithNoPriorAnchorActsAsPlainClick) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");

  click_row(handler_, table_id, 1, /*ctrl=*/false, /*shift=*/true);

  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{false, true}));
}

TEST_F(McEventTest, PlainClickAfterMultiSelectReplacesSelectionWithOneRow) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");

  click_row(handler_, table_id, 0);
  click_row(handler_, table_id, 1, /*ctrl=*/true);
  click_row(handler_, table_id, 1); // plain click clears the multi-selection

  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{false, true}));
}

TEST_F(McEventTest, SelectionSurvivesSortByName) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args("/home", {{"zebra.txt", "file", "1 B", ""}, {"apple.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");
  // Default order is ascending by Name: apple.txt(0), zebra.txt(1).
  click_row(handler_, table_id, 1); // select zebra.txt

  simulate_sorted(".main.panels.left.left_table", /*column_id=*/0, /*ascending=*/false);

  // Descending order flips to zebra.txt(0), apple.txt(1) -- the selection
  // is name-keyed, so it follows zebra.txt to its new row.
  ASSERT_EQ(table_name_cell_text(".main.panels.left.left_table", 0), "zebra.txt");
  EXPECT_EQ(row_selected_flags(".main.panels.left.left_table"), (std::vector<bool>{true, false}));
  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_selected")->as<std::string>("text"_key),
      "Selected: zebra.txt");
}

TEST_F(McEventTest, UploadClickedWithMultipleSelectedFilesEmitsAllNames) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args(
          "/home", {{"a.txt", "file", "1 B", ""}, {"b.txt", "file", "1 B", ""}, {"c.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");
  click_row(handler_, table_id, 0);
  click_row(handler_, table_id, 2, /*ctrl=*/true);

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
  EXPECT_EQ(read_names(captured), (std::vector<std::string>{"a.txt", "c.txt"}));
  EXPECT_EQ(captured.as<std::string>("local_path"_key), "/home");
}

TEST_F(McEventTest, UploadClickedWithSelectedDirectoryAndFileSkipsTheDirectory) {
  proxy_->call(
      "update_local_listing"_key,
      make_local_listing_args("/home", {{"docs", "dir", "", ""}, {"a.txt", "file", "1 B", ""}}))
      .get();
  auto table_id = widget_id(".main.panels.left.left_table");
  click_row(handler_, table_id, 0); // "docs" (a directory)
  click_row(handler_, table_id, 1, /*ctrl=*/true); // + "a.txt"

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
  EXPECT_EQ(read_names(captured), std::vector<std::string>{"a.txt"});
}

// ── ".." row has no context menu ────────────────────────────────────────────────

TEST_F(McEventTest, DotDotRowHasNoContextMenu) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  std::filesystem::create_directories(resource_dir / "sub");
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  // Navigate into "sub" via the right_path InputText's "changed" event (same
  // idiom as SandboxSortKeepsDotDotPinnedFirst above), which injects a
  // leading ".." row.
  dynamic changed;
  changed["value"_key] = std::string{"/sub"};
  handler_->on_event(widget_id(".main.panels.right.right_path"), "changed"_key, changed);

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  ASSERT_TRUE(row0);
  EXPECT_FALSE(find_row_context_menu(*row0)) << "\"..\" row should have no context menu";
}

// ── Properties dialog ────────────────────────────────────────────────────────────

TEST_F(McEventTest, SandboxPropertiesMenuItemClickOpensDialogWithCorrectInfo) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "note.txt"); out << "hello"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto menu = find_row_context_menu(*row0);
  ASSERT_TRUE(menu);
  auto properties = find_menu_item(*menu, "Properties");
  ASSERT_TRUE(properties);

  handler_->on_event(properties->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  EXPECT_EQ(objs.at("__mc_properties_0.grid.name_row")->as<std::string>("text"_key), "Name: note.txt");
  EXPECT_EQ(objs.at("__mc_properties_0.grid.type_row")->as<std::string>("text"_key), "Type: File");
  EXPECT_EQ(objs.at("__mc_properties_0.grid.size_row")->as<std::string>("text"_key), "Size: 5 B");
  EXPECT_EQ(objs.at("__mc_properties_0.grid.path_row")->as<std::string>("text"_key), "Path: /note.txt");

  // The Close button requests the dialog close; actual removal is deferred
  // to the Window's own "closed" event, mirroring
  // WindowClosedEmitsClosedAndCleansUp's pattern for the main window.
  auto close_id = objs.at("__mc_properties_0.close_row.btn_close")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(close_id, "clicked"_key, dynamic{});
  auto properties_window_id = objs.at("__mc_properties_0")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(properties_window_id, "closed"_key, dynamic{});
  EXPECT_EQ(objs.count("__mc_properties_0"), 0u);
}

// ── Rename dialog ─────────────────────────────────────────────────────────────

TEST_F(McEventTest, SandboxRenameMenuItemClickOpensDialogPrefilledWithCurrentName) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "note.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto menu = find_row_context_menu(*row0);
  auto rename = find_menu_item(*menu, "Rename...");
  ASSERT_TRUE(rename);

  handler_->on_event(rename->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  EXPECT_EQ(objs.at("__mc_rename_0.new_name")->as<std::string>("value"_key), "note.txt");
  EXPECT_EQ(objs.at("__mc_rename_0.message")->as<std::string>("text"_key), "Rename \"note.txt\" to:");
}

TEST_F(McEventTest, SandboxRenameApplyViaOkButtonRenamesFileAndRefreshesListing) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "old.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto rename = find_menu_item(*find_row_context_menu(*row0), "Rename...");
  handler_->on_event(rename->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  (*objs.at("__mc_rename_0.new_name"))["value"_key] = std::string{"new.txt"};
  auto ok_id = objs.at("__mc_rename_0.buttons.btn_ok")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(ok_id, "clicked"_key, dynamic{});

  EXPECT_FALSE(std::filesystem::exists(resource_dir / "old.txt"));
  EXPECT_TRUE(std::filesystem::exists(resource_dir / "new.txt"));
  EXPECT_EQ(objs.at(root_ + ".main.status")->as<std::string>("text"_key), "Renamed.");
  EXPECT_EQ(table_name_cell_text(".main.panels.right.right_table", 0), "new.txt");

  // request_close_at() only sets a flag; confirm the dialog is still present
  // until the Window's own "closed" event actually removes it.
  ASSERT_EQ(objs.count("__mc_rename_0"), 1u);
  auto rename_window_id = objs.at("__mc_rename_0")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(rename_window_id, "closed"_key, dynamic{});
  EXPECT_EQ(objs.count("__mc_rename_0"), 0u);
}

TEST_F(McEventTest, SandboxRenameViaEnterOnInputAppliesRenameSameAsOkButton) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "old.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto rename = find_menu_item(*find_row_context_menu(*row0), "Rename...");
  handler_->on_event(rename->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  (*objs.at("__mc_rename_0.new_name"))["value"_key] = std::string{"new.txt"};
  auto input_id = objs.at("__mc_rename_0.new_name")->as<bison::key_t>("__wish_id"_key);
  dynamic changed;
  changed["value"_key] = std::string{"new.txt"};
  handler_->on_event(input_id, "changed"_key, changed);

  EXPECT_TRUE(std::filesystem::exists(resource_dir / "new.txt"));
}

TEST_F(McEventTest, SandboxRenameCancelDoesNotRenameAndCleansUpOnClose) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "old.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto rename = find_menu_item(*find_row_context_menu(*row0), "Rename...");
  handler_->on_event(rename->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  auto cancel_id = objs.at("__mc_rename_0.buttons.btn_cancel")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(cancel_id, "clicked"_key, dynamic{});

  EXPECT_TRUE(std::filesystem::exists(resource_dir / "old.txt"));
  ASSERT_EQ(objs.count("__mc_rename_0"), 1u) << "Cancel requests close but doesn't remove objects yet";

  auto rename_window_id = objs.at("__mc_rename_0")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(rename_window_id, "closed"_key, dynamic{});
  EXPECT_EQ(objs.count("__mc_rename_0"), 0u);
}

TEST_F(McEventTest, SandboxRenameRejectsNameWithPathSeparator) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "old.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto rename = find_menu_item(*find_row_context_menu(*row0), "Rename...");
  handler_->on_event(rename->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  (*objs.at("__mc_rename_0.new_name"))["value"_key] = std::string{"a/b.txt"};
  auto ok_id = objs.at("__mc_rename_0.buttons.btn_ok")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(ok_id, "clicked"_key, dynamic{});

  EXPECT_TRUE(std::filesystem::exists(resource_dir / "old.txt")) << "invalid rename must not touch the filesystem";
  EXPECT_EQ(objs.at(root_ + ".main.status")->as<std::string>("text"_key), "Invalid name.");
}

TEST_F(McEventTest, LocalRenameApplyEmitsOnLocalRenameRequested) {
  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"old.txt", "file", "1 B", ""}})).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.left.left_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto rename = find_menu_item(*find_row_context_menu(*row0), "Rename...");
  ASSERT_TRUE(rename);
  handler_->on_event(rename->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  auto& objs = srv_->last_session->ui_objects;
  (*objs.at("__mc_rename_0.new_name"))["value"_key] = std::string{"new.txt"};

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_local_rename_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  auto ok_id = objs.at("__mc_rename_0.buttons.btn_ok")->as<bison::key_t>("__wish_id"_key);
  handler_->on_event(ok_id, "clicked"_key, dynamic{});

  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("old_name"_key), "old.txt");
  EXPECT_EQ(captured.as<std::string>("new_name"_key), "new.txt");
}

// ── Copy Path ─────────────────────────────────────────────────────────────────

TEST_F(McEventTest, SandboxCopyPathMenuItemClickSetsStatusMessage) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "note.txt"); out << "hi"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  auto* cf =
      srv_->last_session->ui_objects.at(root_ + ".main.panels.right.right_table")->findField<dynamic_ptr>("children"_key);
  auto row0 = (**cf)[size_t{0}].as<dynamic_ptr>();
  auto copy_item = find_menu_item(*find_row_context_menu(*row0), "Copy Path");
  ASSERT_TRUE(copy_item);

  handler_->on_event(copy_item->as<bison::key_t>("__wish_id"_key), "clicked"_key, dynamic{});

  EXPECT_EQ(
      srv_->last_session->ui_objects.at(root_ + ".main.status")->as<std::string>("text"_key),
      "Copied path for \"note.txt\" to clipboard.");
}

// ── Overwrite confirmation ─────────────────────────────────────────────────────
//
// The server no longer builds its own second modal window for this -- it
// just emits "on_upload_conflict"/"on_download_conflict" instead of
// "on_upload_requested"/"on_download_requested" when the target name already
// exists, and leaves confirming with the user (via an instantiated
// MessageBox, "yes_no" preset) to the client -- see
// modules/bdg/desktop/mc/client/mc.cpp's
// confirm_overwrite().

TEST_F(McEventTest, UploadClickedWithExistingSandboxFileEmitsConflictInsteadOfRequested) {
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

  bool got_requested = false;
  bool got_conflict = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_upload_requested"_key)
      got_requested = true;
    if (event == "on_upload_conflict"_key) {
      got_conflict = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.panels.middle.upload"), "clicked"_key, dynamic{});

  wait_for(got_conflict);
  EXPECT_FALSE(got_requested) << "upload should be held back pending overwrite confirmation";
  ASSERT_TRUE(got_conflict);
  EXPECT_EQ(read_names(captured), std::vector<std::string>{"a.txt"});
  EXPECT_EQ(captured.as<std::string>("local_path"_key), "/home");
}

TEST_F(McEventTest, DownloadClickedWithExistingLocalFileEmitsConflictInsteadOfRequested) {
  const auto& resource_dir = srv_->last_session->resource_dir;
  for (auto& entry : std::filesystem::directory_iterator{resource_dir})
    std::filesystem::remove_all(entry.path());
  { std::ofstream out(resource_dir / "a.txt"); out << "existing"; }
  proxy_->call("refresh_sandbox"_key, dynamic{}).get();

  proxy_->call("update_local_listing"_key, make_local_listing_args("/home", {{"a.txt", "file", "1 B", ""}})).get();
  dynamic sel;
  sel["index"_key] = int32_t{0};
  handler_->on_event(widget_id(".main.panels.right.right_table"), "row_selected"_key, sel);

  bool got_requested = false;
  bool got_conflict = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_download_requested"_key)
      got_requested = true;
    if (event == "on_download_conflict"_key) {
      got_conflict = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  handler_->on_event(widget_id(".main.panels.middle.download"), "clicked"_key, dynamic{});

  wait_for(got_conflict);
  EXPECT_FALSE(got_requested) << "download should be held back pending overwrite confirmation";
  ASSERT_TRUE(got_conflict);
  EXPECT_EQ(read_names(captured), std::vector<std::string>{"a.txt"});
}

TEST_F(McEventTest, WindowClosedEmitsClosedAndCleansUp) {
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
// Reproduces a bug found while debugging mc's upload flow via
// wish's automation module: a second RMI call on the same object, issued
// shortly after an earlier one on the same session, occasionally either
// throws "Method not found" or -- worse -- never resolves at all. Only ever
// observed under bdg::wish::standalone (worker thread + render thread, both
// contending for the session lock), never under the transport-backed
// bison::rmi::client + memory_server_transport pair every other test in
// this file uses. Bounded with wait_for() so a regression here fails this
// test instead of hanging the whole suite.
TEST(McStandaloneTest, RepeatedRefreshSandboxCallsDoNotHangOrFail) {
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto explorer = sa.instantiate("wish"_key, "Mc"_key).get();
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

TEST(McStandaloneTest, RepeatedRefreshSandboxCallsDoNotHangOrFailUnderWebRenderer) {
  wish::standalone sa{std::make_unique<wish::web_renderer>("127.0.0.1", 0, 16)};
  sa.start();

  auto explorer = sa.instantiate("wish"_key, "Mc"_key).get();
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

TEST(McStandaloneTest, SetThenCallSequenceDoesNotCorruptNamespace) {
  // Mirrors the exact client-side call sequence around one upload:
  // update_local_listing() once, then a set() patch (progress report),
  // another set() patch (clear progress), then a call() the object has
  // never serviced before (refresh_sandbox) -- reproducing "Method not
  // found" outside of any web/browser/automation involvement.
  wish::standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto explorer = sa.instantiate("wish"_key, "Mc"_key).get();
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
