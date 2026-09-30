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
// update_* method expects (see kubectl.hpp's do_update_* doc comments).
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

class KubectlLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(KubectlLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "KubectlFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "KubectlFrontend"_key);
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

// The windows this form registers are keyed "__kubectl_N" (the Pods main
// root) and "__kubectl_N_<suffix>" for deployments/services/nodes/logs/
// describe -- find the bare main root (mirrors test_docker.cpp).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__kubectl_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__kubectl_N_<suffix>": a second '_' after the numeric index
    // ("__kubectl_" is 10 chars).
    if (k.find('_', 10) != std::string::npos)
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

class KubectlRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "KubectlFrontend"_key).get());
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

  // Count of live dynamic_ptr children, robust to sparse numeric keys left
  // by erase() (dynamic::size() reports "highest key + 1"). Used for the Top
  // window's plots, whose PlotLine children are added/removed at runtime.
  size_t child_elem_count(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    size_t n = 0;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
        ++n;
    });
    return n;
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

TEST_F(KubectlRmiTest, InstantiationBuildsAllSixWindows) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.toolbar.btn_refresh"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_deployments"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_services"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_nodes"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_logs.vbox.editor"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_describe.vbox.editor"));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_nodes"}));
}

// Both scroll axes must be enabled on every scrollable table: ImGui has no
// single "Scroll" flag, so the descriptor lists ScrollX|ScrollY explicitly.
TEST_F(KubectlRmiTest, ScrollableTablesEnableBothScrollAxes) {
  constexpr int32_t kScrollX = 1 << 24;
  constexpr int32_t kScrollY = 1 << 25;
  for (const char* suffix :
       {".vbox.table", "_deployments.vbox.table", "_services.vbox.table", "_nodes.vbox.table", "_console.vbox.table"}) {
    auto it = srv_->last_session->ui_objects.find(root_ + suffix);
    ASSERT_NE(it, srv_->last_session->ui_objects.end()) << suffix;
    int32_t flags = it->second->as<int32_t>("flags"_key);
    EXPECT_TRUE(flags & kScrollX) << suffix;
    EXPECT_TRUE(flags & kScrollY) << suffix;
  }
}

// Logs / Describe are read-only, syntax-highlighted TextEditors; only Logs
// follows the newest line.
TEST_F(KubectlRmiTest, LogsAndDescribePanesAreReadOnlyTextEditors) {
  for (auto [suffix, lang, auto_scroll] :
       {std::tuple{"_logs.vbox.editor", "log", true}, std::tuple{"_describe.vbox.editor", "yaml", false}}) {
    auto it = srv_->last_session->ui_objects.find(root_ + suffix);
    ASSERT_NE(it, srv_->last_session->ui_objects.end()) << suffix;
    EXPECT_EQ(it->second->as<bison::key_t>(dynamic::CLASS), "TextEditor"_key) << suffix;
    EXPECT_EQ(it->second->as<std::string>("language"_key), lang) << suffix;
    EXPECT_TRUE(it->second->as<bool>("read_only"_key)) << suffix;
    EXPECT_EQ(it->second->as<bool>("auto_scroll"_key), auto_scroll) << suffix;
    EXPECT_EQ(editor_file(root_ + suffix), "") << suffix;
  }
}

TEST_F(KubectlRmiTest, UpdatePodsPopulatesTableAndStatusCounts) {
  call("update_pods"_key,
       make_list_args("pods", {
           {{"namespace", "default"}, {"name", "web-0"}, {"phase", "Running"}, {"ready", "1/1"}, {"restarts", "0"}, {"age", "3h"}},
           {{"namespace", "default"}, {"name", "web-1"}, {"phase", "Running"}, {"ready", "1/1"}, {"restarts", "2"}, {"age", "3h"}},
           {{"namespace", "kube-system"}, {"name", "job-x"}, {"phase", "Succeeded"}, {"ready", "0/1"}, {"restarts", "0"}, {"age", "5h"}},
       }));
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 3u);
  auto status = srv_->last_session->ui_objects.at(root_ + ".vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("3 pods"), std::string::npos);
  EXPECT_NE(status.find("2 running"), std::string::npos);
}

TEST_F(KubectlRmiTest, RebuildIsIdempotentInRowCount) {
  auto args = make_list_args("pods", {{{"namespace", "d"}, {"name", "p"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1m"}}});
  call("update_pods"_key, args.clone());
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 1u);
  call("update_pods"_key, args.clone());
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 1u);
}

TEST_F(KubectlRmiTest, PodDeleteConfirmsThenEmitsWithNamespace) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Delete..."), "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("pod_action_requested"_key, since))
      << "delete must be held back pending confirmation";

  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto msg = srv_->last_session->ui_objects.at(cr + ".body.message")->as<std::string>("text"_key);
  EXPECT_NE(msg.find("api-7"), std::string::npos);
  EXPECT_NE(msg.find("prod"), std::string::npos);

  auto yes = srv_->last_session->ui_objects.at(cr + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);
  fire_at(cr, yes, "clicked"_key);
  auto cap = srv_->events->wait_for("pod_action_requested"_key, since);
  ASSERT_TRUE(cap);
  EXPECT_EQ(cap->as<std::string>("name"_key), "api-7");
  EXPECT_EQ(cap->as<std::string>("namespace"_key), "prod");
  EXPECT_EQ(cap->as<std::string>("action"_key), "delete");
}

TEST_F(KubectlRmiTest, DeleteNoCancelsWithoutEmitting) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "d"}, {"name", "p"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Delete..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto no = srv_->last_session->ui_objects.at(cr + ".buttons.btn1")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  size_t since = srv_->events->mark();
  fire_at(cr, no, "clicked"_key);
  got = srv_->events->wait_for("pod_action_requested"_key, since).has_value();
  EXPECT_FALSE(got);
}

TEST_F(KubectlRmiTest, DeploymentRestartFiresImmediately) {
  call("update_deployments"_key,
       make_list_args("deployments", {{{"namespace", "default"}, {"name", "web"}, {"ready", "3/3"}, {"uptodate", "3"}, {"available", "3"}, {"age", "10d"}}}));

  bool got = false;
  dynamic cap;
  size_t since = srv_->events->mark();
  fire_at(root_ + "_deployments", menu_id_in(root_ + "_deployments.vbox.table", 0, "Restart"), "clicked"_key);
  if (auto ev = srv_->events->wait_for("deployment_action_requested"_key, since)) {
    got = true;
    cap = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("action"_key), "restart");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}

TEST_F(KubectlRmiTest, NodeMenuIsStateAwareForCordon) {
  call("update_nodes"_key,
       make_list_args("nodes", {
           {{"name", "node-a"}, {"status", "Ready"}, {"schedulable", "true"}, {"version", "v1.29.0"}, {"age", "40d"}},
           {{"name", "node-b"}, {"status", "Ready,SchedulingDisabled"}, {"schedulable", "false"}, {"version", "v1.29.0"}, {"age", "40d"}},
       }));
  EXPECT_NE(menu_id_in(root_ + "_nodes.vbox.table", 0, "Cordon").id, 0u);
  EXPECT_EQ(menu_id_in(root_ + "_nodes.vbox.table", 0, "Uncordon").id, 0u);
  EXPECT_NE(menu_id_in(root_ + "_nodes.vbox.table", 1, "Uncordon").id, 0u);
  EXPECT_EQ(menu_id_in(root_ + "_nodes.vbox.table", 1, "Cordon").id, 0u);
}

TEST_F(KubectlRmiTest, NodeDrainConfirmsThenEmitsNameOnly) {
  call("update_nodes"_key,
       make_list_args("nodes", {{{"name", "node-a"}, {"status", "Ready"}, {"schedulable", "true"}, {"version", "v1.29.0"}, {"age", "40d"}}}));
  fire_at(root_ + "_nodes", menu_id_in(root_ + "_nodes.vbox.table", 0, "Drain..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto yes = srv_->last_session->ui_objects.at(cr + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  size_t since = srv_->events->mark();
  fire_at(cr, yes, "clicked"_key);
  if (auto ev = srv_->events->wait_for("node_action_requested"_key, since)) {
    got = true;
    cap = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("name"_key), "node-a");
  EXPECT_EQ(cap.as<std::string>("action"_key), "drain");
  EXPECT_FALSE(cap.findField<std::string>("namespace"_key));
}

TEST_F(KubectlRmiTest, RefreshButtonEmitsRefreshRequested) {
  bool got = false;
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_refresh"), "clicked"_key);
  got = srv_->events->wait_for("refresh_requested"_key, since).has_value();
  EXPECT_TRUE(got);
}

TEST_F(KubectlRmiTest, PhaseComboFilterHidesNonMatchingRows) {
  call("update_pods"_key,
       make_list_args("pods", {
           {{"namespace", "d"}, {"name", "run"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1h"}},
           {{"namespace", "d"}, {"name", "pend"}, {"phase", "Pending"}, {"ready", "0/1"}, {"age", "1m"}},
       }));
  EXPECT_TRUE(row_visible(root_ + ".vbox.table", 0));
  EXPECT_TRUE(row_visible(root_ + ".vbox.table", 1));

  dynamic p;
  p["value"_key] = int32_t{1}; // "Running"
  fire(id_at(root_ + ".vbox.toolbar.state"), "changed"_key, std::move(p));
  EXPECT_TRUE(row_visible(root_ + ".vbox.table", 0));
  EXPECT_FALSE(row_visible(root_ + ".vbox.table", 1));
}

TEST_F(KubectlRmiTest, NamespaceFilterMatchesNamespaceColumn) {
  call("update_pods"_key,
       make_list_args("pods", {
           {{"namespace", "default"}, {"name", "a"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1h"}},
           {{"namespace", "kube-system"}, {"name", "b"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1h"}},
       }));
  dynamic p;
  p["value"_key] = std::string{"kube"};
  fire(id_at(root_ + ".vbox.toolbar.ns"), "changed"_key, std::move(p));
  EXPECT_FALSE(row_visible(root_ + ".vbox.table", 0));
  EXPECT_TRUE(row_visible(root_ + ".vbox.table", 1));
}

TEST_F(KubectlRmiTest, CommandResultRoutesFailureToScopedWindowStatus) {
  dynamic args;
  args["command"_key] = std::string{"delete deployment web"};
  args["scope"_key] = std::string{"deployments"};
  args["ok"_key] = false;
  args["output"_key] = std::string{"deployments.apps \"web\" not found"};
  call("command_result"_key, std::move(args));
  auto st = srv_->last_session->ui_objects.at(root_ + "_deployments.vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(st.find("failed"), std::string::npos);
  EXPECT_NE(st.find("not found"), std::string::npos);
}

TEST_F(KubectlRmiTest, LogsMenuActionSetsTargetAndEmitsLogsRequested) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));

  bool got = false;
  dynamic cap;
  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);
  if (auto ev = srv_->events->wait_for("logs_requested"_key, since)) {
    got = true;
    cap = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("name"_key), "api-7");
  EXPECT_EQ(cap.as<std::string>("namespace"_key), "prod");
  EXPECT_FALSE(cap.as<bool>("follow"_key));

  auto target = srv_->last_session->ui_objects.at(root_ + "_logs.vbox.toolbar.target")->as<std::string>("text"_key);
  EXPECT_NE(target.find("api-7"), std::string::npos);
}

TEST_F(KubectlRmiTest, UpdateLogsWritesEditorFileAndGuardsAgainstStaleTarget) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);

  dynamic ok;
  ok["name"_key] = std::string{"api-7"};
  ok["namespace"_key] = std::string{"prod"};
  ok["title"_key] = std::string{"logs: prod/api-7"};
  ok["text"_key] = std::string{"line one\nline two\nline three"};
  call("update_logs"_key, std::move(ok));
  const std::string first = editor_file(root_ + "_logs.vbox.editor");
  EXPECT_EQ(first.rfind("private/", 0), 0u) << first;
  EXPECT_EQ(editor_text(root_ + "_logs.vbox.editor"), "line one\nline two\nline three");

  dynamic stale;
  stale["name"_key] = std::string{"other"};
  stale["namespace"_key] = std::string{"prod"};
  stale["title"_key] = std::string{"stale"};
  stale["text"_key] = std::string{"a\nb\nc\nd\ne"};
  call("update_logs"_key, std::move(stale));
  EXPECT_EQ(editor_file(root_ + "_logs.vbox.editor"), first);
  EXPECT_EQ(editor_text(root_ + "_logs.vbox.editor"), "line one\nline two\nline three");

  // Every update gets a fresh file (the editor reloads only on a file_path
  // change) and the one it replaced is deleted.
  dynamic again;
  again["name"_key] = std::string{"api-7"};
  again["namespace"_key] = std::string{"prod"};
  again["title"_key] = std::string{"logs"};
  again["text"_key] = std::string{"line four"};
  call("update_logs"_key, std::move(again));
  EXPECT_NE(editor_file(root_ + "_logs.vbox.editor"), first);
  EXPECT_EQ(editor_text(root_ + "_logs.vbox.editor"), "line four");
  EXPECT_FALSE(std::filesystem::exists(srv_->last_session->resource_dir / first));
}

TEST_F(KubectlRmiTest, ClosingDeletesLogsEditorFile) {
  call(
      "update_pods"_key,
      make_list_args(
          "pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);
  dynamic ok;
  ok["name"_key] = std::string{"api-7"};
  ok["namespace"_key] = std::string{"prod"};
  ok["title"_key] = std::string{"logs"};
  ok["text"_key] = std::string{"hello"};
  call("update_logs"_key, std::move(ok));
  const auto file = srv_->last_session->resource_dir / editor_file(root_ + "_logs.vbox.editor");
  ASSERT_TRUE(std::filesystem::exists(file));

  bool got = false;
  size_t since = srv_->events->mark();
  auto win = srv_->last_session->ui_objects.at(root_ + "_logs")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_logs", win, "closed"_key);
  got = srv_->events->wait_for("closed"_key, since).has_value();
  ASSERT_TRUE(got);
  EXPECT_FALSE(std::filesystem::exists(file));
}

TEST_F(KubectlRmiTest, LogsFollowCheckboxReEmitsWithFollowTrue) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);

  size_t since = srv_->events->mark();
  dynamic p;
  p["value"_key] = true;
  fire_at(root_ + "_logs", id_at(root_ + "_logs.vbox.toolbar.follow"), "changed"_key, std::move(p));
  // Match on follow == true: the Logs click above also emits a
  // logs_requested (follow = false) that may be delivered after the mark.
  EXPECT_TRUE(
      srv_->events->wait_for("logs_requested"_key, since, [](const dynamic& e) { return e.as<bool>("follow"_key); }));
}

TEST_F(KubectlRmiTest, DescribeMenuActionEmitsAndUpdateDescribeFills) {
  call("update_services"_key,
       make_list_args("services", {{{"namespace", "default"}, {"name", "web"}, {"type", "ClusterIP"}, {"cluster_ip", "10.0.0.1"}, {"ports", "80"}, {"age", "2d"}}}));

  bool got = false;
  dynamic cap;
  size_t since = srv_->events->mark();
  fire_at(root_ + "_services", menu_id_in(root_ + "_services.vbox.table", 0, "Describe"), "clicked"_key);
  if (auto ev = srv_->events->wait_for("describe_requested"_key, since)) {
    got = true;
    cap = std::move(*ev);
  }
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("kind"_key), "service");
  EXPECT_EQ(cap.as<std::string>("name"_key), "web");

  dynamic resp;
  resp["kind"_key] = std::string{"service"};
  resp["name"_key] = std::string{"web"};
  resp["namespace"_key] = std::string{"default"};
  resp["title"_key] = std::string{"service: default/web"};
  const std::string resp_text = "Name:  web\nNamespace:  default\nType:  ClusterIP";
  resp["text"_key] = resp_text;
  call("update_describe"_key, std::move(resp));
  EXPECT_EQ(editor_text(root_ + "_describe.vbox.editor"), resp_text);
}

TEST_F(KubectlRmiTest, ClosingAnyWindowEmitsClosed) {
  bool got = false;
  size_t since = srv_->events->mark();
  auto win = srv_->last_session->ui_objects.at(root_ + "_services")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_services", win, "closed"_key);
  got = srv_->events->wait_for("closed"_key, since).has_value();
  EXPECT_TRUE(got);
}

// ── Default dock layout ──────────────────────────────────────────────────────

TEST_F(KubectlRmiTest, RegistersDefaultDockLayout) {
  const std::string dl = find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_");
  ASSERT_FALSE(dl.empty());
  auto& obj = srv_->last_session->ui_objects.at(dl);
  // kubectl opts into its own nested dockspace (dock::viewport(...)), so the
  // registered root is the embedded DockSpaceViewport wrapping the actual
  // DockLayout, not a bare DockLayout.
  EXPECT_EQ(obj->class_key(), "DockSpaceViewport"_key);
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{dl}));

  std::vector<std::string> tokens;
  std::function<void(bdg::wish::ui_element&)> walk = [&](bdg::wish::ui_element& n) {
    if (n.class_key() == "DockArea"_key) {
      const std::string& w = n.findField("windows"_key)->as<std::string>();
      size_t start = 0;
      while (start <= w.size()) {
        size_t nl = w.find('\n', start);
        tokens.push_back(w.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos)
          break;
        start = nl + 1;
      }
    }
    n.for_each_child_ordered([&](bison::key_t, bdg::wish::ui_element& c) { walk(c); });
  };
  walk(*obj);

  ASSERT_FALSE(tokens.empty());
  for (const auto& t : tokens)
    EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{t})) << "unresolved DockArea window: " << t;
}

TEST_F(KubectlRmiTest, ClosingAWindowAlsoTearsDownDockLayout) {
  ASSERT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_").empty());
  auto win = srv_->last_session->ui_objects.at(root_ + "_top")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_top", win, "closed"_key); // kubectl's on_event tears down synchronously
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_").empty());
}

// ── Console window (client `kubectl` subprocess trace) ────────────────────

TEST_F(KubectlRmiTest, InstantiationBuildsConsoleWindow) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_console.vbox.table"));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_console"}));
}

TEST_F(KubectlRmiTest, AppendCommandLogAddsRowsAndClearConsoleEmptiesThem) {
  auto log_args = [](const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
    dynamic a;
    a["command"_key] = command;
    a["exit_code"_key] = exit_code;
    a["ok"_key] = ok;
    a["output"_key] = output;
    return a;
  };

  EXPECT_EQ(row_count(root_ + "_console.vbox.table"), 0u);
  call("append_command_log"_key, log_args("kubectl get pods -A", 0, true, ""));
  EXPECT_EQ(row_count(root_ + "_console.vbox.table"), 1u);
  call("append_command_log"_key, log_args("kubectl delete pod x -n d", 1, false, "not found"));
  EXPECT_EQ(row_count(root_ + "_console.vbox.table"), 2u);

  auto clear = menu_id_in(root_ + "_console.vbox.table", 0, "Clear Console");
  ASSERT_NE(clear.id, 0u);
  fire_at(root_ + "_console", clear, "clicked"_key);
  EXPECT_EQ(row_count(root_ + "_console.vbox.table"), 0u);
}

// ── Top window (live `kubectl top` graphs) ──────────────────────────────

namespace {
dynamic make_top_args(
    const std::vector<std::tuple<std::string, std::string, float, float>>& pods,
    const std::vector<std::tuple<std::string, float, float>>& nodes, const std::string& error = {}) {
  dynamic args;
  dynamic pods_arr;
  dynamic nodes_arr;
  size_t i = 0;
  for (auto& [ns, name, cpu_m, mem_mib] : pods) {
    auto e = std::make_shared<dynamic>();
    (*e)["namespace"_key] = ns;
    (*e)["name"_key] = name;
    (*e)["cpu"_key] = std::string{"1m"};
    (*e)["cpu_m"_key] = cpu_m;
    (*e)["mem"_key] = std::string{"1Mi"};
    (*e)["mem_mib"_key] = mem_mib;
    pods_arr[i++] = dynamic_ptr{e};
  }
  size_t j = 0;
  for (auto& [name, cpu_pct, mem_pct] : nodes) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = name;
    (*e)["cpu"_key] = std::string{"100m"};
    (*e)["cpu_m"_key] = 100.0f;
    (*e)["cpu_pct"_key] = cpu_pct;
    (*e)["mem"_key] = std::string{"500Mi"};
    (*e)["mem_mib"_key] = 500.0f;
    (*e)["mem_pct"_key] = mem_pct;
    nodes_arr[j++] = dynamic_ptr{e};
  }
  args["pods"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(pods_arr))};
  args["nodes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(nodes_arr))};
  if (!error.empty())
    args["error"_key] = error;
  return args;
}
} // namespace

TEST_F(KubectlRmiTest, InstantiationBuildsTopWindow) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_top.vbox.pods_cpu_plot"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_top.vbox.nodes_mem_plot"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_top.vbox.pods_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_top.vbox.nodes_table"));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_top"}));
  EXPECT_EQ(child_elem_count(root_ + "_top.vbox.pods_cpu_plot"), 1u); // aggregate only
}

TEST_F(KubectlRmiTest, UpdateStatsAddsPerPodAndPerNodeLines) {
  call("update_stats"_key, make_top_args(
                               {{"default", "web", 20.0f, 64.0f}, {"kube-system", "coredns", 5.0f, 14.0f}},
                               {{"node-a", 30.0f, 40.0f}}));
  EXPECT_EQ(child_elem_count(root_ + "_top.vbox.pods_cpu_plot"), 3u);  // aggregate + 2 pods
  EXPECT_EQ(child_elem_count(root_ + "_top.vbox.nodes_cpu_plot"), 2u); // aggregate + 1 node
  EXPECT_EQ(row_count(root_ + "_top.vbox.pods_table"), 2u);
  EXPECT_EQ(row_count(root_ + "_top.vbox.nodes_table"), 1u);

  auto status = srv_->last_session->ui_objects.at(root_ + "_top.vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("2 pods, 1 nodes"), std::string::npos);
}

TEST_F(KubectlRmiTest, UpdateStatsDropsSeriesForVanishedPods) {
  call("update_stats"_key, make_top_args(
                               {{"default", "web", 20.0f, 64.0f}, {"kube-system", "coredns", 5.0f, 14.0f}},
                               {{"node-a", 30.0f, 40.0f}}));
  EXPECT_EQ(child_elem_count(root_ + "_top.vbox.pods_cpu_plot"), 3u);

  call("update_stats"_key, make_top_args({{"default", "web", 25.0f, 70.0f}}, {{"node-a", 33.0f, 42.0f}}));
  EXPECT_EQ(child_elem_count(root_ + "_top.vbox.pods_cpu_plot"), 2u); // aggregate + web
  EXPECT_EQ(row_count(root_ + "_top.vbox.pods_table"), 1u);
}

TEST_F(KubectlRmiTest, UpdateStatsErrorShownInStatusLabel) {
  call("update_stats"_key, make_top_args({}, {}, "metrics not available (metrics-server not installed)"));
  auto status = srv_->last_session->ui_objects.at(root_ + "_top.vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("metrics-server not installed"), std::string::npos);
}
