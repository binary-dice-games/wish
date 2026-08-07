// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// Builds the "commits" array shape update_log expects: {hash, parents[],
// author, date, subject} per entry -- see git.hpp's do_update_log doc
// comment for the full contract.
struct fake_commit {
  std::string hash;
  std::vector<std::string> parents;
  std::string author;
  std::string date;
  std::string subject;
};

dynamic make_log_args(const std::vector<fake_commit>& commits, bool working_dirty) {
  dynamic args;
  args["working_dirty"_key] = working_dirty;

  dynamic arr;
  size_t i = 0;
  for (auto& c : commits) {
    auto e = std::make_shared<dynamic>();
    (*e)["hash"_key] = c.hash;
    dynamic parents;
    size_t pi = 0;
    for (auto& p : c.parents)
      parents[pi++] = p;
    (*e)["parents"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(parents))};
    (*e)["author"_key] = c.author;
    (*e)["date"_key] = c.date;
    (*e)["subject"_key] = c.subject;
    arr[i++] = dynamic_ptr{e};
  }
  args["commits"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

struct fake_branch {
  std::string name;
  bool is_remote{false};
  std::string upstream;
  int32_t ahead{0};
  int32_t behind{0};
};

dynamic make_refs_args(const std::string& current_branch, const std::vector<fake_branch>& branches) {
  dynamic args;
  args["current_branch"_key] = current_branch;
  dynamic arr;
  size_t i = 0;
  for (auto& b : branches) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = b.name;
    (*e)["is_remote"_key] = b.is_remote;
    (*e)["upstream"_key] = b.upstream;
    (*e)["ahead"_key] = b.ahead;
    (*e)["behind"_key] = b.behind;
    arr[i++] = dynamic_ptr{e};
  }
  args["branches"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  args["tags"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  args["stashes"_key] = dynamic_ptr{std::make_shared<dynamic>()};
  return args;
}

struct fake_status_entry {
  std::string path;
  std::string status;
};

dynamic make_status_args(const std::vector<fake_status_entry>& staged, const std::vector<fake_status_entry>& unstaged) {
  dynamic args;
  auto build = [](const std::vector<fake_status_entry>& entries) {
    dynamic arr;
    size_t i = 0;
    for (auto& e : entries) {
      auto ep = std::make_shared<dynamic>();
      (*ep)["path"_key] = e.path;
      (*ep)["status"_key] = e.status;
      arr[i++] = dynamic_ptr{ep};
    }
    return dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  };
  args["staged"_key] = build(staged);
  args["unstaged"_key] = build(unstaged);
  return args;
}

} // namespace

// ── Local (non-RMI) fixture — checks prototype registration ──────────────────

class GitRepoLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(GitRepoLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "GitRepo"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "GitRepo"_key);
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

// The four internal windows this form registers (see git.hpp's class doc
// comment) are keyed "__git_N", "__git_N_files", "__git_N_diff",
// "__git_N_log" -- find the bare (no-dot) main-window root among
// session.objects.
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__git_", 0) == 0 && k.find('.') == std::string::npos && k.find("_files") == std::string::npos &&
        k.find("_diff") == std::string::npos && k.find("_log") == std::string::npos)
      return k;
  }
  return {};
}

class GitRepoRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "GitRepo"_key).get());
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

  // Numeric-indexed ("array") children count of the element at `path` --
  // excludes the string-keyed TableColumn/section children present from
  // construction, same convention test_process_explorer.cpp's row_count()
  // uses for Table rows (dynamically-added TreeNode/Table rows here use the
  // same numeric-key convention -- see git.cpp's rebuild_section()/
  // rebuild_graph_table()/add_file_row()).
  size_t child_array_count(const std::string& path) const {
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

TEST_F(GitRepoRmiTest, InstantiationCreatesFourWindows) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_files"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_diff"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_log"));
}

TEST_F(GitRepoRmiTest, MainWindowContainsGraphTableAndSidebarSections) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.body.graph_panel.graph_table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.body.sidebar.branches"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.body.sidebar.remotes"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.body.sidebar.tags"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.body.sidebar.stashes"));
}

TEST_F(GitRepoRmiTest, UpdateRefsPopulatesBranchesSection) {
  call("update_refs"_key,
       make_refs_args(
           "main",
           {
               {"main", false, "origin/main", 0, 0},
               {"feature", false, "", 0, 0},
               {"origin/main", true, "", 0, 0},
           }));

  EXPECT_EQ(child_array_count(root_ + ".vbox.body.sidebar.branches"), 2u);
  EXPECT_EQ(child_array_count(root_ + ".vbox.body.sidebar.remotes"), 1u);

  auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.body.graph_panel.current_branch_label");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());
  EXPECT_NE(it->second->as<std::string>("text"_key).find("main"), std::string::npos);
}

TEST_F(GitRepoRmiTest, UpdateRefsRebuildIsIdempotentInRowCount) {
  fake_branch b{"main", false, "", 0, 0};
  call("update_refs"_key, make_refs_args("main", {b}));
  EXPECT_EQ(child_array_count(root_ + ".vbox.body.sidebar.branches"), 1u);
  // Calling again with the same data must not accumulate duplicate rows.
  call("update_refs"_key, make_refs_args("main", {b}));
  EXPECT_EQ(child_array_count(root_ + ".vbox.body.sidebar.branches"), 1u);
}

TEST_F(GitRepoRmiTest, UpdateLogPopulatesGraphTableRows) {
  call("update_log"_key,
       make_log_args(
           {
               {"c2", {"c1"}, "Ada", "2026-01-02 10:00", "second"},
               {"c1", {}, "Ada", "2026-01-01 10:00", "first"},
           },
           /*working_dirty=*/false));

  EXPECT_EQ(child_array_count(root_ + ".vbox.body.graph_panel.graph_table"), 2u);
}

TEST_F(GitRepoRmiTest, UpdateLogAddsSyntheticWorkingRowWhenDirty) {
  call("update_log"_key,
       make_log_args(
           {
               {"c1", {}, "Ada", "2026-01-01 10:00", "first"},
           },
           /*working_dirty=*/true));

  // The synthetic "Uncommitted changes" row plus the one real commit.
  EXPECT_EQ(child_array_count(root_ + ".vbox.body.graph_panel.graph_table"), 2u);
}

TEST_F(GitRepoRmiTest, UpdateLogRebuildReplacesRowsRatherThanAccumulating) {
  call("update_log"_key, make_log_args({{"a", {}, "Ada", "d", "s"}}, false));
  EXPECT_EQ(child_array_count(root_ + ".vbox.body.graph_panel.graph_table"), 1u);
  call("update_log"_key, make_log_args({{"a", {}, "Ada", "d", "s"}, {"b", {"a"}, "Ada", "d", "s2"}}, false));
  EXPECT_EQ(child_array_count(root_ + ".vbox.body.graph_panel.graph_table"), 2u);
}

TEST_F(GitRepoRmiTest, UpdateStatusPopulatesFilesTable) {
  call("update_status"_key,
       make_status_args(
           {{"a.txt", "M"}},
           {{"b.txt", "M"}, {"c.txt", "?"}}));

  // Working-tree mode is the default on a fresh form -- staged + unstaged
  // rows both land in the Files window's table.
  EXPECT_EQ(child_array_count(root_ + "_files.vbox.files_table"), 3u);
}

TEST_F(GitRepoRmiTest, AppendCommandLogAddsRowsToLogTable) {
  auto make_log_call_args = [](const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
    dynamic args;
    args["command"_key] = command;
    args["exit_code"_key] = exit_code;
    args["ok"_key] = ok;
    args["output"_key] = output;
    return args;
  };

  EXPECT_EQ(child_array_count(root_ + "_log.vbox.log_table"), 0u);
  call("append_command_log"_key, make_log_call_args("git status --porcelain=v1", 0, true, ""));
  EXPECT_EQ(child_array_count(root_ + "_log.vbox.log_table"), 1u);
  call("append_command_log"_key, make_log_call_args("git push", 1, false, "rejected"));
  EXPECT_EQ(child_array_count(root_ + "_log.vbox.log_table"), 2u);
}
