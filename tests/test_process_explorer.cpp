// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <registry.hpp>
#include <server.hpp>
#include <session.hpp>
#include <ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// Builds the same `update_snapshot` args shape the reference client
// (app/wish_cli/client/apps/process_explorer/process_explorer.cpp) sends --
// duplicated here rather than shared, since tests exercise the server's
// documented wire contract independent of any particular client.
struct fake_process {
  int32_t pid;
  std::string name;
  std::string command;
  std::string state;
  float cpu_percent;
  float mem_rss_bytes;
};

// bison::dynamic fields only support float (not double) among floating-
// point alternatives -- matches the reference client's encode_snapshot().
dynamic make_snapshot_args(
    float cpu_percent,
    std::vector<float> per_core_percent,
    float mem_total_bytes,
    float mem_used_bytes,
    const std::vector<fake_process>& processes) {
  dynamic args;
  args["cpu_percent"_key] = cpu_percent;
  args["per_core_percent"_key] = std::move(per_core_percent);
  args["mem_total_bytes"_key] = mem_total_bytes;
  args["mem_used_bytes"_key] = mem_used_bytes;

  dynamic procs;
  size_t i = 0;
  for (auto& p : processes) {
    auto e = std::make_shared<dynamic>();
    (*e)["pid"_key] = p.pid;
    (*e)["name"_key] = p.name;
    (*e)["command"_key] = p.command;
    (*e)["state"_key] = p.state;
    (*e)["cpu_percent"_key] = p.cpu_percent;
    (*e)["mem_rss_bytes"_key] = p.mem_rss_bytes;
    procs[i++] = dynamic_ptr{e};
  }
  args["processes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(procs))};
  return args;
}

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class ProcessExplorerLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(ProcessExplorerLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "ProcessExplorer"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "ProcessExplorer"_key);
}

TEST_F(ProcessExplorerLocalTest, DefaultTitle) {
  auto obj = dynamic::instantiate("wish"_key, "ProcessExplorer"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Process Explorer");
}

// ── Session-capturing server for internal-tree tests ──────────────────────────

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::session* last_session{nullptr};

 protected:
  void on_session_created(wish::session& s) override {
    last_session = &s;
  }
};

// Helper: find the root key for the internal form tree (starts with
// "__procexp_", no dot -- i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__procexp_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class ProcessExplorerWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "ProcessExplorer"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(ProcessExplorerWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __procexp_... root key in session.objects";
}

TEST_F(ProcessExplorerWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(ProcessExplorerWindowTest, TreeContainsSummaryLabels) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.summary.cpu_label"));
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.summary.mem_label"));
}

TEST_F(ProcessExplorerWindowTest, TreeContainsPlots) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.cpu_plot.cpu_series"));
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.mem_plot.mem_series"));
}

TEST_F(ProcessExplorerWindowTest, TreeContainsEmptyProcessTableBeforeAnySnapshot) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto it = srv_->last_session->objects.find(root + ".vbox.proc_table");
  ASSERT_NE(it, srv_->last_session->objects.end());

  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  // dynamic::size() only counts numeric-indexed ("array") entries; the 6
  // TableColumn children are string-keyed and don't count toward it, so a
  // fresh table with no rows yet reports 0, not 6.
  EXPECT_EQ((*cf)->size(), 0u);
}

// ── update_snapshot() reconciliation ─────────────────────────────────────────

class ProcessExplorerSnapshotTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "ProcessExplorer"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->objects);
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  dynamic update_snapshot(
      float cpu_percent,
      std::vector<float> per_core_percent,
      float mem_total_bytes,
      float mem_used_bytes,
      const std::vector<fake_process>& processes) {
    return proxy_
        ->call(
            "update_snapshot"_key,
            make_snapshot_args(cpu_percent, std::move(per_core_percent), mem_total_bytes, mem_used_bytes, processes))
        .get();
  }

  // dynamic::size() only counts numeric-indexed entries, so it already
  // excludes the 6 string-keyed TableColumn children -- no subtraction needed.
  size_t row_count() const {
    auto it = srv_->last_session->objects.find(root_ + ".vbox.proc_table");
    if (it == srv_->last_session->objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  std::string label_text(const std::string& path) const {
    auto it = srv_->last_session->objects.find(path);
    if (it == srv_->last_session->objects.end())
      return {};
    return it->second->as<std::string>("text"_key);
  }

  std::vector<float> plot_field(const std::string& path, bison::key_t field_key) const {
    auto it = srv_->last_session->objects.find(path);
    if (it == srv_->last_session->objects.end())
      return {};
    auto* f = it->second->findField<std::vector<float>>(field_key);
    return f ? *f : std::vector<float>{};
  }

  // Reconstructs visual row order from each TableRow's "order" field and its
  // first cell (the PID Label), independent of pid_to_row_'s internal
  // (unordered) iteration order.
  std::vector<int> row_pids_in_order() const {
    auto it = srv_->last_session->objects.find(root_ + ".vbox.proc_table");
    if (it == srv_->last_session->objects.end())
      return {};
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    std::vector<std::pair<int32_t, int>> ordered;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (!f.is<dynamic_ptr>())
        return;
      auto row = f.as<dynamic_ptr>();
      if (!row)
        return;
      int32_t order = row->as<int32_t>("order"_key);
      auto* rc = row->findField<dynamic_ptr>("children"_key);
      if (!rc || !*rc)
        return;
      auto& pid_field = (*rc)->at(size_t{0});
      if (!pid_field.is<dynamic_ptr>())
        return;
      auto pid_label = pid_field.as<dynamic_ptr>();
      if (!pid_label)
        return;
      ordered.push_back({order, std::stoi(pid_label->as<std::string>("text"_key))});
    });
    std::sort(ordered.begin(), ordered.end());
    std::vector<int> result;
    result.reserve(ordered.size());
    for (auto& [order, pid] : ordered)
      result.push_back(pid);
    return result;
  }

  // Mirrors the imgui renderer's "sorted" event (see render_table in
  // imgui_ui_renderer.cpp): simulates a column-header click without needing
  // a real ImGui frame.
  void simulate_sort(int32_t column_id, bool ascending) {
    auto table_id = srv_->last_session->objects.at(root_ + ".vbox.proc_table")->as<bison::key_t>("__wish_id"_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    dynamic payload;
    payload["column_id"_key] = column_id;
    payload["ascending"_key] = ascending;
    h->second->on_event(table_id, "sorted"_key, std::move(payload));
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

TEST_F(ProcessExplorerSnapshotTest, UpdatesSummaryLabels) {
  update_snapshot(37.5, {}, 1000.0, 400.0, {});

  EXPECT_EQ(label_text(root_ + ".vbox.summary.cpu_label"), "CPU: 37.5%");
  EXPECT_NE(label_text(root_ + ".vbox.summary.mem_label").find("40.0%"), std::string::npos);
}

// Regression test: the ImPlot renderer plots min(xs.size(), ys.size())
// points, so a series with populated "ys" but empty "xs" silently renders
// nothing. Both plot series must get a matching "xs" alongside "ys".
TEST_F(ProcessExplorerSnapshotTest, PlotSeriesGetMatchingXsAndYs) {
  update_snapshot(10.0, {}, 1000.0, 100.0, {});
  update_snapshot(20.0, {}, 1000.0, 200.0, {});

  auto cpu_xs = plot_field(root_ + ".vbox.cpu_plot.cpu_series", "xs"_key);
  auto cpu_ys = plot_field(root_ + ".vbox.cpu_plot.cpu_series", "ys"_key);
  ASSERT_EQ(cpu_xs.size(), cpu_ys.size());
  ASSERT_EQ(cpu_xs.size(), 2u);
  EXPECT_FLOAT_EQ(cpu_ys[0], 10.0f);
  EXPECT_FLOAT_EQ(cpu_ys[1], 20.0f);

  auto mem_xs = plot_field(root_ + ".vbox.mem_plot.mem_series", "xs"_key);
  auto mem_ys = plot_field(root_ + ".vbox.mem_plot.mem_series", "ys"_key);
  ASSERT_EQ(mem_xs.size(), mem_ys.size());
  ASSERT_EQ(mem_xs.size(), 2u);
}

TEST_F(ProcessExplorerSnapshotTest, FirstCallSizesCoreMeters) {
  update_snapshot(10.0, {20.0f, 30.0f, 40.0f, 50.0f}, 1000.0, 100.0, {});

  auto it = srv_->last_session->objects.find(root_ + ".vbox.cores");
  ASSERT_NE(it, srv_->last_session->objects.end());
  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  EXPECT_EQ((*cf)->size(), 4u);
}

TEST_F(ProcessExplorerSnapshotTest, NewProcessAddsRow) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 1024.0 * 1024.0}});

  EXPECT_EQ(row_count(), 1u);
}

TEST_F(ProcessExplorerSnapshotTest, ExistingProcessUpdatesInPlaceWithoutDuplicating) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 1024.0 * 1024.0}});
  ASSERT_EQ(row_count(), 1u);

  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 42.0, 2.0 * 1024.0 * 1024.0}});
  EXPECT_EQ(row_count(), 1u);
}

TEST_F(ProcessExplorerSnapshotTest, VanishedProcessRemovesRow) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  ASSERT_EQ(row_count(), 1u);

  update_snapshot(5.0, {}, 1000.0, 100.0, {}); // pid 100 no longer present
  EXPECT_EQ(row_count(), 0u);
}

TEST_F(ProcessExplorerSnapshotTest, SecondProcessAddsSecondRow) {
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{100, "init", "[init]", "S", 5.0, 0.0}, {200, "sshd", "[sshd]", "S", 1.0, 0.0}});
  EXPECT_EQ(row_count(), 2u);
}

// ── Column-header sorting ─────────────────────────────────────────────────────

TEST_F(ProcessExplorerSnapshotTest, DefaultsToCpuPercentDescending) {
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{1, "low", "[low]", "S", 10.0, 0.0}, {2, "high", "[high]", "S", 50.0, 0.0}, {3, "mid", "[mid]", "S", 30.0, 0.0}});

  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{2, 3, 1}));
}

TEST_F(ProcessExplorerSnapshotTest, SortedEventByPidReorders) {
  // CPU% (the default sort column) intentionally does NOT correlate with
  // PID order here, so a passing test can only mean the "sorted" event --
  // not the default criterion -- drove the resulting order.
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{30, "c", "[c]", "S", 10.0, 0.0}, {10, "a", "[a]", "S", 50.0, 0.0}, {20, "b", "[b]", "S", 30.0, 0.0}});

  simulate_sort(0, /*ascending=*/false); // PID descending
  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{30, 20, 10}));

  simulate_sort(0, /*ascending=*/true); // PID ascending
  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{10, 20, 30}));
}

TEST_F(ProcessExplorerSnapshotTest, SortedEventByNameDescendingReorders) {
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{1, "alpha", "[alpha]", "S", 10.0, 0.0}, {2, "beta", "[beta]", "S", 10.0, 0.0}, {3, "gamma", "[gamma]", "S", 10.0, 0.0}});

  simulate_sort(1, /*ascending=*/false); // Name descending
  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{3, 2, 1})); // gamma, beta, alpha
}

TEST_F(ProcessExplorerSnapshotTest, SortCriterionPersistsAcrossSubsequentSnapshots) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{1, "a", "[a]", "S", 90.0, 0.0}, {2, "b", "[b]", "S", 10.0, 0.0}});
  simulate_sort(0, /*ascending=*/true); // PID ascending, overriding the CPU%-descending default
  ASSERT_EQ(row_pids_in_order(), (std::vector<int>{1, 2}));

  // A later snapshot (even one that would reorder under the old default)
  // must keep applying the user's chosen criterion, not reset to it.
  update_snapshot(5.0, {}, 1000.0, 100.0, {{1, "a", "[a]", "S", 5.0, 0.0}, {2, "b", "[b]", "S", 95.0, 0.0}});
  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{1, 2}));
}

// ── Event routing ─────────────────────────────────────────────────────────────

TEST_F(ProcessExplorerSnapshotTest, WindowClosedEmitsClosedAndCleansUp) {
  bool got_closed = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got_closed = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  auto win_id = srv_->last_session->objects.at(root_)->as<bison::key_t>("__wish_id"_key);
  auto h = srv_->last_session->top_level_handlers.find(root_);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(win_id, "closed"_key, dynamic{});

  // form::emit() defers delivery to the render loop's next frame (see
  // session.hpp's contract on emit_event), so spin briefly for it, same
  // idiom as test_integration.cpp's event round-trip test.
  auto t0 = std::chrono::steady_clock::now();
  while (!got_closed && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  EXPECT_TRUE(got_closed);
  EXPECT_EQ(srv_->last_session->objects.count(root_), 0u);
}
