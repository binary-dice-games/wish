// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// Generic `{ <key>: [ {field:value, ...}, ... ] }` builder -- the shape every
// update_* method expects (see pkg.hpp's do_update_* doc comments).
dynamic make_list_args(
    const std::string& key, const std::vector<std::vector<std::pair<std::string, std::string>>>& rows) {
  dynamic args;
  dynamic arr;
  size_t i = 0;
  for (auto& row : rows) {
    auto e = std::make_shared<dynamic>();
    for (auto& [k, v] : row)
      (*e)[bison::key_t{k}] = v;
    arr[i++] = dynamic_ptr{e};
  }
  args[bison::key_t{key}] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

} // namespace

// ── Local (non-RMI) fixture ─────────────────────────────────────────────────

class PkgLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(PkgLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "PkgFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "PkgFrontend"_key);
}

// ── Session-capturing server ───────────────────────────────────────────────

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

// The windows this form registers are keyed "__pkg_N" (the Packages main
// root) and "__pkg_N_<suffix>" for search/details/console --
// find the bare main root (mirrors test_helm.cpp).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__pkg_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__pkg_N_<suffix>": a second '_' after the numeric index
    // ("__pkg_" is 6 chars).
    if (k.find('_', 6) != std::string::npos)
      continue;
    return k;
  }
  return {};
}

static std::string find_root_with_prefix(const wish::name_map& objects, const std::string& prefix) {
  for (const auto& [k, _] : objects)
    if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
      return k;
  return {};
}

class PkgRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "PkgFrontend"_key).get());
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

  dynamic call(bison::key_t method, dynamic args) {
    return proxy_->call(method, std::move(args)).get();
  }

  // file_path of the TextEditor at @p path ("" if missing/unset).
  std::string editor_file(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    auto* f = it->second->findField<std::string>("file_path"_key);
    return f ? *f : std::string{};
  }

  // Contents of the sandbox file the TextEditor at @p path displays.
  std::string editor_text(const std::string& path) const {
    auto rel = editor_file(path);
    if (rel.empty())
      return {};
    std::ifstream in(srv_->last_session->resource_dir / rel, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
  }

  size_t row_count(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  static dynamic_ptr nth_child(const dynamic_ptr& parent, size_t index) {
    if (!parent)
      return nullptr;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    auto& f = (*cf)->at(index);
    return f.is<dynamic_ptr>() ? f.as<dynamic_ptr>() : nullptr;
  }

  dynamic_ptr row_in(const std::string& table_path, size_t index) const {
    auto it = srv_->last_session->ui_objects.find(table_path);
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    return nth_child(it->second, index);
  }

  // __wish_id of a row's MenuItem, found by label (robust to the
  // state-dependent item ordering / separator slots).
  bison::key_t menu_id_in(const std::string& table_path, size_t row, const std::string& label) const {
    auto row_ptr = row_in(table_path, row);
    if (!row_ptr)
      return {};
    auto* rcf = row_ptr->findField<dynamic_ptr>("children"_key);
    if (!rcf || !*rcf)
      return {};
    size_t last = (*rcf)->size() - 1; // last cell is the MenuButton
    auto menu = (*rcf)->at(last).as<dynamic_ptr>();
    if (!menu)
      return {};
    auto* mcf = menu->findField<dynamic_ptr>("children"_key);
    bison::key_t found{};
    if (mcf && *mcf)
      (*mcf)->forEach([&](bison::key_t, const field& f) {
        if (found.id || !f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
          return;
        auto* lf = f.as<dynamic_ptr>()->findField<std::string>("label"_key);
        if (lf && *lf == label)
          found = f.as<dynamic_ptr>()->as<bison::key_t>("__wish_id"_key);
      });
    return found;
  }

  bool row_visible(const std::string& table_path, size_t row) const {
    auto r = row_in(table_path, row);
    return r ? r->as<bool>("visible"_key) : false;
  }

  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }
  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }

  bison::key_t id_at(const std::string& abs_path) const {
    auto it = srv_->last_session->ui_objects.find(abs_path);
    return it == srv_->last_session->ui_objects.end() ? bison::key_t{}
                                                      : it->second->as<bison::key_t>("__wish_id"_key);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};


// ── Shared snapshots ────────────────────────────────────────────────────────

namespace {

// htop is upgradable; libc6 carries dpkg's architecture qualifier.
dynamic three_packages(bool outdated_checked = true) {
  auto args = make_list_args(
      "packages",
      {
          {{"name", "htop"}, {"version", "3.3.0-4"}, {"latest", "3.3.0-5"}, {"description", "process viewer"}},
          {{"name", "curl"}, {"version", "8.5.0"}, {"latest", ""}, {"description", "transfer a URL"}},
          {{"name", "libc6:amd64"}, {"version", "2.39"}, {"latest", ""}, {"description", "GNU C Library"}},
      });
  args["outdated_checked"_key] = outdated_checked;
  return args;
}

dynamic results_for(const std::string& query, const std::vector<std::vector<std::pair<std::string, std::string>>>& rows) {
  auto args = make_list_args("results", rows);
  args["query"_key] = query;
  return args;
}

dynamic value_payload(const std::string& v) {
  dynamic p;
  p["value"_key] = v;
  return p;
}

} // namespace

#define PKG_TEXT_AT(path) srv_->last_session->ui_objects.at(path)->as<std::string>("text"_key)

// Number of TableRow children really present (row_count() is "highest index
// + 1", which over-counts after rows were erased).
#define PKG_ROWS_AT(path) rows_at(path)

class PkgFormTest : public PkgRmiTest {
 protected:
  size_t rows_at(const std::string& path) const {
    size_t n = 0;
    auto* cf = srv_->last_session->ui_objects.at(path)->findField<dynamic_ptr>("children"_key);
    if (cf && *cf)
      (*cf)->forEach([&](bison::key_t, const field& f) {
        if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>() &&
            f.as<dynamic_ptr>()->as<bison::key_t>(dynamic::CLASS) == "TableRow"_key)
          ++n;
      });
    return n;
  }
  std::string cell(const std::string& table, size_t row, size_t col) const {
    return nth_child(row_in(table, row), col)->as<std::string>("text"_key);
  }
  void search(const std::string& query) {
    fire(id_at(root_ + ".vbox.find_bar.query"), "changed"_key, value_payload(query));
    fire(id_at(root_ + ".vbox.find_bar.btn_search"), "clicked"_key);
  }
};

// ── Windows ─────────────────────────────────────────────────────────────────

TEST_F(PkgFormTest, InstantiationBuildsAllFourWindows) {
  auto& objects = srv_->last_session->ui_objects;
  EXPECT_TRUE(objects.count(root_ + ".vbox.toolbar.btn_refresh"));
  EXPECT_TRUE(objects.count(root_ + ".vbox.find_bar.btn_search"));
  EXPECT_TRUE(objects.count(root_ + ".vbox.table"));
  EXPECT_TRUE(objects.count(root_ + "_search.vbox.table"));
  EXPECT_TRUE(objects.count(root_ + "_details.vbox.editor"));
  EXPECT_TRUE(objects.count(root_ + "_console.vbox.table"));
  for (const char* suffix : {"_search", "_details", "_console"})
    EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + suffix})) << suffix;
}

TEST_F(PkgFormTest, ClosingAnyWindowTearsEverythingDown) {
  size_t since = srv_->events->mark();
  fire_at(root_ + "_search", id_at(root_ + "_search"), "closed"_key);
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_details"));
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_").empty());
}

TEST_F(PkgFormTest, SetEnvironmentNamesTheManagerAndHowChangesAreMade) {
  dynamic args;
  args["manager"_key] = std::string{"apt"};
  args["text"_key] = std::string{"apt 3.2.0 (amd64)"};
  args["elevation"_key] = std::string{"pkexec"};
  call("set_environment"_key, std::move(args));
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.env"), "apt: apt 3.2.0 (amd64)  (changes run through pkexec)");

  dynamic brew;
  brew["manager"_key] = std::string{"brew"};
  brew["text"_key] = std::string{"Homebrew 4.3.0"};
  brew["elevation"_key] = std::string{"none"};
  call("set_environment"_key, std::move(brew));
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.env"), "brew: Homebrew 4.3.0");
}

// ── Packages ────────────────────────────────────────────────────────────────

TEST_F(PkgFormTest, UpdatePackagesPopulatesTableAndStatusCounts) {
  const std::string table = root_ + ".vbox.table";
  call("update_packages"_key, three_packages(false));
  EXPECT_EQ(PKG_ROWS_AT(table), 3u);
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.status"), "3 packages") << "no upgradable count before a check";

  call("update_packages"_key, three_packages(true));
  EXPECT_EQ(PKG_ROWS_AT(table), 3u) << "a rebuild must replace, not append";
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.status"), "3 packages (1 upgradable)");
  EXPECT_EQ(cell(table, 0, 2), "3.3.0-5") << "the Latest column";
  EXPECT_EQ(cell(table, 1, 2), "");
  EXPECT_EQ(cell(table, 0, 3), "process viewer");
}

TEST_F(PkgFormTest, FiltersRebuildTheRowsFromTheSnapshot) {
  const std::string table = root_ + ".vbox.table";
  call("update_packages"_key, three_packages());

  fire(id_at(root_ + ".vbox.toolbar.filter"), "changed"_key, value_payload("LIB"));
  ASSERT_EQ(PKG_ROWS_AT(table), 1u);
  EXPECT_EQ(cell(table, 0, 0), "libc6:amd64");
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.status"), "3 packages (1 upgradable), 1 matching");

  fire(id_at(root_ + ".vbox.toolbar.filter"), "changed"_key, value_payload(""));
  dynamic upgradable;
  upgradable["value"_key] = int32_t{1};
  fire(id_at(root_ + ".vbox.toolbar.state"), "changed"_key, std::move(upgradable));
  ASSERT_EQ(PKG_ROWS_AT(table), 1u);
  EXPECT_EQ(cell(table, 0, 0), "htop");

  // The filter survives a refresh.
  call("update_packages"_key, three_packages());
  EXPECT_EQ(PKG_ROWS_AT(table), 1u);
}

TEST_F(PkgFormTest, ALargeSnapshotOnlyBuildsTheFirstRows) {
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;
  for (int i = 0; i < 1000; ++i)
    rows.push_back({{"name", "pkg-" + std::to_string(i)}, {"version", "1.0"}});
  call("update_packages"_key, make_list_args("packages", rows));
  const std::string table = root_ + ".vbox.table";
  EXPECT_EQ(PKG_ROWS_AT(table), 300u);
  auto status = PKG_TEXT_AT(root_ + ".vbox.status");
  EXPECT_NE(status.find("1000 packages"), std::string::npos) << status;
  EXPECT_NE(status.find("first 300"), std::string::npos) << status;

  // Every package stays reachable through the filter.
  fire(id_at(root_ + ".vbox.toolbar.filter"), "changed"_key, value_payload("pkg-999"));
  ASSERT_EQ(PKG_ROWS_AT(table), 1u);
  EXPECT_EQ(cell(table, 0, 0), "pkg-999");
}

TEST_F(PkgFormTest, ToolbarButtonsEmitTheirRequests) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_refresh"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("refresh_requested"_key, since));
  fire(id_at(root_ + ".vbox.toolbar.btn_outdated"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("outdated_requested"_key, since));
  fire(id_at(root_ + ".vbox.toolbar.btn_index"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("index_requested"_key, since));
}

TEST_F(PkgFormTest, UpgradeAllAsksFirst) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_upgrade_all"), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  EXPECT_FALSE(srv_->events->wait_for("upgrade_all_requested"_key, since, nullptr, std::chrono::milliseconds{100}));
  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("upgrade_all_requested"_key, since));
}

TEST_F(PkgFormTest, RowMenuUpgradeFiresDirectlyAndRemoveConfirms) {
  call("update_packages"_key, three_packages());
  const std::string table = root_ + ".vbox.table";

  size_t since = srv_->events->mark();
  fire(menu_id_in(table, 0, "Upgrade"), "clicked"_key);
  auto ev = srv_->events->wait_for("package_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "htop");
  EXPECT_EQ(ev->as<std::string>("action"_key), "upgrade");

  since = srv_->events->mark();
  fire(menu_id_in(table, 2, "Remove..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  EXPECT_NE(PKG_TEXT_AT(cr + ".body.message").find("'libc6:amd64'"), std::string::npos);
  EXPECT_FALSE(srv_->events->wait_for("package_action_requested"_key, since, nullptr, std::chrono::milliseconds{100}));
  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  ev = srv_->events->wait_for("package_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "libc6:amd64");
  EXPECT_EQ(ev->as<std::string>("action"_key), "remove");
}

TEST_F(PkgFormTest, DetailsMenuActionEmitsAndUpdateDetailsFillsEditor) {
  call("update_packages"_key, three_packages());
  const std::string editor = root_ + "_details.vbox.editor";

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 1, "Files"), "clicked"_key);
  auto ev = srv_->events->wait_for("details_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("kind"_key), "files");
  EXPECT_EQ(ev->as<std::string>("name"_key), "curl");

  auto details = [](const std::string& kind, const std::string& name, const std::string& text) {
    dynamic d;
    d["kind"_key] = kind;
    d["name"_key] = name;
    d["title"_key] = kind + ": " + name;
    d["text"_key] = text;
    return d;
  };
  call("update_details"_key, details("show", "curl", "stale: another kind"));
  EXPECT_EQ(editor_file(editor), "");
  call("update_details"_key, details("files", "curl", "/usr/bin/curl\n"));
  EXPECT_EQ(editor_file(editor).rfind("private/", 0), 0u);
  EXPECT_EQ(editor_text(editor), "/usr/bin/curl\n");
}

TEST_F(PkgFormTest, CommandResultWritesOneLineToTheScopedStatus) {
  dynamic failed;
  failed["command"_key] = std::string{"install nope"};
  failed["ok"_key] = false;
  failed["output"_key] = std::string{"E: Unable to locate package nope\nmore"};
  call("command_result"_key, std::move(failed));
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.status"), "install nope failed: E: Unable to locate package nope");

  dynamic search_failed;
  search_failed["command"_key] = std::string{"search x"};
  search_failed["scope"_key] = std::string{"search"};
  search_failed["ok"_key] = false;
  search_failed["output"_key] = std::string{"index is locked"};
  call("command_result"_key, std::move(search_failed));
  EXPECT_NE(PKG_TEXT_AT(root_ + "_search.vbox.status").find("index is locked"), std::string::npos);
}

TEST_F(PkgFormTest, ASecondConfirmationOpensAfterTheFirstWasAnswered) {
  // The first MessageBox's root key is recycled by the second. Dropping the
  // first form object must not take the second one's dialog down with it.
  call("update_packages"_key, three_packages());
  auto confirm_root = [&] { return find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_"); };

  fire(menu_id_in(root_ + ".vbox.table", 1, "Remove..."), "clicked"_key);
  std::string cr = confirm_root();
  ASSERT_FALSE(cr.empty());
  fire_at(cr, id_at(cr + ".buttons.btn1"), "clicked"_key); // No
  fire_at(cr, id_at(cr), "closed"_key);                    // the renderer confirms the modal closed
  ASSERT_TRUE(confirm_root().empty());

  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_upgrade_all"), "clicked"_key);
  cr = confirm_root();
  ASSERT_FALSE(cr.empty()) << "the second confirmation never appeared";
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{cr}));
  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("upgrade_all_requested"_key, since));
}

// ── Search / install ────────────────────────────────────────────────────────

TEST_F(PkgFormTest, SearchNeedsTextThenEmitsAndListsResultsMarkingInstalledOnes) {
  call("update_packages"_key, three_packages());
  const std::string table = root_ + "_search.vbox.table";

  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.find_bar.btn_search"), "clicked"_key);
  EXPECT_NE(PKG_TEXT_AT(root_ + ".vbox.status").find("Enter a package name"), std::string::npos);
  EXPECT_FALSE(srv_->events->wait_for("search_requested"_key, since, nullptr, std::chrono::milliseconds{100}));

  search("libc");
  auto ev = srv_->events->wait_for("search_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("query"_key), "libc");
  EXPECT_EQ(PKG_TEXT_AT(root_ + "_search.vbox.toolbar.target"), "search: libc");
  EXPECT_EQ(PKG_TEXT_AT(root_ + ".vbox.status"), "3 packages (1 upgradable)") << "the validation message is gone";

  // A response for a search the window no longer shows is dropped.
  call("update_search"_key, results_for("other", {{{"name", "x"}}}));
  EXPECT_EQ(PKG_ROWS_AT(table), 0u);

  call("update_search"_key, results_for("libc", {{{"name", "libc6"}, {"description", "GNU C Library"}},
                                                 {{"name", "libc6-dev"}, {"description", "headers"}}}));
  ASSERT_EQ(PKG_ROWS_AT(table), 2u);
  EXPECT_EQ(cell(table, 0, 1), "2.39") << "installed as libc6:amd64";
  EXPECT_EQ(cell(table, 1, 1), "");
  EXPECT_EQ(cell(table, 0, 2), "GNU C Library");
  EXPECT_EQ(PKG_TEXT_AT(root_ + "_search.vbox.status"), "2 matches");

  // The Installed column follows the next packages snapshot.
  auto more = make_list_args("packages", {{{"name", "libc6-dev"}, {"version", "2.39"}}});
  call("update_packages"_key, std::move(more));
  EXPECT_EQ(cell(table, 0, 1), "");
  EXPECT_EQ(cell(table, 1, 1), "2.39");

  call("update_search"_key, results_for("libc", {}));
  EXPECT_EQ(PKG_TEXT_AT(root_ + "_search.vbox.status"), "No package matches 'libc'.");
}

TEST_F(PkgFormTest, InstallFromTheSearchRowAndFromTheBox) {
  search("htop");
  call("update_search"_key, results_for("htop", {{{"name", "htop"}, {"description", "process viewer"}}}));

  size_t since = srv_->events->mark();
  fire_at(root_ + "_search", menu_id_in(root_ + "_search.vbox.table", 0, "Install"), "clicked"_key);
  auto ev = srv_->events->wait_for("install_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("names"_key), "htop");

  since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.find_bar.query"), "changed"_key, value_payload("curl wget"));
  fire(id_at(root_ + ".vbox.find_bar.btn_install"), "clicked"_key);
  ev = srv_->events->wait_for("install_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("names"_key), "curl wget");
  EXPECT_NE(PKG_TEXT_AT(root_ + ".vbox.status").find("Installing curl wget"), std::string::npos);
}

// ── Console window ──────────────────────────────────────────────────────────

TEST_F(PkgFormTest, AppendCommandLogAddsRowsAndClearConsoleEmptiesThem) {
  auto log_args = [](const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
    dynamic a;
    a["command"_key] = command;
    a["exit_code"_key] = exit_code;
    a["ok"_key] = ok;
    a["output"_key] = output;
    return a;
  };
  const std::string table = root_ + "_console.vbox.table";
  call("append_command_log"_key, log_args("apt-cache search htop", 0, true, "htop - viewer"));
  call("append_command_log"_key, log_args("sudo -n apt-get install -y -- nope", 100, false, "E: Unable to locate"));
  EXPECT_EQ(PKG_ROWS_AT(table), 2u);

  auto clear = menu_id_in(table, 0, "Clear Console");
  ASSERT_NE(clear.id, 0u);
  fire_at(root_ + "_console", clear, "clicked"_key);
  EXPECT_EQ(PKG_ROWS_AT(table), 0u);
}
