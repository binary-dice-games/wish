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
// update_* method expects (see helm.hpp's do_update_* doc comments).
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

class HelmLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(HelmLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "HelmFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "HelmFrontend"_key);
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

// The windows this form registers are keyed "__helm_N" (the Releases main
// root) and "__helm_N_<suffix>" for repos/charts/history/details/console --
// find the bare main root (mirrors test_kubectl.cpp).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__helm_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__helm_N_<suffix>": a second '_' after the numeric index
    // ("__helm_" is 7 chars).
    if (k.find('_', 7) != std::string::npos)
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

class HelmRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "HelmFrontend"_key).get());
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

dynamic two_releases() {
  return make_list_args(
      "releases",
      {
          {{"namespace", "prod"}, {"name", "web"}, {"revision", "3"}, {"updated", "2026-01-15 10:23:45"},
           {"status", "deployed"}, {"chart", "nginx-15.1.0"}, {"app_version", "1.25.1"}},
          {{"namespace", "staging"}, {"name", "db"}, {"revision", "1"}, {"updated", "2026-01-16 08:00:00"},
           {"status", "pending-install"}, {"chart", "postgresql-12.0.0"}, {"app_version", "15.3"}},
      });
}

dynamic one_chart() {
  auto args = make_list_args(
      "charts", {{{"name", "bitnami/nginx"}, {"version", "15.1.0"}, {"app_version", "1.25.1"}, {"description", "web"}}});
  args["query"_key] = std::string{"nginx"};
  return args;
}

dynamic value_payload(const std::string& v) {
  dynamic p;
  p["value"_key] = v;
  return p;
}

} // namespace

#define HELM_TEXT_AT(path) srv_->last_session->ui_objects.at(path)->as<std::string>("text"_key)

// ── Windows ─────────────────────────────────────────────────────────────────

TEST_F(HelmRmiTest, InstantiationBuildsAllSixWindows) {
  auto& objects = srv_->last_session->ui_objects;
  EXPECT_TRUE(objects.count(root_ + ".vbox.toolbar.btn_refresh"));
  EXPECT_TRUE(objects.count(root_ + ".vbox.table"));
  EXPECT_TRUE(objects.count(root_ + "_repos.vbox.add_bar.btn_add"));
  EXPECT_TRUE(objects.count(root_ + "_charts.vbox.toolbar.btn_install"));
  EXPECT_FALSE(objects.count(root_ + "_install")) << "the install dialog is built on demand";
  EXPECT_TRUE(objects.count(root_ + "_history.vbox.table"));
  EXPECT_TRUE(objects.count(root_ + "_details.vbox.editor"));
  EXPECT_TRUE(objects.count(root_ + "_console.vbox.table"));
  for (const char* suffix : {"_repos", "_charts", "_history", "_details", "_console"})
    EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + suffix})) << suffix;
}

TEST_F(HelmRmiTest, RegistersDefaultDockLayoutAndClosingTearsItDown) {
  const std::string dl = find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_");
  ASSERT_FALSE(dl.empty());
  EXPECT_EQ(srv_->last_session->ui_objects.at(dl)->class_key(), "DockSpaceViewport"_key);

  size_t since = srv_->events->mark();
  fire_at(root_ + "_charts", id_at(root_ + "_charts"), "closed"_key);
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since));
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_").empty());
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_repos"));
}

// ── Releases ────────────────────────────────────────────────────────────────

TEST_F(HelmRmiTest, UpdateReleasesPopulatesTableAndStatusCounts) {
  call("update_releases"_key, two_releases());
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 2u);
  EXPECT_EQ(HELM_TEXT_AT(root_ + ".vbox.status"), "2 releases (1 deployed)");

  call("update_releases"_key, two_releases());
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 2u) << "a rebuild must replace, not append";
}

TEST_F(HelmRmiTest, RefreshButtonEmitsRefreshRequested) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_refresh"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("refresh_requested"_key, since));
}

TEST_F(HelmRmiTest, ReleaseFiltersToggleRowVisibility) {
  call("update_releases"_key, two_releases());
  const std::string table = root_ + ".vbox.table";

  dynamic pending;
  pending["value"_key] = int32_t{3}; // "Pending" matches pending-install
  fire(id_at(root_ + ".vbox.toolbar.state"), "changed"_key, std::move(pending));
  EXPECT_FALSE(row_visible(table, 0));
  EXPECT_TRUE(row_visible(table, 1));

  dynamic all;
  all["value"_key] = int32_t{0};
  fire(id_at(root_ + ".vbox.toolbar.state"), "changed"_key, std::move(all));
  fire(id_at(root_ + ".vbox.toolbar.ns"), "changed"_key, value_payload("PROD"));
  EXPECT_TRUE(row_visible(table, 0));
  EXPECT_FALSE(row_visible(table, 1));

  fire(id_at(root_ + ".vbox.toolbar.ns"), "changed"_key, value_payload(""));
  fire(id_at(root_ + ".vbox.toolbar.filter"), "changed"_key, value_payload("db"));
  EXPECT_FALSE(row_visible(table, 0));
  EXPECT_TRUE(row_visible(table, 1));
}

TEST_F(HelmRmiTest, UninstallConfirmsThenEmitsWithNamespace) {
  call("update_releases"_key, two_releases());

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Uninstall..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto msg = HELM_TEXT_AT(cr + ".body.message");
  EXPECT_NE(msg.find("'web'"), std::string::npos);
  EXPECT_NE(msg.find("'prod'"), std::string::npos);

  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  auto ev = srv_->events->wait_for("release_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "web");
  EXPECT_EQ(ev->as<std::string>("namespace"_key), "prod");
  EXPECT_EQ(ev->as<std::string>("action"_key), "uninstall");
}

TEST_F(HelmRmiTest, UninstallNoCancelsWithoutEmitting) {
  call("update_releases"_key, two_releases());
  fire(menu_id_in(root_ + ".vbox.table", 0, "Uninstall..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());

  size_t since = srv_->events->mark();
  fire_at(cr, id_at(cr + ".buttons.btn1"), "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("release_action_requested"_key, since));
}

TEST_F(HelmRmiTest, ReleaseRollbackTargetsThePreviousRevision) {
  call("update_releases"_key, two_releases());
  fire(menu_id_in(root_ + ".vbox.table", 0, "Rollback..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  EXPECT_NE(HELM_TEXT_AT(cr + ".body.message").find("previous revision"), std::string::npos);

  size_t since = srv_->events->mark();
  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  auto ev = srv_->events->wait_for("release_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("action"_key), "rollback");
  EXPECT_EQ(ev->as<std::string>("revision"_key), "");
}

TEST_F(HelmRmiTest, CommandResultRoutesFailureToScopedWindowStatus) {
  dynamic args;
  args["command"_key] = std::string{"repo add bitnami"};
  args["scope"_key] = std::string{"repos"};
  args["ok"_key] = false;
  args["output"_key] = std::string{"looks like a bad URL"};
  call("command_result"_key, std::move(args));
  auto st = HELM_TEXT_AT(root_ + "_repos.vbox.status");
  EXPECT_NE(st.find("failed"), std::string::npos);
  EXPECT_NE(st.find("bad URL"), std::string::npos);
  EXPECT_EQ(HELM_TEXT_AT(root_ + ".vbox.status"), "");
}

// ── Details ─────────────────────────────────────────────────────────────────

TEST_F(HelmRmiTest, ValuesMenuActionEmitsAndUpdateDetailsFillsEditor) {
  call("update_releases"_key, two_releases());
  const std::string editor = root_ + "_details.vbox.editor";

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Values"), "clicked"_key);
  auto ev = srv_->events->wait_for("details_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("kind"_key), "values");
  EXPECT_EQ(ev->as<std::string>("name"_key), "web");
  EXPECT_EQ(ev->as<std::string>("namespace"_key), "prod");
  EXPECT_NE(HELM_TEXT_AT(root_ + "_details.vbox.toolbar.target").find("prod/web"), std::string::npos);

  auto details = [](const std::string& kind, const std::string& name, const std::string& text) {
    dynamic d;
    d["kind"_key] = kind;
    d["name"_key] = name;
    d["namespace"_key] = std::string{"prod"};
    d["title"_key] = kind + ": prod/" + name;
    d["text"_key] = text;
    return d;
  };
  call("update_details"_key, details("values", "web", "replicaCount: 2\n"));
  const std::string first = editor_file(editor);
  EXPECT_EQ(first.rfind("private/", 0), 0u) << first;
  EXPECT_EQ(editor_text(editor), "replicaCount: 2\n");

  // A response for a target the window no longer shows is dropped.
  call("update_details"_key, details("manifest", "web", "stale"));
  call("update_details"_key, details("values", "other", "stale"));
  EXPECT_EQ(editor_file(editor), first);

  // Every accepted update gets a fresh file; the one it replaced is deleted.
  call("update_details"_key, details("values", "web", "replicaCount: 3\n"));
  EXPECT_NE(editor_file(editor), first);
  EXPECT_EQ(editor_text(editor), "replicaCount: 3\n");
  EXPECT_FALSE(std::filesystem::exists(srv_->last_session->resource_dir / first));

  // Closing any window removes the file.
  const auto file = srv_->last_session->resource_dir / editor_file(editor);
  ASSERT_TRUE(std::filesystem::exists(file));
  fire_at(root_ + "_details", id_at(root_ + "_details"), "closed"_key);
  EXPECT_FALSE(std::filesystem::exists(file));
}

TEST_F(HelmRmiTest, DetailsLanguageFollowsTheKind) {
  call("update_releases"_key, two_releases());
  call("update_charts"_key, one_chart());
  auto language = [&] {
    return srv_->last_session->ui_objects.at(root_ + "_details.vbox.editor")->as<std::string>("language"_key);
  };

  fire(menu_id_in(root_ + ".vbox.table", 0, "Notes"), "clicked"_key);
  EXPECT_EQ(language(), "none");
  fire(menu_id_in(root_ + ".vbox.table", 0, "Manifest"), "clicked"_key);
  EXPECT_EQ(language(), "yaml");

  size_t since = srv_->events->mark();
  fire_at(root_ + "_charts", menu_id_in(root_ + "_charts.vbox.table", 0, "Readme"), "clicked"_key);
  EXPECT_EQ(language(), "markdown");
  auto ev = srv_->events->wait_for("details_requested"_key, since, [](const dynamic& e) {
    return e.as<std::string>("kind"_key) == "chart_readme";
  });
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "bitnami/nginx");
  EXPECT_EQ(ev->as<std::string>("version"_key), "15.1.0");
}

// ── History ─────────────────────────────────────────────────────────────────

TEST_F(HelmRmiTest, HistoryMenuActionEmitsAndRevisionRollbackCarriesRevision) {
  call("update_releases"_key, two_releases());
  const std::string table = root_ + "_history.vbox.table";

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "History"), "clicked"_key);
  auto ev = srv_->events->wait_for("history_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "web");
  EXPECT_EQ(ev->as<std::string>("namespace"_key), "prod");

  auto history = [](const std::string& name) {
    auto args = make_list_args(
        "revisions",
        {
            {{"revision", "1"}, {"updated", "Mon Jan 12 10:00:00 2026"}, {"status", "superseded"},
             {"chart", "nginx-15.0.0"}, {"description", "Install complete"}},
            {{"revision", "2"}, {"updated", "Thu Jan 15 10:23:45 2026"}, {"status", "deployed"},
             {"chart", "nginx-15.1.0"}, {"description", "Upgrade complete"}},
        });
    args["name"_key] = name;
    args["namespace"_key] = std::string{"prod"};
    return args;
  };

  call("update_history"_key, history("other")); // stale: not the open release
  EXPECT_EQ(row_count(table), 0u);
  call("update_history"_key, history("web"));
  EXPECT_EQ(row_count(table), 2u);
  EXPECT_EQ(HELM_TEXT_AT(root_ + "_history.vbox.status"), "2 revisions");

  fire_at(root_ + "_history", menu_id_in(table, 0, "Rollback to this revision..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  EXPECT_NE(HELM_TEXT_AT(cr + ".body.message").find("revision 1"), std::string::npos);

  since = srv_->events->mark();
  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  auto rb = srv_->events->wait_for("release_action_requested"_key, since);
  ASSERT_TRUE(rb);
  EXPECT_EQ(rb->as<std::string>("name"_key), "web");
  EXPECT_EQ(rb->as<std::string>("namespace"_key), "prod");
  EXPECT_EQ(rb->as<std::string>("action"_key), "rollback");
  EXPECT_EQ(rb->as<std::string>("revision"_key), "1");
}

// ── Repositories ────────────────────────────────────────────────────────────

TEST_F(HelmRmiTest, RepoMenuUpdateFiresImmediatelyAndRemoveConfirms) {
  call("update_repos"_key, make_list_args("repos", {{{"name", "bitnami"}, {"url", "https://charts.bitnami.com/bitnami"}}}));
  const std::string table = root_ + "_repos.vbox.table";
  EXPECT_EQ(row_count(table), 1u);
  EXPECT_EQ(HELM_TEXT_AT(root_ + "_repos.vbox.status"), "1 repository");

  size_t since = srv_->events->mark();
  fire_at(root_ + "_repos", menu_id_in(table, 0, "Update"), "clicked"_key);
  auto up = srv_->events->wait_for("repo_action_requested"_key, since);
  ASSERT_TRUE(up);
  EXPECT_EQ(up->as<std::string>("name"_key), "bitnami");
  EXPECT_EQ(up->as<std::string>("action"_key), "update");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());

  fire_at(root_ + "_repos", menu_id_in(table, 0, "Remove..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  since = srv_->events->mark();
  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  auto rm = srv_->events->wait_for(
      "repo_action_requested"_key, since, [](const dynamic& e) { return e.as<std::string>("action"_key) == "remove"; });
  ASSERT_TRUE(rm);
  EXPECT_EQ(rm->as<std::string>("name"_key), "bitnami");
}

TEST_F(HelmRmiTest, UpdateAllEmitsUpdateWithEmptyName) {
  size_t since = srv_->events->mark();
  fire_at(root_ + "_repos", id_at(root_ + "_repos.vbox.toolbar.btn_update_all"), "clicked"_key);
  auto ev = srv_->events->wait_for("repo_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "");
  EXPECT_EQ(ev->as<std::string>("action"_key), "update");
}

TEST_F(HelmRmiTest, AddRepoNeedsBothFieldsThenEmitsAndClearsThem) {
  const std::string repos = root_ + "_repos";
  const auto add = id_at(repos + ".vbox.add_bar.btn_add");

  size_t since = srv_->events->mark();
  fire_at(repos, id_at(repos + ".vbox.add_bar.repo_name"), "changed"_key, value_payload("bitnami"));
  fire_at(repos, add, "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("repo_add_requested"_key, since));
  EXPECT_NE(HELM_TEXT_AT(repos + ".vbox.status").find("name and URL"), std::string::npos);

  fire_at(repos, id_at(repos + ".vbox.add_bar.repo_url"), "changed"_key, value_payload("https://example.com/charts"));
  fire_at(repos, add, "clicked"_key);
  auto ev = srv_->events->wait_for("repo_add_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "bitnami");
  EXPECT_EQ(ev->as<std::string>("url"_key), "https://example.com/charts");
  EXPECT_EQ(srv_->last_session->ui_objects.at(repos + ".vbox.add_bar.repo_name")->as<std::string>("value"_key), "");
}

// ── Charts ──────────────────────────────────────────────────────────────────

TEST_F(HelmRmiTest, SearchButtonEmitsTheTypedQuery) {
  const std::string charts = root_ + "_charts";
  fire_at(charts, id_at(charts + ".vbox.toolbar.query"), "changed"_key, value_payload("nginx"));

  size_t since = srv_->events->mark();
  fire_at(charts, id_at(charts + ".vbox.toolbar.btn_search"), "clicked"_key);
  auto ev = srv_->events->wait_for("search_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("query"_key), "nginx");

  call("update_charts"_key, one_chart());
  EXPECT_EQ(row_count(charts + ".vbox.table"), 1u);
  EXPECT_EQ(HELM_TEXT_AT(charts + ".vbox.status"), "1 chart matching 'nginx'");
}

TEST_F(HelmRmiTest, EmptySnapshotsExplainWhatToDoNext) {
  call("update_releases"_key, make_list_args("releases", {}));
  call("update_repos"_key, make_list_args("repos", {}));
  call("update_charts"_key, make_list_args("charts", {}));
  EXPECT_NE(HELM_TEXT_AT(root_ + ".vbox.status").find("Install a chart"), std::string::npos);
  EXPECT_NE(HELM_TEXT_AT(root_ + "_repos.vbox.status").find("Add one"), std::string::npos);
  EXPECT_NE(HELM_TEXT_AT(root_ + "_charts.vbox.status").find("Add a repository"), std::string::npos);
}

// ── Install / Upgrade dialog ────────────────────────────────────────────────

TEST_F(HelmRmiTest, ChartInstallOpensPrefilledDialogAndSubmitEmitsEverything) {
  const std::string charts = root_ + "_charts";
  const std::string dlg = root_ + "_install";
  call("update_charts"_key, one_chart());

  fire_at(charts, menu_id_in(charts + ".vbox.table", 0, "Install..."), "clicked"_key);
  auto& objects = srv_->last_session->ui_objects;
  ASSERT_TRUE(objects.count(dlg + ".vbox.buttons.btn_submit"));
  EXPECT_EQ(objects.at(dlg + ".vbox.chart_row.chart")->as<std::string>("value"_key), "bitnami/nginx");
  EXPECT_EQ(objects.at(dlg + ".vbox.chart_row.version")->as<std::string>("value"_key), "15.1.0");
  EXPECT_EQ(objects.at(dlg + ".vbox.buttons.btn_submit")->as<std::string>("label"_key), "Install");

  // No release name yet: nothing is emitted, the dialog says why.
  size_t since = srv_->events->mark();
  fire_at(dlg, id_at(dlg + ".vbox.buttons.btn_submit"), "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("install_requested"_key, since));
  EXPECT_NE(HELM_TEXT_AT(dlg + ".vbox.status").find("release name"), std::string::npos);

  // A name Kubernetes would reject (dots) is refused before helm runs.
  fire_at(dlg, id_at(dlg + ".vbox.release_row.release"), "changed"_key, value_payload("v1.0"));
  fire_at(dlg, id_at(dlg + ".vbox.buttons.btn_submit"), "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("install_requested"_key, since));
  EXPECT_NE(HELM_TEXT_AT(dlg + ".vbox.status").find("lowercase"), std::string::npos);

  fire_at(dlg, id_at(dlg + ".vbox.release_row.release"), "changed"_key, value_payload("my-web"));
  fire_at(dlg, id_at(dlg + ".vbox.values"), "changed"_key, value_payload("replicaCount: 2\n"));
  dynamic on;
  on["value"_key] = true;
  fire_at(dlg, id_at(dlg + ".vbox.opts.wait"), "changed"_key, std::move(on));
  fire_at(dlg, id_at(dlg + ".vbox.buttons.btn_submit"), "clicked"_key);
  auto ev = srv_->events->wait_for("install_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("mode"_key), "install");
  EXPECT_EQ(ev->as<std::string>("chart"_key), "bitnami/nginx");
  EXPECT_EQ(ev->as<std::string>("version"_key), "15.1.0");
  EXPECT_EQ(ev->as<std::string>("release"_key), "my-web");
  EXPECT_EQ(ev->as<std::string>("namespace"_key), "default");
  EXPECT_EQ(ev->as<std::string>("values"_key), "replicaCount: 2\n");
  EXPECT_TRUE(ev->as<bool>("create_namespace"_key));
  EXPECT_TRUE(ev->as<bool>("wait"_key));

  // A failure keeps the dialog (and what was typed) for a retry ...
  auto result = [](bool ok) {
    dynamic r;
    r["command"_key] = std::string{"install my-web"};
    r["scope"_key] = std::string{"install"};
    r["ok"_key] = ok;
    r["output"_key] = std::string{ok ? "" : "cannot re-use a name that is still in use"};
    return r;
  };
  call("command_result"_key, result(false));
  ASSERT_TRUE(srv_->last_session->ui_objects.count(dlg));
  EXPECT_NE(HELM_TEXT_AT(dlg + ".vbox.status").find("still in use"), std::string::npos);

  // ... success closes it and lands in the Releases status line.
  call("command_result"_key, result(true));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(dlg));
  EXPECT_FALSE(srv_->last_session->top_level_handlers.count(bison::key_t{dlg}));
  EXPECT_EQ(HELM_TEXT_AT(root_ + ".vbox.status"), "install my-web: OK");
}

TEST_F(HelmRmiTest, InstallChartButtonOpensEmptyDialogAndCancelClosesIt) {
  const std::string charts = root_ + "_charts";
  const std::string dlg = root_ + "_install";
  fire_at(charts, id_at(charts + ".vbox.toolbar.btn_install"), "clicked"_key);
  ASSERT_TRUE(srv_->last_session->ui_objects.count(dlg));
  EXPECT_EQ(srv_->last_session->ui_objects.at(dlg + ".vbox.chart_row.chart")->as<std::string>("value"_key), "");

  size_t since = srv_->events->mark();
  fire_at(dlg, id_at(dlg + ".vbox.buttons.btn_cancel"), "clicked"_key);
  EXPECT_FALSE(srv_->last_session->ui_objects.count(dlg));

  // Closing the dialog's own X must not tear the whole form down.
  fire_at(charts, id_at(charts + ".vbox.toolbar.btn_install"), "clicked"_key);
  fire_at(dlg, id_at(dlg), "closed"_key);
  EXPECT_FALSE(srv_->last_session->ui_objects.count(dlg));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(charts));
  EXPECT_FALSE(srv_->events->wait_for("closed"_key, since));
}

TEST_F(HelmRmiTest, UpgradeDialogLoadsReleaseValuesAndEmitsUpgrade) {
  const std::string dlg = root_ + "_install";
  call("update_releases"_key, two_releases());

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Upgrade..."), "clicked"_key);
  auto& objects = srv_->last_session->ui_objects;
  ASSERT_TRUE(objects.count(dlg));
  EXPECT_EQ(objects.at(dlg + ".vbox.release_row.release")->as<std::string>("value"_key), "web");
  EXPECT_EQ(objects.at(dlg + ".vbox.buttons.btn_submit")->as<std::string>("label"_key), "Upgrade");
  EXPECT_FALSE(objects.at(dlg + ".vbox.opts.create_ns")->as<bool>("visible"_key));

  auto req = srv_->events->wait_for("install_values_requested"_key, since);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->as<std::string>("source"_key), "release");
  EXPECT_EQ(req->as<std::string>("name"_key), "web");
  EXPECT_EQ(req->as<std::string>("namespace"_key), "prod");

  auto values = [](int32_t token, const std::string& text) {
    dynamic v;
    v["token"_key] = token;
    v["text"_key] = text;
    return v;
  };
  const int32_t token = req->as<int32_t>("token"_key);
  call("set_install_values"_key, values(token + 1, "stale: true\n")); // some other dialog
  EXPECT_EQ(objects.at(dlg + ".vbox.values")->as<std::string>("value"_key), "");
  call("set_install_values"_key, values(token, "replicaCount: 2\n"));
  EXPECT_EQ(objects.at(dlg + ".vbox.values")->as<std::string>("value"_key), "replicaCount: 2\n");

  fire_at(dlg, id_at(dlg + ".vbox.chart_row.chart"), "changed"_key, value_payload("bitnami/nginx"));
  fire_at(dlg, id_at(dlg + ".vbox.buttons.btn_submit"), "clicked"_key);
  auto ev = srv_->events->wait_for("install_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("mode"_key), "upgrade");
  EXPECT_EQ(ev->as<std::string>("release"_key), "web");
  EXPECT_EQ(ev->as<std::string>("namespace"_key), "prod");
  EXPECT_EQ(ev->as<std::string>("values"_key), "replicaCount: 2\n");
  EXPECT_FALSE(ev->as<bool>("create_namespace"_key));
}

TEST_F(HelmRmiTest, LoadChartDefaultsNeedsAChartThenRequestsItsValues) {
  const std::string charts = root_ + "_charts";
  const std::string dlg = root_ + "_install";
  fire_at(charts, id_at(charts + ".vbox.toolbar.btn_install"), "clicked"_key);

  size_t since = srv_->events->mark();
  fire_at(dlg, id_at(dlg + ".vbox.values_bar.btn_defaults"), "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("install_values_requested"_key, since));
  EXPECT_NE(HELM_TEXT_AT(dlg + ".vbox.status").find("chart first"), std::string::npos);

  fire_at(dlg, id_at(dlg + ".vbox.chart_row.chart"), "changed"_key, value_payload("oci://example.com/charts/app"));
  fire_at(dlg, id_at(dlg + ".vbox.values_bar.btn_defaults"), "clicked"_key);
  auto req = srv_->events->wait_for("install_values_requested"_key, since);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->as<std::string>("source"_key), "chart");
  EXPECT_EQ(req->as<std::string>("chart"_key), "oci://example.com/charts/app");
}

TEST_F(HelmRmiTest, ChartRowsAreCappedWithAHint) {
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;
  for (int i = 0; i < 510; ++i)
    rows.push_back({{"name", "repo/chart-" + std::to_string(i)}, {"version", "1.0.0"}});
  call("update_charts"_key, make_list_args("charts", rows));
  EXPECT_EQ(row_count(root_ + "_charts.vbox.table"), 500u);
  auto status = HELM_TEXT_AT(root_ + "_charts.vbox.status");
  EXPECT_NE(status.find("510 charts"), std::string::npos) << status;
  EXPECT_NE(status.find("first 500"), std::string::npos) << status;
}

// ── Console window (client `helm` subprocess trace) ─────────────────────────

TEST_F(HelmRmiTest, AppendCommandLogAddsRowsAndClearConsoleEmptiesThem) {
  auto log_args = [](const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
    dynamic a;
    a["command"_key] = command;
    a["exit_code"_key] = exit_code;
    a["ok"_key] = ok;
    a["output"_key] = output;
    return a;
  };
  const std::string table = root_ + "_console.vbox.table";

  EXPECT_EQ(row_count(table), 0u);
  call("append_command_log"_key, log_args("helm list -A", 0, true, ""));
  call("append_command_log"_key, log_args("helm uninstall x -n d", 1, false, "release: not found"));
  EXPECT_EQ(row_count(table), 2u);

  auto clear = menu_id_in(table, 0, "Clear Console");
  ASSERT_NE(clear.id, 0u);
  fire_at(root_ + "_console", clear, "clicked"_key);
  EXPECT_EQ(row_count(table), 0u);
}
