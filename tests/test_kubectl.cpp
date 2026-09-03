// MIT License © 2026 Binary Dice Games
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

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
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

  static void wait_for(const bool& flag) {
    auto t0 = std::chrono::steady_clock::now();
    while (!flag && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_logs.vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_describe.vbox.table"));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_nodes"}));
}

// Both scroll axes must be enabled on every scrollable table: ImGui has no
// single "Scroll" flag, so the descriptor lists ScrollX|ScrollY explicitly.
// The single-column Logs/Describe line tables additionally use a WidthFixed
// col_line so ScrollX has a content width to pan over (a WidthStretch column
// is clamped to the viewport and long lines get clipped, not scrolled).
TEST_F(KubectlRmiTest, ScrollableTablesEnableBothScrollAxes) {
  constexpr int32_t kScrollX = 1 << 24;
  constexpr int32_t kScrollY = 1 << 25;
  constexpr int32_t kWidthFixed = 1 << 4;
  for (const char* suffix : {".vbox.table", "_deployments.vbox.table", "_services.vbox.table",
                             "_nodes.vbox.table", "_logs.vbox.table", "_describe.vbox.table",
                             "_console.vbox.table"}) {
    auto it = srv_->last_session->ui_objects.find(root_ + suffix);
    ASSERT_NE(it, srv_->last_session->ui_objects.end()) << suffix;
    int32_t flags = it->second->as<int32_t>("flags"_key);
    EXPECT_TRUE(flags & kScrollX) << suffix;
    EXPECT_TRUE(flags & kScrollY) << suffix;
  }
  for (const char* col_path : {"_logs.vbox.table.col_line", "_describe.vbox.table.col_line"}) {
    auto it = srv_->last_session->ui_objects.find(root_ + col_path);
    ASSERT_NE(it, srv_->last_session->ui_objects.end()) << col_path;
    EXPECT_TRUE(it->second->as<int32_t>("flags"_key) & kWidthFixed) << col_path;
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

  dynamic cap;
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "pod_action_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire(menu_id_in(root_ + ".vbox.table", 0, "Delete..."), "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got) << "delete must be held back pending confirmation";

  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto msg = srv_->last_session->ui_objects.at(cr + ".body.message")->as<std::string>("text"_key);
  EXPECT_NE(msg.find("api-7"), std::string::npos);
  EXPECT_NE(msg.find("prod"), std::string::npos);

  auto yes = srv_->last_session->ui_objects.at(cr + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);
  fire_at(cr, yes, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("name"_key), "api-7");
  EXPECT_EQ(cap.as<std::string>("namespace"_key), "prod");
  EXPECT_EQ(cap.as<std::string>("action"_key), "delete");
}

TEST_F(KubectlRmiTest, DeleteNoCancelsWithoutEmitting) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "d"}, {"name", "p"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Delete..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto no = srv_->last_session->ui_objects.at(cr + ".buttons.btn1")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "pod_action_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(cr, no, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
}

TEST_F(KubectlRmiTest, DeploymentRestartFiresImmediately) {
  call("update_deployments"_key,
       make_list_args("deployments", {{{"namespace", "default"}, {"name", "web"}, {"ready", "3/3"}, {"uptodate", "3"}, {"available", "3"}, {"age", "10d"}}}));

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "deployment_action_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(root_ + "_deployments", menu_id_in(root_ + "_deployments.vbox.table", 0, "Restart"), "clicked"_key);
  wait_for(got);
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
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "node_action_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(cr, yes, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("name"_key), "node-a");
  EXPECT_EQ(cap.as<std::string>("action"_key), "drain");
  EXPECT_FALSE(cap.findField<std::string>("namespace"_key));
}

TEST_F(KubectlRmiTest, RefreshButtonEmitsRefreshRequested) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "refresh_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire(id_at(root_ + ".vbox.toolbar.btn_refresh"), "clicked"_key);
  wait_for(got);
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
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "logs_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("name"_key), "api-7");
  EXPECT_EQ(cap.as<std::string>("namespace"_key), "prod");
  EXPECT_FALSE(cap.as<bool>("follow"_key));

  auto target = srv_->last_session->ui_objects.at(root_ + "_logs.vbox.toolbar.target")->as<std::string>("text"_key);
  EXPECT_NE(target.find("api-7"), std::string::npos);
}

TEST_F(KubectlRmiTest, UpdateLogsSplitsTextAndGuardsAgainstStaleTarget) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);

  dynamic ok;
  ok["name"_key] = std::string{"api-7"};
  ok["namespace"_key] = std::string{"prod"};
  ok["title"_key] = std::string{"logs: prod/api-7"};
  ok["text"_key] = std::string{"line one\nline two\nline three"};
  call("update_logs"_key, std::move(ok));
  EXPECT_EQ(row_count(root_ + "_logs.vbox.table"), 3u);

  dynamic stale;
  stale["name"_key] = std::string{"other"};
  stale["namespace"_key] = std::string{"prod"};
  stale["title"_key] = std::string{"stale"};
  stale["text"_key] = std::string{"a\nb\nc\nd\ne"};
  call("update_logs"_key, std::move(stale));
  EXPECT_EQ(row_count(root_ + "_logs.vbox.table"), 3u);
}

TEST_F(KubectlRmiTest, LogsFollowCheckboxReEmitsWithFollowTrue) {
  call("update_pods"_key,
       make_list_args("pods", {{{"namespace", "prod"}, {"name", "api-7"}, {"phase", "Running"}, {"ready", "1/1"}, {"age", "1d"}}}));
  fire(menu_id_in(root_ + ".vbox.table", 0, "Logs"), "clicked"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "logs_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  dynamic p;
  p["value"_key] = true;
  fire_at(root_ + "_logs", id_at(root_ + "_logs.vbox.toolbar.follow"), "changed"_key, std::move(p));
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_TRUE(cap.as<bool>("follow"_key));
}

TEST_F(KubectlRmiTest, DescribeMenuActionEmitsAndUpdateDescribeFills) {
  call("update_services"_key,
       make_list_args("services", {{{"namespace", "default"}, {"name", "web"}, {"type", "ClusterIP"}, {"cluster_ip", "10.0.0.1"}, {"ports", "80"}, {"age", "2d"}}}));

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "describe_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(root_ + "_services", menu_id_in(root_ + "_services.vbox.table", 0, "Describe"), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("kind"_key), "service");
  EXPECT_EQ(cap.as<std::string>("name"_key), "web");

  dynamic resp;
  resp["kind"_key] = std::string{"service"};
  resp["name"_key] = std::string{"web"};
  resp["namespace"_key] = std::string{"default"};
  resp["title"_key] = std::string{"service: default/web"};
  resp["text"_key] = std::string{"Name:  web\nNamespace:  default\nType:  ClusterIP"};
  call("update_describe"_key, std::move(resp));
  EXPECT_EQ(row_count(root_ + "_describe.vbox.table"), 3u);
}

TEST_F(KubectlRmiTest, ClosingAnyWindowEmitsClosed) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };
  auto win = srv_->last_session->ui_objects.at(root_ + "_services")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_services", win, "closed"_key);
  wait_for(got);
  EXPECT_TRUE(got);
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
