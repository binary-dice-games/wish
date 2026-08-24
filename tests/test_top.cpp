// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

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
// (app/wish_cli/client/apps/top/top.cpp) sends --
// duplicated here rather than shared, since tests exercise the server's
// documented wire contract independent of any particular client.
struct fake_process {
  int32_t pid;
  std::string name;
  std::string command;
  std::string state;
  float cpu_percent;
  float mem_rss_bytes;
  int32_t nice_value{0};
  std::vector<int32_t> affinity_cores{};
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
    (*e)["nice_value"_key] = p.nice_value;
    (*e)["affinity_cores"_key] = p.affinity_cores;
    procs[i++] = dynamic_ptr{e};
  }
  args["processes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(procs))};
  return args;
}

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class TopLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(TopLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Top"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "Top"_key);
}

TEST_F(TopLocalTest, DefaultTitle) {
  auto obj = dynamic::instantiate("wish"_key, "Top"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Top");
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
// "__top_", no dot -- i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__top_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Internal Window construction ─────────────────────────────────────────────

class TopWindowTest : public ::testing::Test {
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
    client_->instantiate("wish"_key, "Top"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(TopWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __top_... root key in session.objects";
}

TEST_F(TopWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(TopWindowTest, TreeContainsSummaryLabels) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.summary.cpu_label"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.summary.mem_label"));
}

TEST_F(TopWindowTest, TreeContainsPlots) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.cpu_plot.cpu_series"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.mem_plot.mem_series"));
}

TEST_F(TopWindowTest, TreeContainsEmptyProcessTableBeforeAnySnapshot) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto it = srv_->last_session->ui_objects.find(root + ".vbox.proc_table");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());

  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  // dynamic::size() only counts numeric-indexed ("array") entries; the 6
  // TableColumn children are string-keyed and don't count toward it, so a
  // fresh table with no rows yet reports 0, not 6.
  EXPECT_EQ((*cf)->size(), 0u);
}

// ── update_snapshot() reconciliation ─────────────────────────────────────────

class TopSnapshotTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Top"_key).get());
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
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.proc_table");
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  std::string label_text(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return it->second->as<std::string>("text"_key);
  }

  std::vector<float> plot_field(const std::string& path, bison::key_t field_key) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    auto* f = it->second->findField<std::vector<float>>(field_key);
    return f ? *f : std::vector<float>{};
  }

  // Reconstructs visual row order from each TableRow's "order" field and its
  // first cell (the PID Label), independent of pid_to_row_'s internal
  // (unordered) iteration order.
  std::vector<int> row_pids_in_order() const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.proc_table");
    if (it == srv_->last_session->ui_objects.end())
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
    auto table_id = srv_->last_session->ui_objects.at(root_ + ".vbox.proc_table")->as<bison::key_t>("__wish_id"_key);
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    dynamic payload;
    payload["column_id"_key] = column_id;
    payload["ascending"_key] = ascending;
    h->second->on_event(table_id, "sorted"_key, std::move(payload));
  }

  // ── Context-menu test helpers ────────────────────────────────────────────
  //
  // Row cells (and their ContextMenu/MenuItem children) are registered only
  // via ctx().put_object() -- not merged into ui_objects' dot-path map, same
  // as pid_label/name_label/etc. -- so they must be found by walking the
  // Table's "children" field, same technique as row_pids_in_order() above.
  // Confirm-kill/affinity/properties dialogs, by contrast, ARE built via
  // import_json()+ui_objects.merge() (like mc's own confirm
  // dialog), so those are found by dot-path prefix instead (find_root_with_prefix()).

  dynamic_ptr find_row(int pid) const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.proc_table");
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    // NOTE: dynamic_ptr's default constructor (dynamic_ptr(key_t klass =
    // 0U)) allocates a fresh empty dynamic rather than a null shared_ptr, so
    // a plain `dynamic_ptr found;` is truthy from the start -- a separate
    // bool tracks whether a real match was assigned.
    dynamic_ptr found;
    bool has_found = false;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (has_found || !f.is<dynamic_ptr>())
        return;
      auto row = f.as<dynamic_ptr>();
      if (!row)
        return;
      auto* rc = row->findField<dynamic_ptr>("children"_key);
      if (!rc || !*rc)
        return;
      auto& pid_field = (*rc)->at(size_t{0});
      if (!pid_field.is<dynamic_ptr>())
        return;
      auto pid_label = pid_field.as<dynamic_ptr>();
      if (pid_label && pid_label->as<std::string>("text"_key) == std::to_string(pid)) {
        found = row;
        has_found = true;
      }
    });
    return has_found ? found : nullptr;
  }

  static dynamic_ptr nth_child(const dynamic_ptr& parent, size_t index) {
    if (!parent)
      return nullptr;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    auto& f = (*cf)->at(index);
    if (!f.is<dynamic_ptr>())
      return nullptr;
    return f.as<dynamic_ptr>();
  }

  static bison::key_t element_id(const dynamic_ptr& elem) {
    return elem ? elem->as<bison::key_t>("__wish_id"_key) : bison::key_t{};
  }

  // Row's ContextMenu is its 7th (index 6) child, after the 6 cells.
  dynamic_ptr context_menu_of(int pid) const {
    return nth_child(find_row(pid), 6);
  }

  // ContextMenu children, in build_row_context_menu()'s order.
  dynamic_ptr properties_item_of(int pid) const {
    return nth_child(context_menu_of(pid), 0);
  }
  dynamic_ptr pause_resume_item_of(int pid) const {
    return nth_child(context_menu_of(pid), 2);
  }
  dynamic_ptr kill_item_of(int pid) const {
    return nth_child(context_menu_of(pid), 3);
  }
  dynamic_ptr priority_menu_of(int pid) const {
    return nth_child(context_menu_of(pid), 5);
  }
  dynamic_ptr priority_item_of(int pid, size_t level_index) const {
    return nth_child(priority_menu_of(pid), level_index);
  }
  dynamic_ptr affinity_item_of(int pid) const {
    return nth_child(context_menu_of(pid), 6);
  }

  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }

  // Confirm-kill/properties are privately-instantiated MessageBox/
  // PropertiesDialog form instances (see form::instantiate_child_form()),
  // each registered as its OWN top_level_handlers entry (keyed by ITS OWN
  // root, not root_) -- unlike the old raw-element dialogs, whose widget
  // ids were all routed through top's own on_event(). fire() alone can no
  // longer reach a confirm/properties dialog's buttons; use this instead,
  // with the dialog's own root (e.g. from find_root_with_prefix()).
  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }

  static std::string find_root_with_prefix(const wish::name_map& objects, const std::string& prefix) {
    for (const auto& [k, _] : objects) {
      if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
        return k;
    }
    return {};
  }

  std::string label_text_at(const std::string& dialog_root, const std::string& suffix) const {
    return label_text(dialog_root + "." + suffix);
  }

  // Reads one row's displayed value text out of the Properties dialog's
  // internal ObjectInspector (see properties_dialog.hpp/object_inspector.hpp):
  // @p properties_root is the PropertiesDialog's own root (e.g. from
  // find_root_with_prefix(objects, "__properties_dialog_")); the
  // ObjectInspector itself stamps its Table's rows at an independent path
  // (its own base_path_ counter, not nested under properties_root -- see
  // object_inspector::set_target()'s doc comment), so rows are found by
  // walking the object graph from properties_root + ".vbox.inspector"
  // rather than guessing a path. Matches by @p display_name (the field's
  // DisplayName, e.g. "Parent PID") rather than a numeric row index,
  // mirroring find_row()'s own by-content matching style -- robust against
  // register_process_details_class()'s field declaration order changing.
  // ProcessDetails' PropertiesDialog is always read_only, so every row's
  // value widget is a plain Label; returns "" if no such row is found.
  std::string process_details_value(const std::string& properties_root, const std::string& display_name) const {
    auto it = srv_->last_session->ui_objects.find(properties_root + ".vbox.inspector");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    auto& table_field = (*cf)->at(size_t{0});
    if (!table_field.is<dynamic_ptr>())
      return {};
    auto table = table_field.as<dynamic_ptr>();
    if (!table)
      return {};
    auto* tc = table->findField<dynamic_ptr>("children"_key);
    if (!tc || !*tc)
      return {};
    std::string result;
    (*tc)->forEach([&](bison::key_t, const field& f) {
      if (!result.empty() || !f.is<dynamic_ptr>())
        return;
      auto row = f.as<dynamic_ptr>();
      if (!row)
        return;
      auto* rc = row->findField<dynamic_ptr>("children"_key);
      if (!rc || !*rc) // TableColumn entries share this map but have no "children".
        return;
      auto& name_field = (*rc)->at(size_t{0});
      if (!name_field.is<dynamic_ptr>())
        return;
      auto name_label = name_field.as<dynamic_ptr>();
      if (!name_label || name_label->as<std::string>("text"_key) != display_name)
        return;
      auto& value_field = (*rc)->at(size_t{1});
      if (!value_field.is<dynamic_ptr>())
        return;
      auto value_widget = value_field.as<dynamic_ptr>();
      if (value_widget)
        result = value_widget->as<std::string>("text"_key);
    });
    return result;
  }

  // emit() defers delivery to the render loop's next frame (see
  // session.hpp's contract on emit_event), so callers must spin briefly for
  // it -- same idiom as WindowClosedEmitsClosedAndCleansUp below.
  static void wait_for(const bool& flag) {
    auto t0 = std::chrono::steady_clock::now();
    while (!flag && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

TEST_F(TopSnapshotTest, UpdatesSummaryLabels) {
  update_snapshot(37.5, {}, 1000.0, 400.0, {});

  EXPECT_EQ(label_text(root_ + ".vbox.summary.cpu_label"), "CPU: 37.5%");
  EXPECT_NE(label_text(root_ + ".vbox.summary.mem_label").find("40.0%"), std::string::npos);
}

// Regression test: the ImPlot renderer plots min(xs.size(), ys.size())
// points, so a series with populated "ys" but empty "xs" silently renders
// nothing. Both plot series must get a matching "xs" alongside "ys".
TEST_F(TopSnapshotTest, PlotSeriesGetMatchingXsAndYs) {
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

TEST_F(TopSnapshotTest, FirstCallSizesCoreMeters) {
  update_snapshot(10.0, {20.0f, 30.0f, 40.0f, 50.0f}, 1000.0, 100.0, {});

  auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.cores");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());
  auto* cf = it->second->findField<dynamic_ptr>("children"_key);
  ASSERT_NE(cf, nullptr);
  ASSERT_TRUE(*cf);
  EXPECT_EQ((*cf)->size(), 4u);
}

TEST_F(TopSnapshotTest, NewProcessAddsRow) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 1024.0 * 1024.0}});

  EXPECT_EQ(row_count(), 1u);
}

TEST_F(TopSnapshotTest, ExistingProcessUpdatesInPlaceWithoutDuplicating) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 1024.0 * 1024.0}});
  ASSERT_EQ(row_count(), 1u);

  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 42.0, 2.0 * 1024.0 * 1024.0}});
  EXPECT_EQ(row_count(), 1u);
}

TEST_F(TopSnapshotTest, VanishedProcessRemovesRow) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  ASSERT_EQ(row_count(), 1u);

  update_snapshot(5.0, {}, 1000.0, 100.0, {}); // pid 100 no longer present
  EXPECT_EQ(row_count(), 0u);
}

TEST_F(TopSnapshotTest, SecondProcessAddsSecondRow) {
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{100, "init", "[init]", "S", 5.0, 0.0}, {200, "sshd", "[sshd]", "S", 1.0, 0.0}});
  EXPECT_EQ(row_count(), 2u);
}

// ── Column-header sorting ─────────────────────────────────────────────────────

TEST_F(TopSnapshotTest, DefaultsToCpuPercentDescending) {
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{1, "low", "[low]", "S", 10.0, 0.0}, {2, "high", "[high]", "S", 50.0, 0.0}, {3, "mid", "[mid]", "S", 30.0, 0.0}});

  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{2, 3, 1}));
}

TEST_F(TopSnapshotTest, SortedEventByPidReorders) {
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

TEST_F(TopSnapshotTest, SortedEventByNameDescendingReorders) {
  update_snapshot(
      5.0,
      {},
      1000.0,
      100.0,
      {{1, "alpha", "[alpha]", "S", 10.0, 0.0}, {2, "beta", "[beta]", "S", 10.0, 0.0}, {3, "gamma", "[gamma]", "S", 10.0, 0.0}});

  simulate_sort(1, /*ascending=*/false); // Name descending
  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{3, 2, 1})); // gamma, beta, alpha
}

TEST_F(TopSnapshotTest, SortCriterionPersistsAcrossSubsequentSnapshots) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{1, "a", "[a]", "S", 90.0, 0.0}, {2, "b", "[b]", "S", 10.0, 0.0}});
  simulate_sort(0, /*ascending=*/true); // PID ascending, overriding the CPU%-descending default
  ASSERT_EQ(row_pids_in_order(), (std::vector<int>{1, 2}));

  // A later snapshot (even one that would reorder under the old default)
  // must keep applying the user's chosen criterion, not reset to it.
  update_snapshot(5.0, {}, 1000.0, 100.0, {{1, "a", "[a]", "S", 5.0, 0.0}, {2, "b", "[b]", "S", 95.0, 0.0}});
  EXPECT_EQ(row_pids_in_order(), (std::vector<int>{1, 2}));
}

// ── Event routing ─────────────────────────────────────────────────────────────

TEST_F(TopSnapshotTest, WindowClosedEmitsClosedAndCleansUp) {
  bool got_closed = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got_closed = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  auto win_id = srv_->last_session->ui_objects.at(root_)->as<bison::key_t>("__wish_id"_key);
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
  EXPECT_EQ(srv_->last_session->ui_objects.count(root_), 0u);
}

// ── Row context menu: shape ───────────────────────────────────────────────────

TEST_F(TopSnapshotTest, NewRowGetsContextMenuWithExpectedItems) {
  update_snapshot(5.0, {10.0f, 20.0f}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0, 0, {0, 1}}});

  auto menu = context_menu_of(100);
  ASSERT_TRUE(menu);
  EXPECT_EQ(menu->as<bison::key_t>(dynamic::CLASS), "ContextMenu"_key);

  EXPECT_EQ(properties_item_of(100)->as<std::string>("label"_key), "Properties...");
  EXPECT_EQ(pause_resume_item_of(100)->as<std::string>("label"_key), "Pause");
  EXPECT_EQ(kill_item_of(100)->as<std::string>("label"_key), "Kill Process");
  EXPECT_EQ(affinity_item_of(100)->as<std::string>("label"_key), "Set CPU Affinity...");

  auto priority_menu = priority_menu_of(100);
  ASSERT_TRUE(priority_menu);
  EXPECT_EQ(priority_menu->as<std::string>("label"_key), "Priority");
  // nice_value defaults to 0 ("Normal", index 2) -- only that entry checked.
  for (size_t i = 0; i < 6; ++i) {
    auto item = priority_item_of(100, i);
    ASSERT_TRUE(item) << "missing priority item " << i;
    EXPECT_EQ(item->as<bool>("checked"_key), i == 2) << "priority item " << i;
  }
}

TEST_F(TopSnapshotTest, PauseResumeItemLabelTracksProcessState) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  EXPECT_EQ(pause_resume_item_of(100)->as<std::string>("label"_key), "Pause");

  // 'T' (traced/stopped) is the state a SIGSTOP'd process reports.
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "T", 5.0, 0.0}});
  EXPECT_EQ(pause_resume_item_of(100)->as<std::string>("label"_key), "Resume");
}

TEST_F(TopSnapshotTest, PriorityCheckmarkTracksNiceValueAcrossSnapshots) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0, 0}});
  EXPECT_TRUE(priority_item_of(100, 2)->as<bool>("checked"_key)); // Normal

  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0, -10}});
  EXPECT_FALSE(priority_item_of(100, 2)->as<bool>("checked"_key));
  EXPECT_TRUE(priority_item_of(100, 4)->as<bool>("checked"_key)); // High
}

// ── Kill: confirmation dialog ─────────────────────────────────────────────────

TEST_F(TopSnapshotTest, KillClickShowsConfirmDialogInsteadOfEmitting) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire(element_id(kill_item_of(100)), "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got) << "kill should be held back pending confirmation";

  // Confirm-kill is a privately-instantiated MessageBox (see
  // form::instantiate_child_form()) -- "__message_box_..." is that form
  // class's own next_available_key() prefix, matching message_box.cpp's
  // kLayoutYesNo ("body.message", "buttons.btn0"/"btn1" for Yes/No).
  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty()) << "no confirm dialog root registered";
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{confirm_root}));
  EXPECT_NE(label_text_at(confirm_root, "body.message").find("100"), std::string::npos);
  EXPECT_NE(label_text_at(confirm_root, "body.message").find("init"), std::string::npos);
}

TEST_F(TopSnapshotTest, ConfirmKillYesEmitsOnProcessActionRequestedWithKill) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  fire(element_id(kill_item_of(100)), "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  // The Yes button belongs to the confirm MessageBox's own internal tree,
  // handled by ITS OWN on_event() (top_level_handlers[confirm_root]) --
  // not top's -- so this must go through fire_at(), not fire().
  fire_at(confirm_root, yes_id, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<int32_t>("pid"_key), 100);
  EXPECT_EQ(captured.as<std::string>("action"_key), "kill");
}

TEST_F(TopSnapshotTest, ConfirmKillNoCancelsWithoutEmitting) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  fire(element_id(kill_item_of(100)), "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto no_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn1")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire_at(confirm_root, no_id, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
}

// ── Pause/resume and priority: direct emission (no confirmation) ─────────────

TEST_F(TopSnapshotTest, PauseResumeClickEmitsPauseWhenRunning) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});

  bool got = false;
  dynamic captured;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
  };

  fire(element_id(pause_resume_item_of(100)), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<int32_t>("pid"_key), 100);
  EXPECT_EQ(captured.as<std::string>("action"_key), "pause");
}

TEST_F(TopSnapshotTest, PauseResumeClickEmitsResumeWhenStopped) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "T", 5.0, 0.0}});

  bool got = false;
  dynamic captured;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
  };

  fire(element_id(pause_resume_item_of(100)), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("action"_key), "resume");
}

TEST_F(TopSnapshotTest, PriorityItemClickEmitsSetPriorityWithCorrectNice) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});

  bool got = false;
  dynamic captured;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
  };

  // Index 4 == "High" == nice -10 (see kPriorityLevels in top.cpp).
  fire(element_id(priority_item_of(100, 4)), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<int32_t>("pid"_key), 100);
  EXPECT_EQ(captured.as<std::string>("action"_key), "set_priority");
  EXPECT_EQ(captured.as<int32_t>("nice"_key), -10);
}

// ── Set CPU Affinity dialog ───────────────────────────────────────────────────

TEST_F(TopSnapshotTest, AffinityDialogOpensWithCurrentCoresPrechecked) {
  update_snapshot(
      5.0, {10.0f, 20.0f, 30.0f, 40.0f}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0, 0, {0, 2}}});

  fire(element_id(affinity_item_of(100)), "clicked"_key);

  std::string affinity_root = find_root_with_prefix(srv_->last_session->ui_objects, "__top_affinity_");
  ASSERT_FALSE(affinity_root.empty());
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{affinity_root}));

  auto core_checked = [&](int i) {
    return srv_->last_session->ui_objects.at(affinity_root + ".vbox.cores.core" + std::to_string(i))
        ->as<bool>("value"_key);
  };
  EXPECT_TRUE(core_checked(0));
  EXPECT_FALSE(core_checked(1));
  EXPECT_TRUE(core_checked(2));
  EXPECT_FALSE(core_checked(3));
}

TEST_F(TopSnapshotTest, AffinityApplyEmitsSetAffinityWithCheckedCores) {
  update_snapshot(5.0, {10.0f, 20.0f, 30.0f}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0, 0, {0, 1, 2}}});
  fire(element_id(affinity_item_of(100)), "clicked"_key);

  std::string affinity_root = find_root_with_prefix(srv_->last_session->ui_objects, "__top_affinity_");
  ASSERT_FALSE(affinity_root.empty());

  // Uncheck core 1 directly (as if render_checkbox() had already toggled
  // it), then Apply -- the handler reads each checkbox's current `value`
  // field rather than tracking a separate "changed" history.
  srv_->last_session->ui_objects.at(affinity_root + ".vbox.cores.core1")["value"_key] = false;

  bool got = false;
  dynamic captured;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic payload) {
    if (event == "on_process_action_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
  };

  auto apply_id =
      srv_->last_session->ui_objects.at(affinity_root + ".vbox.buttons.btn_apply")->as<bison::key_t>("__wish_id"_key);
  fire(apply_id, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<int32_t>("pid"_key), 100);
  EXPECT_EQ(captured.as<std::string>("action"_key), "set_affinity");
  auto* cores = captured.findField<std::vector<int32_t>>("cores"_key);
  ASSERT_NE(cores, nullptr);
  EXPECT_EQ(*cores, (std::vector<int32_t>{0, 2}));
}

TEST_F(TopSnapshotTest, AffinityApplyWithNoCoresCheckedDoesNotEmitAndSetsStatus) {
  update_snapshot(5.0, {10.0f}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0, 0, {0}}});
  fire(element_id(affinity_item_of(100)), "clicked"_key);

  std::string affinity_root = find_root_with_prefix(srv_->last_session->ui_objects, "__top_affinity_");
  ASSERT_FALSE(affinity_root.empty());
  srv_->last_session->ui_objects.at(affinity_root + ".vbox.cores.core0")["value"_key] = false;

  bool got = false;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic) {
    if (event == "on_process_action_requested"_key)
      got = true;
  };

  auto apply_id =
      srv_->last_session->ui_objects.at(affinity_root + ".vbox.buttons.btn_apply")->as<bison::key_t>("__wish_id"_key);
  fire(apply_id, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_NE(label_text(root_ + ".vbox.status_label").find("at least one core"), std::string::npos);
}

// ── Properties dialog ─────────────────────────────────────────────────────────

TEST_F(TopSnapshotTest, PropertiesClickShowsLoadingAndRequestsDetails) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});

  bool got = false;
  dynamic captured;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic payload) {
    if (event == "on_process_details_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
  };

  fire(element_id(properties_item_of(100)), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<int32_t>("pid"_key), 100);

  // The Properties dialog is a privately-instantiated PropertiesDialog (see
  // form::instantiate_child_form()) -- "__properties_dialog_..." is that
  // form class's own next_available_key() prefix.
  std::string properties_root = find_root_with_prefix(srv_->last_session->ui_objects, "__properties_dialog_");
  ASSERT_FALSE(properties_root.empty());
  EXPECT_EQ(process_details_value(properties_root, "PID"), "100");
}

TEST_F(TopSnapshotTest, ReportProcessDetailsPopulatesDialog) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  fire(element_id(properties_item_of(100)), "clicked"_key);

  dynamic report;
  report["pid"_key] = int32_t{100};
  report["found"_key] = true;
  report["ppid"_key] = int32_t{1};
  report["user"_key] = std::string{"root"};
  report["thread_count"_key] = int32_t{4};
  report["start_time"_key] = std::string{"2026-08-08 12:00:00"};
  report["exe_path"_key] = std::string{"/sbin/init"};
  report["cwd"_key] = std::string{"/"};
  report["cmdline"_key] = std::string{"/sbin/init"};
  report["nice"_key] = int32_t{0};
  report["affinity_cores"_key] = std::vector<int32_t>{0, 1};
  proxy_->call("report_process_details"_key, std::move(report)).get();

  std::string properties_root = find_root_with_prefix(srv_->last_session->ui_objects, "__properties_dialog_");
  ASSERT_FALSE(properties_root.empty());
  EXPECT_EQ(process_details_value(properties_root, "Parent PID"), "1");
  EXPECT_EQ(process_details_value(properties_root, "User"), "root");
  EXPECT_EQ(process_details_value(properties_root, "Threads"), "4");
  EXPECT_EQ(process_details_value(properties_root, "Executable"), "/sbin/init");
}

TEST_F(TopSnapshotTest, ReportProcessDetailsIgnoredForStalePid) {
  update_snapshot(
      5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}, {200, "sshd", "[sshd]", "S", 1.0, 0.0}});
  fire(element_id(properties_item_of(100)), "clicked"_key);

  std::string properties_root = find_root_with_prefix(srv_->last_session->ui_objects, "__properties_dialog_");
  ASSERT_FALSE(properties_root.empty());

  // A response for a *different* pid (e.g. a slow response that arrived
  // after the user closed pid 100's dialog and opened pid 200's) must not
  // overwrite what's currently showing.
  dynamic report;
  report["pid"_key] = int32_t{200};
  report["found"_key] = true;
  report["ppid"_key] = int32_t{1};
  report["user"_key] = std::string{"nobody"};
  report["thread_count"_key] = int32_t{1};
  report["start_time"_key] = std::string{};
  report["exe_path"_key] = std::string{"/usr/sbin/sshd"};
  report["cwd"_key] = std::string{};
  report["cmdline"_key] = std::string{};
  report["nice"_key] = int32_t{0};
  report["affinity_cores"_key] = std::vector<int32_t>{};
  proxy_->call("report_process_details"_key, std::move(report)).get();

  // Still showing the "Loading..." placeholder show_properties_dialog()
  // set for pid 100 -- the pid-200 response must be ignored outright, not
  // just fail to overwrite a value that happened to already be filled in.
  EXPECT_EQ(process_details_value(properties_root, "User"), "Loading...");
}

TEST_F(TopSnapshotTest, ReportProcessDetailsNotFoundShowsError) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  fire(element_id(properties_item_of(100)), "clicked"_key);

  dynamic report;
  report["pid"_key] = int32_t{100};
  report["found"_key] = false;
  report["error"_key] = std::string{"No such process"};
  proxy_->call("report_process_details"_key, std::move(report)).get();

  std::string properties_root = find_root_with_prefix(srv_->last_session->ui_objects, "__properties_dialog_");
  ASSERT_FALSE(properties_root.empty());
  EXPECT_EQ(process_details_value(properties_root, "User"), "No such process");
}

// ── Action result: status label ───────────────────────────────────────────────

TEST_F(TopSnapshotTest, ReportActionResultSuccessUpdatesStatusLabel) {
  dynamic report;
  report["pid"_key] = int32_t{100};
  report["action"_key] = std::string{"kill"};
  report["success"_key] = true;
  report["error"_key] = std::string{};
  proxy_->call("report_action_result"_key, std::move(report)).get();

  auto text = label_text(root_ + ".vbox.status_label");
  EXPECT_NE(text.find("100"), std::string::npos);
  EXPECT_NE(text.find("succeeded"), std::string::npos);
}

TEST_F(TopSnapshotTest, ReportActionResultFailureUpdatesStatusLabelWithError) {
  dynamic report;
  report["pid"_key] = int32_t{100};
  report["action"_key] = std::string{"set_priority"};
  report["success"_key] = false;
  report["error"_key] = std::string{"Permission denied"};
  proxy_->call("report_action_result"_key, std::move(report)).get();

  auto text = label_text(root_ + ".vbox.status_label");
  EXPECT_NE(text.find("failed"), std::string::npos);
  EXPECT_NE(text.find("Permission denied"), std::string::npos);
}

// ── Vanished process cleans up its context-menu action mappings ──────────────

TEST_F(TopSnapshotTest, VanishedProcessKillItemNoLongerTriggersConfirm) {
  update_snapshot(5.0, {}, 1000.0, 100.0, {{100, "init", "[init]", "S", 5.0, 0.0}});
  auto kill_id = element_id(kill_item_of(100));
  ASSERT_NE(kill_id.id, 0u);

  update_snapshot(5.0, {}, 1000.0, 100.0, {}); // pid 100 no longer present

  bool got = false;
  srv_->last_session->emit_event = [&](bison::key_t, bison::key_t event, dynamic) {
    if (event == "on_process_action_requested"_key)
      got = true;
  };
  // The old kill item's id is no longer registered in action_item_targets_,
  // so replaying its click must be a no-op instead of resurrecting a
  // confirm dialog for a process that's already gone.
  fire(kill_id, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}
