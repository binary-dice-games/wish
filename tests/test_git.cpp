// MIT License © 2025 Binary Dice Games
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

dynamic make_log_args(const std::vector<fake_commit>& commits, bool working_dirty, std::string head_hash = {}) {
  dynamic args;
  args["working_dirty"_key] = working_dirty;
  args["head_hash"_key] = head_hash;

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

struct fake_diff_line {
  std::string kind;
  std::string text;
};

// Builds the args shape update_diff expects (see git.hpp's do_update_diff
// doc comment): hash/staged are echoed back exactly as
// git_repo_source::on_diff_requested() sends them, so a test can simulate
// a response arriving for a selection that no longer matches current state.
dynamic make_diff_args(
    const std::string& path, const std::string& hash, bool staged, const std::vector<fake_diff_line>& lines) {
  dynamic args;
  args["path"_key] = path;
  args["hash"_key] = hash;
  args["staged"_key] = staged;
  dynamic arr;
  size_t i = 0;
  for (auto& l : lines) {
    auto ep = std::make_shared<dynamic>();
    (*ep)["kind"_key] = l.kind;
    (*ep)["text"_key] = l.text;
    arr[i++] = dynamic_ptr{ep};
  }
  args["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

// Builds the args shape update_commit_files expects (see git.hpp's
// do_update_commit_files doc comment): `{ hash, files: [{path, status}] }`.
dynamic make_commit_files_args(const std::string& hash, const std::vector<fake_status_entry>& files) {
  dynamic args;
  args["hash"_key] = hash;
  dynamic arr;
  size_t i = 0;
  for (auto& f : files) {
    auto ep = std::make_shared<dynamic>();
    (*ep)["path"_key] = f.path;
    (*ep)["status"_key] = f.status;
    arr[i++] = dynamic_ptr{ep};
  }
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
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
  // construction, same convention test_top.cpp's row_count()
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

  // ── Confirm-dialog test helpers ──────────────────────────────────────────
  //
  // Sidebar rows (and their MenuButton/MenuItem children) are registered
  // only via rebuild_section()'s direct children-map assignment -- not
  // merged into ui_objects' dot-path map -- so they must be found by
  // walking the section's "children" field, same technique test_top.cpp's
  // find_row()/nth_child() use for row cells. The confirm dialog itself,
  // by contrast, is a privately-instantiated MessageBox (form::
  // instantiate_child_form()), found by dot-path prefix instead
  // (find_root_with_prefix()).

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

  // Branch sidebar row at `index` (HorizontalLayout: Selectable + MenuButton).
  dynamic_ptr branch_row(size_t index) const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.body.sidebar.branches");
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    return nth_child(it->second, index);
  }

  // Row's "Delete" MenuItem -- third entry in make_sidebar_row()'s
  // menu_items list ({"Checkout", "Merge into current", "Delete"}), under
  // the row's MenuButton (its second child, after the Selectable).
  dynamic_ptr delete_item_of_branch(size_t index) const {
    return nth_child(nth_child(branch_row(index), 1), 2);
  }

  // Selects a commit graph row the same way a real click does: fires
  // row_selected on the graph_table's own wish_id with the row's index --
  // see imgui_ui_renderer.cpp's table-row loop (pending_index == the
  // TableRow's ordinal position, same as its numeric children-map key
  // here since rebuild_graph_table() always assigns a fresh, contiguous
  // 0-based sequence -- see git.hpp's "dynamic::size() gotcha").
  void select_graph_row(int32_t index) {
    auto table_id =
        srv_->last_session->ui_objects.at(root_ + ".vbox.body.graph_panel.graph_table")->as<bison::key_t>("__wish_id"_key);
    dynamic payload;
    payload["index"_key] = index;
    fire(table_id, "row_selected"_key, std::move(payload));
  }

  // Files table row's own Selectable (second cell -- see add_file_row()),
  // at `index` in the files table's children map.
  bison::key_t files_row_selectable_id(size_t index) const {
    auto it = srv_->last_session->ui_objects.find(root_ + "_files.vbox.files_table");
    if (it == srv_->last_session->ui_objects.end())
      return {};
    return element_id(nth_child(nth_child(it->second, index), 1));
  }

  // Clicks a Files-table row's path Selectable the same way a real click
  // does (a "changed" event with selected=true -- see git.cpp's
  // selectable_handlers_ dispatch under on_event()).
  void click_file_row(size_t index) {
    dynamic payload;
    payload["selected"_key] = true;
    fire(files_row_selectable_id(index), "changed"_key, std::move(payload));
  }

  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }

  // The confirm dialog is a privately-instantiated MessageBox form
  // instance (see form::instantiate_child_form()), registered as its OWN
  // top_level_handlers entry (keyed by ITS OWN root, not root_) -- fire()
  // alone can't reach its buttons; use this instead, with the dialog's
  // own root (e.g. from find_root_with_prefix()).
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

  // emit() defers delivery to the render loop's next frame (see
  // session.hpp's contract on emit_event), so callers must spin briefly for
  // it -- same idiom test_top.cpp's wait_for() uses.
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

TEST_F(GitRepoRmiTest, UpdateLogMarksHeadRowByHashNotPosition) {
  // "c2" is the newest commit by log order (row 0), but head_hash points at
  // the older "c1" -- e.g. a checked-out branch whose tip is behind another
  // branch's tip in --branches --tags --date-order. is_head must follow the
  // hash, not default to row 0.
  call("update_log"_key,
       make_log_args(
           {
               {"c2", {"c1"}, "Ada", "2026-01-02 10:00", "second"},
               {"c1", {}, "Ada", "2026-01-01 10:00", "first"},
           },
           /*working_dirty=*/false,
           /*head_hash=*/"c1"));

  auto table = srv_->last_session->ui_objects.at(root_ + ".vbox.body.graph_panel.graph_table");
  auto row0_cell = nth_child(nth_child(table, 0), 0);
  auto row1_cell = nth_child(nth_child(table, 1), 0);
  ASSERT_TRUE(row0_cell);
  ASSERT_TRUE(row1_cell);
  EXPECT_FALSE(row0_cell->as<bool>("is_head"_key));
  EXPECT_TRUE(row1_cell->as<bool>("is_head"_key));
}

TEST_F(GitRepoRmiTest, UpdateLogMarksNoRowHeadWhenHashUnknown) {
  call("update_log"_key,
       make_log_args(
           {
               {"c1", {}, "Ada", "2026-01-01 10:00", "first"},
           },
           /*working_dirty=*/false,
           /*head_hash=*/""));

  auto table = srv_->last_session->ui_objects.at(root_ + ".vbox.body.graph_panel.graph_table");
  auto row0_cell = nth_child(nth_child(table, 0), 0);
  ASSERT_TRUE(row0_cell);
  EXPECT_FALSE(row0_cell->as<bool>("is_head"_key));
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

// ── Delete branch: confirmation dialog ────────────────────────────────────────

TEST_F(GitRepoRmiTest, DeleteBranchClickShowsConfirmDialogInsteadOfEmitting) {
  call("update_refs"_key, make_refs_args("main", {{"feature", false, "", 0, 0}}));

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "delete_branch_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire(element_id(delete_item_of_branch(0)), "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got) << "delete should be held back pending confirmation";

  // The confirm dialog is a privately-instantiated MessageBox (see
  // form::instantiate_child_form()) -- "__message_box_..." is that form
  // class's own next_available_key() prefix, matching message_box.cpp's
  // kLayoutYesNo ("body.message", "buttons.btn0"/"btn1" for Yes/No).
  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty()) << "no confirm dialog root registered";
  EXPECT_TRUE(srv_->last_session->top_level_objects.count(bison::key_t{confirm_root}));
  auto it = srv_->last_session->ui_objects.find(confirm_root + ".body.message");
  ASSERT_NE(it, srv_->last_session->ui_objects.end());
  EXPECT_NE(it->second->as<std::string>("text"_key).find("feature"), std::string::npos);
}

TEST_F(GitRepoRmiTest, ConfirmDeleteBranchYesEmitsDeleteBranchRequested) {
  call("update_refs"_key, make_refs_args("main", {{"feature", false, "", 0, 0}}));
  fire(element_id(delete_item_of_branch(0)), "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "delete_branch_requested"_key) {
      got = true;
      captured = std::move(payload);
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  // The Yes button belongs to the confirm MessageBox's own internal tree,
  // handled by ITS OWN on_event() (top_level_handlers[confirm_root]) --
  // not git_repo's -- so this must go through fire_at(), not fire().
  fire_at(confirm_root, yes_id, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("name"_key), "feature");
  EXPECT_FALSE(captured.as<bool>("force"_key));
}

TEST_F(GitRepoRmiTest, ConfirmDeleteBranchNoCancelsWithoutEmitting) {
  call("update_refs"_key, make_refs_args("main", {{"feature", false, "", 0, 0}}));
  fire(element_id(delete_item_of_branch(0)), "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto no_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn1")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "delete_branch_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire_at(confirm_root, no_id, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
}

// ── update_diff staleness guard ────────────────────────────────────────────
//
// Reproduces the bug reported live: selecting an older commit's changed
// file left the Diff window's title correctly updated but its table
// permanently empty. Investigation (docs/automation.md's workflow, driven
// against this exact repo via the automation module) found the request/
// response round trip itself works correctly for a single, unhurried
// click; the real defect is that do_update_diff() -- unlike its sibling
// do_update_commit_files(), which already guards on `hash != selected_
// hash_` -- had no way to tell a stale response (for a selection the user
// has since navigated away from) apart from the current one, since the
// client didn't even echo `hash`/`staged` back for it to compare against.
// A late-arriving stale response could then silently overwrite whatever
// the user is now looking at -- exactly the "title says one thing, body
// shows something else (here, nothing)" symptom reported.

TEST_F(GitRepoRmiTest, DiffPopulatesWhenResponseMatchesCurrentSelection) {
  call("update_log"_key, make_log_args({{"c1", {}, "Ada", "2026-01-01 10:00", "first"}}, /*working_dirty=*/false));
  select_graph_row(0); // the only row -- commit c1.
  call("update_commit_files"_key, make_commit_files_args("c1", {{"foo.txt", "M"}}));
  click_file_row(0); // selects foo.txt under c1 (would emit diff_requested{hash:"c1", path:"foo.txt", staged:false}).

  call("update_diff"_key, make_diff_args("foo.txt", "c1", false, {{"add", "+ line"}}));

  EXPECT_EQ(child_array_count(root_ + "_diff.vbox.diff_table"), 1u);
  auto title_it = srv_->last_session->ui_objects.find(root_ + "_diff.vbox.title_label");
  ASSERT_NE(title_it, srv_->last_session->ui_objects.end());
  EXPECT_EQ(title_it->second->as<std::string>("text"_key), "foo.txt");
}

TEST_F(GitRepoRmiTest, StaleDiffResponseIgnoredAfterSelectionChanges) {
  call("update_log"_key, make_log_args({{"c1", {}, "Ada", "2026-01-01 10:00", "first"}}, /*working_dirty=*/true));
  // Row 0 is the synthetic "Uncommitted changes" row; row 1 is c1.
  select_graph_row(1);
  call("update_commit_files"_key, make_commit_files_args("c1", {{"foo.txt", "M"}}));
  click_file_row(0); // selects foo.txt under c1.

  // The user navigates away (e.g. back to the working tree) before the
  // client's diff_requested response for c1/foo.txt comes back.
  select_graph_row(0);

  // That now-stale response finally arrives.
  call("update_diff"_key, make_diff_args("foo.txt", "c1", false, {{"add", "+ stale, must be dropped"}}));

  // Must be silently discarded -- no rows added, title untouched (still
  // its construction-time default, never having been set for a selection
  // that was current when the response arrived).
  EXPECT_EQ(child_array_count(root_ + "_diff.vbox.diff_table"), 0u);
  auto title_it = srv_->last_session->ui_objects.find(root_ + "_diff.vbox.title_label");
  ASSERT_NE(title_it, srv_->last_session->ui_objects.end());
  EXPECT_EQ(title_it->second->as<std::string>("text"_key), "");
}
