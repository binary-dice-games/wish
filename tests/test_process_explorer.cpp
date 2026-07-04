// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <forms/process_explorer/process_explorer.hpp>
#include <registry.hpp>
#include <server.hpp>
#include <session.hpp>
#include <ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/server/context.hpp"

#include <string>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// Exposes process_explorer's protected static reconciliation logic (and the
// refresh_state type it operates on) without ever constructing a
// process_explorer instance -- so these tests exercise the row add/update/
// remove/sort logic directly with synthetic data, with no server, no
// session, and no background thread involved at all.
class TestableProcessExplorer : public wish::process_explorer {
 public:
  using wish::process_explorer::apply_snapshot;
  using wish::process_explorer::refresh_state;
};

wish::process_sample make_process(int pid, std::string name, double cpu_percent, uint64_t rss_bytes) {
  wish::process_sample p;
  p.pid = pid;
  p.name = std::move(name);
  p.command = "[" + p.name + "]";
  p.state = 'S';
  p.cpu_percent = cpu_percent;
  p.mem_rss_bytes = rss_bytes;
  return p;
}

} // namespace

// ── Pure reconciliation logic (no server/session/thread involved) ───────────

class ProcessExplorerSnapshotTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wish::register_all();
    table_ = ui_element_t{dynamic::instantiate("wish"_key, "Table"_key)};
    // Explicit private children map, matching the technique notepad.cpp uses
    // for tab_bar -- otherwise this instance could share the Element base
    // prototype's default children map.
    (*table_)["children"_key] = dynamic_ptr{bison::key_t{0U}, {}};
    state_.ctx = &ctx_;
    state_.proc_table = table_;
  }

  using ui_element_t = wish::ui_element_ptr;

  size_t row_count() const {
    auto* cf = table_->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  bison::rmi::context ctx_;
  ui_element_t table_;
  TestableProcessExplorer::refresh_state state_;
};

TEST_F(ProcessExplorerSnapshotTest, NewProcessAddsRow) {
  wish::system_snapshot snap;
  snap.processes.push_back(make_process(100, "init", 5.0, 1024 * 1024));

  TestableProcessExplorer::apply_snapshot(state_, snap);

  EXPECT_EQ(state_.pid_to_row.size(), 1u);
  EXPECT_EQ(row_count(), 1u);
  ASSERT_TRUE(state_.pid_to_row.count(100));
  EXPECT_EQ(state_.pid_to_row.at(100).name_label->as<std::string>("text"_key), "init");
}

TEST_F(ProcessExplorerSnapshotTest, ExistingProcessUpdatesInPlaceWithoutDuplicating) {
  wish::system_snapshot first;
  first.processes.push_back(make_process(100, "init", 5.0, 1024 * 1024));
  TestableProcessExplorer::apply_snapshot(state_, first);

  wish::system_snapshot second;
  second.processes.push_back(make_process(100, "init", 42.0, 2 * 1024 * 1024));
  TestableProcessExplorer::apply_snapshot(state_, second);

  EXPECT_EQ(state_.pid_to_row.size(), 1u);
  EXPECT_EQ(row_count(), 1u);
  EXPECT_DOUBLE_EQ(state_.pid_to_row.at(100).cpu_percent, 42.0);
  EXPECT_FLOAT_EQ(state_.pid_to_row.at(100).cpu_bar->as<float>("value"_key), 0.42f);
}

TEST_F(ProcessExplorerSnapshotTest, VanishedProcessRemovesRow) {
  wish::system_snapshot first;
  first.processes.push_back(make_process(100, "init", 5.0, 1024 * 1024));
  TestableProcessExplorer::apply_snapshot(state_, first);
  ASSERT_EQ(row_count(), 1u);

  wish::system_snapshot second; // pid 100 no longer present
  TestableProcessExplorer::apply_snapshot(state_, second);

  EXPECT_EQ(state_.pid_to_row.size(), 0u);
  EXPECT_EQ(row_count(), 0u);
}

TEST_F(ProcessExplorerSnapshotTest, RowsAreSortedByCpuPercentDescending) {
  wish::system_snapshot snap;
  snap.processes.push_back(make_process(1, "low", 10.0, 0));
  snap.processes.push_back(make_process(2, "high", 50.0, 0));
  snap.processes.push_back(make_process(3, "mid", 30.0, 0));
  TestableProcessExplorer::apply_snapshot(state_, snap);

  EXPECT_EQ(state_.pid_to_row.at(2).row->as<int32_t>("order"_key), 0); // high (50%) first
  EXPECT_EQ(state_.pid_to_row.at(3).row->as<int32_t>("order"_key), 1); // mid (30%) second
  EXPECT_EQ(state_.pid_to_row.at(1).row->as<int32_t>("order"_key), 2); // low (10%) last
}

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class ProcessExplorerLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wish::register_all();
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

// ── Internal Window construction (real server; exercises on_init() once) ────

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
// "__procexp_", no dot -- i.e. it is the top-level entry, not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__procexp_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

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

  // Instantiating runs on_init() synchronously to completion (including the
  // one real /proc sample and the initial apply_snapshot() call) before the
  // background refresh thread's very first ~1s sleep chunk elapses, so
  // reading the resulting tree immediately afterward is race-free.
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

TEST_F(ProcessExplorerWindowTest, TreeContainsProcessTableWithAtLeastOneRow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto it = srv_->last_session->objects.find(root + ".vbox.proc_table");
  ASSERT_NE(it, srv_->last_session->objects.end());

  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  // Columns (6) plus at least one TableRow -- this test process itself is
  // always present in /proc, so on_init()'s one real sample always yields
  // at least one row.
  EXPECT_GT((*cf)->size(), 6u);
}

TEST_F(ProcessExplorerWindowTest, WindowClosedEmitsClosedAndCleansUp) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());

  bool got_closed = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got_closed = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  auto win_id = srv_->last_session->objects.at(root)->as<bison::key_t>("__wish_id"_key);
  auto h = srv_->last_session->top_level_handlers.find(root);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(win_id, "closed"_key, dynamic{});

  EXPECT_TRUE(got_closed);
  EXPECT_EQ(srv_->last_session->objects.count(root), 0u);
}

// ── Lifecycle: destroy immediately after instantiate; must not hang/crash ───

TEST(ProcessExplorerLifecycleTest, DestroysCleanlyWithoutHanging) {
  memory_server_transport transport;
  auto srv = std::make_unique<SessionCapturingServer>(transport, std::make_unique<wish::null_renderer>());
  srv->start();
  auto client = std::make_unique<bdg::bison::rmi::client>(transport.connect());
  client->connect();

  client->instantiate("wish"_key, "ProcessExplorer"_key).get();

  // Disconnecting tears the session (and the form, and its refresh thread)
  // down immediately -- well before the first ~1s refresh tick -- exercising
  // the destructor's stop+join path under time pressure.
  client->disconnect();
  client.reset();
  srv->stop();
  srv.reset();
}
