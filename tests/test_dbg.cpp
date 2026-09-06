// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <functional>
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

struct fake_thread {
  uint32_t id;
  std::string state;
  std::string current_function;
};

dynamic make_threads_args(const std::vector<fake_thread>& ts) {
  dynamic args;
  dynamic arr;
  size_t i = 0;
  for (auto& t : ts) {
    auto e = std::make_shared<dynamic>();
    (*e)["id"_key] = static_cast<int32_t>(t.id);
    (*e)["state"_key] = t.state;
    (*e)["current_function"_key] = t.current_function;
    arr[i++] = dynamic_ptr{e};
  }
  args["threads"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

struct fake_frame {
  int32_t index;
  std::string function;
  std::string file;
  int32_t line;
};

dynamic make_callstack_args(uint32_t thread_id, const std::vector<fake_frame>& fs) {
  dynamic args;
  args["thread_id"_key] = static_cast<int32_t>(thread_id);
  dynamic arr;
  size_t i = 0;
  for (auto& f : fs) {
    auto e = std::make_shared<dynamic>();
    (*e)["index"_key] = f.index;
    (*e)["function"_key] = f.function;
    (*e)["file"_key] = f.file;
    (*e)["line"_key] = f.line;
    arr[i++] = dynamic_ptr{e};
  }
  args["frames"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

struct fake_breakpoint {
  std::string file;
  int32_t line;
  bool enabled;
};

dynamic make_breakpoints_args(const std::vector<fake_breakpoint>& bps) {
  dynamic args;
  dynamic arr;
  size_t i = 0;
  for (auto& bp : bps) {
    auto e = std::make_shared<dynamic>();
    (*e)["file"_key] = bp.file;
    (*e)["line"_key] = bp.line;
    (*e)["enabled"_key] = bp.enabled;
    arr[i++] = dynamic_ptr{e};
  }
  args["breakpoints"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

dynamic make_source_args(const std::string& path, const std::vector<int32_t>& breakpoint_lines, int32_t current_line) {
  dynamic args;
  args["path"_key] = path;
  args["breakpoint_lines"_key] = breakpoint_lines;
  args["current_line"_key] = current_line;
  return args;
}

} // namespace

// ── Local (non-RMI) fixture ─────────────────────────────────────────────────

class DbgLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(DbgLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "DebuggerFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "DebuggerFrontend"_key);
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

// The Source (main) window is keyed "__dbg_N"; the five secondary windows
// are "__dbg_N_<suffix>" -- find the bare main root, mirroring
// test_docker.cpp's find_form_root().
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__dbg_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__dbg_N_<suffix>": a second '_' after the numeric index.
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

// ── Tree-walking helpers for Source-window tabs (added outside the dot-path
// map) ────────────────────────────────────────────────────────────────────
// ensure_source_tab() (server/dbg.cpp) creates its TabItem/TextEditor pair
// the same way tail.cpp's ensure_tag_tab() does: assign_id() +
// ctx().put_object() only, never registered into ui_objects by dot-path.
// Mirroring test_tail.cpp's find_tab_item()/find_tab_table() helpers, these
// walk the (dot-path-registered) TabBar's own "children" field directly.
//
// NOTE: dynamic_ptr's default constructor allocates a fresh, non-null
// dynamic (see bison_common.hpp) -- every "not found" sentinel below is
// explicitly dynamic_ptr{nullptr}.

static dynamic_ptr get_dp(const wish::name_map& objects, const std::string& path) {
  auto it = objects.find(path);
  return it == objects.end() ? dynamic_ptr{nullptr} : it->second;
}

static dynamic_ptr find_tab_item(const dynamic_ptr& tab_bar, const std::string& label) {
  dynamic_ptr result{nullptr};
  if (!tab_bar)
    return result;
  auto* cf = tab_bar->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return result;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (result || !f.is<dynamic_ptr>())
      return;
    auto tab = f.as<dynamic_ptr>();
    if (tab && tab->as<bison::key_t>(dynamic::CLASS) == "TabItem"_key && tab->as<std::string>("label"_key) == label)
      result = tab;
  });
  return result;
}

static size_t count_children_of_class(const dynamic_ptr& parent, bison::key_t klass) {
  size_t count = 0;
  if (!parent)
    return count;
  auto* cf = parent->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return count;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto child = f.as<dynamic_ptr>();
    if (child && child->as<bison::key_t>(dynamic::CLASS) == klass)
      ++count;
  });
  return count;
}

static dynamic_ptr find_child_of_class(const dynamic_ptr& parent, bison::key_t klass) {
  dynamic_ptr result{nullptr};
  if (!parent)
    return result;
  auto* cf = parent->findField<dynamic_ptr>("children"_key);
  if (!cf || !*cf)
    return result;
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (result || !f.is<dynamic_ptr>())
      return;
    auto child = f.as<dynamic_ptr>();
    if (child && child->as<bison::key_t>(dynamic::CLASS) == klass)
      result = child;
  });
  return result;
}

class DbgRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "DebuggerFrontend"_key).get());
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

  // `dynamic_object::size()` reports `last_index + 1`, not a live entry
  // count -- correct for tables that are fully cleared and rebuilt from
  // index 0 each time (Threads/Call Stack/Watch/Breakpoints), but wrong for
  // a FIFO-capped table like Output, whose numeric child keys only ever
  // increase and whose oldest (lowest-index) entries are erased in place,
  // leaving gaps below the highest index. Count and index live children by
  // walking `forEach` (ascending key order == insertion order) instead.
  size_t row_count(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    size_t count = 0;
    (*cf)->forEach([&](bison::key_t, const bison::field& f) {
      // TableColumn children live in the same numeric "children" array as
      // TableRow children; only rows count toward "row_count".
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>()->as<bison::key_t>(dynamic::CLASS) != "TableColumn"_key)
        ++count;
    });
    return count;
  }

  static dynamic_ptr nth_child(const dynamic_ptr& parent, size_t index) {
    if (!parent)
      return nullptr;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    dynamic_ptr result;
    size_t seen = 0;
    (*cf)->forEach([&](bison::key_t, const bison::field& f) {
      if (!f.is<dynamic_ptr>() || f.as<dynamic_ptr>()->as<bison::key_t>(dynamic::CLASS) == "TableColumn"_key)
        return;
      if (seen++ == index)
        result = f.as<dynamic_ptr>();
    });
    return result;
  }

  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }
  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }

  static void wait_for(const bool& flag) {
    auto t0 = std::chrono::steady_clock::now();
    while (!flag && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  bison::key_t id_at(const std::string& dot_path) const {
    auto it = srv_->last_session->ui_objects.find(root_ + "." + dot_path);
    return it == srv_->last_session->ui_objects.end() ? bison::key_t{}
                                                      : it->second->as<bison::key_t>("__wish_id"_key);
  }

  bison::key_t window_id(const std::string& suffix) const {
    auto it = srv_->last_session->ui_objects.find(root_ + suffix);
    return it == srv_->last_session->ui_objects.end() ? bison::key_t{}
                                                      : it->second->as<bison::key_t>("__wish_id"_key);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

// ── Window construction ──────────────────────────────────────────────────────

TEST_F(DbgRmiTest, InstantiationBuildsAllSixWindows) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_threads"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_callstack"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_watch"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_breakpoints"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_output"));

  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_threads.vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_callstack.vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_watch.vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_breakpoints.vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_output.vbox.table"));

  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_threads"}));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_callstack"}));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_watch"}));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_breakpoints"}));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_output"}));
}

// ── update_threads ───────────────────────────────────────────────────────────

TEST_F(DbgRmiTest, UpdateThreadsPopulatesTable) {
  call("update_threads"_key,
       make_threads_args({
           {1, "running", "main"},
           {2, "suspended", "worker_loop"},
       }));
  EXPECT_EQ(row_count(root_ + "_threads.vbox.table"), 2u);
}

TEST_F(DbgRmiTest, UpdateThreadsFullRebuildReplacesRows) {
  call("update_threads"_key, make_threads_args({{1, "running", "main"}}));
  EXPECT_EQ(row_count(root_ + "_threads.vbox.table"), 1u);

  call("update_threads"_key,
       make_threads_args({
           {1, "suspended", "main"},
           {2, "running", "worker"},
           {3, "running", "io_thread"},
       }));
  EXPECT_EQ(row_count(root_ + "_threads.vbox.table"), 3u);
}

// ── update_callstack ─────────────────────────────────────────────────────────

TEST_F(DbgRmiTest, UpdateCallstackPopulatesTableAfterThreadSelected) {
  call("update_threads"_key, make_threads_args({{1, "suspended", "main"}}));
  auto table_id = id_at("_threads.vbox.table"); // not used directly; select via row_selected below
  (void)table_id;

  auto threads_table_id =
      srv_->last_session->ui_objects.at(root_ + "_threads.vbox.table")->as<bison::key_t>("__wish_id"_key);
  dynamic sel;
  sel["index"_key] = int32_t{0};
  fire_at(root_ + "_threads", threads_table_id, "row_selected"_key, std::move(sel));

  call("update_callstack"_key,
       make_callstack_args(1, {
                                   {0, "main", "main.cpp", 42},
                                   {1, "__libc_start_main", "libc.so", 0},
                               }));
  EXPECT_EQ(row_count(root_ + "_callstack.vbox.table"), 2u);
}

TEST_F(DbgRmiTest, UpdateCallstackWithStaleThreadIdIsNoOp) {
  call("update_threads"_key, make_threads_args({{1, "suspended", "main"}, {2, "suspended", "worker"}}));
  auto threads_table_id =
      srv_->last_session->ui_objects.at(root_ + "_threads.vbox.table")->as<bison::key_t>("__wish_id"_key);

  dynamic sel;
  sel["index"_key] = int32_t{0}; // selects thread id 1
  fire_at(root_ + "_threads", threads_table_id, "row_selected"_key, std::move(sel));

  // A snapshot computed for thread 2 (not the currently-selected thread 1)
  // must be discarded, not applied.
  call("update_callstack"_key, make_callstack_args(2, {{0, "worker_fn", "w.cpp", 10}}));
  EXPECT_EQ(row_count(root_ + "_callstack.vbox.table"), 0u);

  // A snapshot for the actually-selected thread (1) is applied normally.
  call("update_callstack"_key, make_callstack_args(1, {{0, "main", "main.cpp", 42}}));
  EXPECT_EQ(row_count(root_ + "_callstack.vbox.table"), 1u);
}

// ── Threads row click emits select_thread_requested ─────────────────────────

TEST_F(DbgRmiTest, ThreadsRowClickEmitsSelectThreadRequested) {
  call("update_threads"_key,
       make_threads_args({
           {10, "suspended", "main"},
           {20, "running", "worker"},
       }));
  auto threads_table_id =
      srv_->last_session->ui_objects.at(root_ + "_threads.vbox.table")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "select_thread_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic sel;
  sel["index"_key] = int32_t{1}; // second row -> thread id 20
  fire_at(root_ + "_threads", threads_table_id, "row_selected"_key, std::move(sel));
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<int32_t>("thread_id"_key), 20);
}

TEST_F(DbgRmiTest, CallstackRowClickEmitsSelectFrameRequested) {
  call("update_threads"_key, make_threads_args({{1, "suspended", "main"}}));
  auto threads_table_id =
      srv_->last_session->ui_objects.at(root_ + "_threads.vbox.table")->as<bison::key_t>("__wish_id"_key);
  dynamic sel;
  sel["index"_key] = int32_t{0};
  fire_at(root_ + "_threads", threads_table_id, "row_selected"_key, std::move(sel));

  call("update_callstack"_key,
       make_callstack_args(1, {
                                   {0, "main", "main.cpp", 42},
                                   {1, "callee", "callee.cpp", 7},
                               }));
  auto callstack_table_id =
      srv_->last_session->ui_objects.at(root_ + "_callstack.vbox.table")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "select_frame_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  dynamic fsel;
  fsel["index"_key] = int32_t{1};
  fire_at(root_ + "_callstack", callstack_table_id, "row_selected"_key, std::move(fsel));
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<int32_t>("frame_id"_key), 1);
}

// ── Closing any window tears down all six subtrees ──────────────────────────

TEST_F(DbgRmiTest, ClosingAnyWindowEmitsClosedAndTearsDownAllSix) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  auto win = window_id("_watch");
  fire_at(root_ + "_watch", win, "closed"_key);
  wait_for(got);
  EXPECT_TRUE(got);

  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_threads"));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_callstack"));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_watch"));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_breakpoints"));
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_output"));
}

// ── update_source ────────────────────────────────────────────────────────────

TEST_F(DbgRmiTest, UpdateSourceOpensTabAndSetsFields) {
  call("update_source"_key, make_source_args("main.cpp", {10, 20}, 10));

  auto tab_bar = get_dp(srv_->last_session->ui_objects, root_ + ".vbox.files");
  ASSERT_TRUE(tab_bar);
  auto tab = find_tab_item(tab_bar, "main.cpp");
  ASSERT_TRUE(tab);
  auto editor = find_child_of_class(tab, "TextEditor"_key);
  ASSERT_TRUE(editor);
  EXPECT_EQ(editor->as<std::string>("file_path"_key), "main.cpp");

  auto* lines_f = editor->findField<std::vector<int32_t>>("breakpoint_lines"_key);
  ASSERT_NE(lines_f, nullptr);
  EXPECT_EQ(*lines_f, (std::vector<int32_t>{10, 20}));
  EXPECT_EQ(editor->as<int32_t>("current_line"_key), 10);
}

TEST_F(DbgRmiTest, UpdateSourceReusesExistingTabForSamePath) {
  call("update_source"_key, make_source_args("main.cpp", {}, 5));
  call("update_source"_key, make_source_args("main.cpp", {7}, 8));

  auto tab_bar = get_dp(srv_->last_session->ui_objects, root_ + ".vbox.files");
  // Only one TabItem for "main.cpp" -- the second update_source call must
  // reuse the tab created by the first, not append a duplicate.
  EXPECT_EQ(count_children_of_class(tab_bar, "TabItem"_key), 1u);

  auto tab = find_tab_item(tab_bar, "main.cpp");
  auto editor = find_child_of_class(tab, "TextEditor"_key);
  ASSERT_TRUE(editor);
  EXPECT_EQ(editor->as<int32_t>("current_line"_key), 8);
}

// ── line_context_menu -> toggle_breakpoint_requested -> update_breakpoints ──

TEST_F(DbgRmiTest, LineContextMenuTogglesBreakpointAndRebuildsTable) {
  call("update_source"_key, make_source_args("main.cpp", {}, 0));

  auto tab_bar = get_dp(srv_->last_session->ui_objects, root_ + ".vbox.files");
  auto tab = find_tab_item(tab_bar, "main.cpp");
  auto editor = find_child_of_class(tab, "TextEditor"_key);
  ASSERT_TRUE(editor);
  auto editor_id = editor->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "toggle_breakpoint_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  // TextEditor's own gutter right-click -- text_editor.cpp: "Right-clicking
  // a line number emits 'line_context_menu'". The Source window's own
  // handler (debugger_frontend::on_event) turns this into
  // toggle_breakpoint_requested{path, line}.
  dynamic ctx_payload;
  ctx_payload["line"_key] = int32_t{20};
  ctx_payload["has_breakpoint"_key] = false;
  fire(editor_id, "line_context_menu"_key, std::move(ctx_payload));
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("path"_key), "main.cpp");
  EXPECT_EQ(cap.as<int32_t>("line"_key), 20);

  // Client's on_toggle_breakpoint_requested reaction would normally call
  // update_breakpoints with the full, newly-rebuilt list; simulate that
  // round trip and assert the Breakpoints table rebuilds correctly.
  call("update_breakpoints"_key, make_breakpoints_args({{"main.cpp", 20, true}}));
  EXPECT_EQ(row_count(root_ + "_breakpoints.vbox.table"), 1u);
}

// ── update_watch ──────────────────────────────────────────────────────────

struct fake_watch_entry {
  std::string name;
  std::string value;
  std::string type;
};

static dynamic make_watch_args(uint32_t frame_id, const std::vector<fake_watch_entry>& entries) {
  dynamic args;
  args["frame_id"_key] = static_cast<int32_t>(frame_id);
  dynamic arr;
  size_t i = 0;
  for (auto& e : entries) {
    auto ep = std::make_shared<dynamic>();
    (*ep)["name"_key] = e.name;
    (*ep)["value"_key] = e.value;
    (*ep)["type"_key] = e.type;
    arr[i++] = dynamic_ptr{ep};
  }
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

TEST_F(DbgRmiTest, UpdateWatchWithStaleFrameIdIsNoOp) {
  // Select thread 1's frame 0 (Call Stack row click sets has_selected_frame_
  // / selected_frame_id_ server-side, same guard mechanism as
  // update_callstack's thread_id staleness check).
  call("update_threads"_key, make_threads_args({{1, "suspended", "main"}}));
  auto threads_table_id =
      srv_->last_session->ui_objects.at(root_ + "_threads.vbox.table")->as<bison::key_t>("__wish_id"_key);
  dynamic tsel;
  tsel["index"_key] = int32_t{0};
  fire_at(root_ + "_threads", threads_table_id, "row_selected"_key, std::move(tsel));

  call("update_callstack"_key,
       make_callstack_args(1, {
                                   {0, "inner_function", "dbg_fixture.cpp", 24},
                                   {1, "outer_function", "dbg_fixture.cpp", 29},
                               }));
  auto callstack_table_id =
      srv_->last_session->ui_objects.at(root_ + "_callstack.vbox.table")->as<bison::key_t>("__wish_id"_key);
  dynamic fsel;
  fsel["index"_key] = int32_t{1}; // selects frame 1, not frame 0
  fire_at(root_ + "_callstack", callstack_table_id, "row_selected"_key, std::move(fsel));

  // A snapshot computed for frame 0 (not the currently-selected frame 1)
  // must be discarded, not applied.
  call("update_watch"_key, make_watch_args(0, {{"x", "0", "int"}}));
  EXPECT_EQ(row_count(root_ + "_watch.vbox.table"), 0u);

  // A snapshot for the actually-selected frame (1) is applied normally.
  call("update_watch"_key, make_watch_args(1, {{"x", "0", "int"}}));
  EXPECT_EQ(row_count(root_ + "_watch.vbox.table"), 1u);
}

TEST_F(DbgRmiTest, WatchAddButtonEmitsAddWatchRequestedWithTypedExpression) {
  auto expr_input_id = srv_->last_session->ui_objects.at(root_ + "_watch.vbox.toolbar.expr")
                            ->as<bison::key_t>("__wish_id"_key);
  auto add_button_id = srv_->last_session->ui_objects.at(root_ + "_watch.vbox.toolbar.add")
                            ->as<bison::key_t>("__wish_id"_key);

  dynamic changed;
  changed["value"_key] = std::string{"counter"};
  fire_at(root_ + "_watch", expr_input_id, "changed"_key, std::move(changed));

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "add_watch_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire_at(root_ + "_watch", add_button_id, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("expr"_key), "counter");
}

TEST_F(DbgRmiTest, WatchAddButtonWithEmptyExpressionIsNoOp) {
  auto add_button_id = srv_->last_session->ui_objects.at(root_ + "_watch.vbox.toolbar.add")
                            ->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "add_watch_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire_at(root_ + "_watch", add_button_id, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
}

// ── append_output ─────────────────────────────────────────────────────────

static dynamic make_output_args(const std::string& text, const std::string& level) {
  dynamic args;
  args["text"_key] = text;
  args["level"_key] = level;
  return args;
}

TEST_F(DbgRmiTest, AppendOutputColorCodesBySeverity) {
  call("append_output"_key, make_output_args("attach ok", "info"));
  call("append_output"_key, make_output_args("slow step", "warn"));
  call("append_output"_key, make_output_args("access violation", "error"));

  auto table = get_dp(srv_->last_session->ui_objects, root_ + "_output.vbox.table");
  ASSERT_TRUE(table);

  auto info_cell = nth_child(nth_child(table, 0), 0);
  ASSERT_TRUE(info_cell);
  EXPECT_EQ(info_cell->as<std::string>("text"_key), "[info] attach ok");
  EXPECT_EQ(info_cell->as<std::string>("text_color_light"_key), "#656D76FF");
  EXPECT_EQ(info_cell->as<std::string>("text_color_dark"_key), "#8B949EFF");

  auto warn_cell = nth_child(nth_child(table, 1), 0);
  ASSERT_TRUE(warn_cell);
  EXPECT_EQ(warn_cell->as<std::string>("text"_key), "[warn] slow step");
  EXPECT_EQ(warn_cell->as<std::string>("text_color_light"_key), "#9A6700FF");
  EXPECT_EQ(warn_cell->as<std::string>("text_color_dark"_key), "#D29922FF");

  auto error_cell = nth_child(nth_child(table, 2), 0);
  ASSERT_TRUE(error_cell);
  EXPECT_EQ(error_cell->as<std::string>("text"_key), "[error] access violation");
  EXPECT_EQ(error_cell->as<std::string>("text_color_light"_key), "#CF222EFF");
  EXPECT_EQ(error_cell->as<std::string>("text_color_dark"_key), "#F85149FF");
}

TEST_F(DbgRmiTest, AppendOutputFifoCapsAtMaxRowsAndEvictsOldest) {
  // 500 mirrors debugger_frontend's own kMaxOutputRows (server/dbg.hpp) --
  // not exposed via the header, so duplicated here; keep in sync if that
  // constant changes (same convention as test_tail.cpp's kMaxBufferedRows
  // comment).
  constexpr int kMaxOutputRows = 500;
  for (int i = 0; i < kMaxOutputRows + 2; ++i)
    call("append_output"_key, make_output_args("line " + std::to_string(i), "info"));

  EXPECT_EQ(row_count(root_ + "_output.vbox.table"), static_cast<size_t>(kMaxOutputRows));

  // Oldest two rows (line 0, line 1) were evicted; the table's first
  // remaining row is line 2 and the last is the most recently appended.
  auto table = get_dp(srv_->last_session->ui_objects, root_ + "_output.vbox.table");
  ASSERT_TRUE(table);
  auto first_cell = nth_child(nth_child(table, 0), 0);
  ASSERT_TRUE(first_cell);
  EXPECT_EQ(first_cell->as<std::string>("text"_key), "[info] line 2");
}
