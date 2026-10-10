// MIT License © 2026 Binary Dice Games
/// @file test_bdg_server_common.cpp
/// @brief Tests for modules/bdg/common/server: tool_form and the shared
///        panels, driven through a small test form over an in-memory RMI
///        connection.
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include "modules/bdg/common/server/console_panel.hpp"
#include "modules/bdg/common/server/list_panel.hpp"
#include "modules/bdg/common/server/rolling_plot.hpp"
#include "modules/bdg/common/server/text_viewer_panel.hpp"
#include "modules/bdg/common/server/tool_form.hpp"
#include "modules/bdg/common/server/ui_helpers.hpp"

#include <context/context.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace common = bdg::wish::common;
using namespace bdg::bison::rmi::transport;

namespace {

// ── Test form ───────────────────────────────────────────────────────────────

constexpr const char* kListLayout = R"json({
  "type": "Window", "title": "Items", "width": 400, "height": 300,
  "children": { "vbox": { "type": "VerticalLayout", "children": {
    "status": { "type": "Label", "text": "" },
    "table": {
      "type": "Table", "id": "##items", "columns": 2, "height": -1, "outer_height": -1,
      "children": {
        "col_name":    { "type": "TableColumn", "label": "Name", "column_id": 0 },
        "col_actions": { "type": "TableColumn", "label": "",     "column_id": 1 }
      }
    }
  } } }
})json";

constexpr const char* kPlotLayout = R"json({
  "type": "Window", "title": "Stats",
  "children": { "vbox": { "type": "VerticalLayout", "children": {
    "plot": { "type": "Plot", "title": "CPU", "children": {} }
  } } }
})json";

struct item_meta {
  std::string name;
};

/// Owns one of each shared panel; every outcome a test checks is emitted as
/// an event or visible in the session's ui_objects.
class common_test_form : public common::tool_form {
 public:
  explicit common_test_form(dynamic&& base) : tool_form(std::move(base)) {}

  dynamic do_append_log(const dynamic& args) {
    console_.append_from(args);
    return {};
  }

  // { names: "a,b,c" } -> one row per name, each with Open / - / Delete...
  dynamic do_set_rows(const dynamic& args) {
    list_.clear();
    std::string names = common::str_of(args, "names"_key);
    size_t start = 0;
    while (start <= names.size() && !names.empty()) {
      size_t comma = names.find(',', start);
      std::string name = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      list_.add(
          item_meta{name},
          {make_label(name)},
          {{"Open", [this, name] { emit("opened"_key, named(name)); }},
           {},
           {"Delete",
            [this, name] {
              show_confirm(
                  "Delete " + name + "?",
                  [this, name] { emit("deleted"_key, named(name)); },
                  {.title = "Confirm Delete", .on_no = [this, name] { emit("kept"_key, named(name)); }});
            },
            true}});
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
    list_.refresh();
    list_.set_status(common::plural(list_.rows().size(), "item"), true);
    return {};
  }

  dynamic do_filter(const dynamic& args) {
    const std::string needle = common::str_of(args, "needle"_key);
    list_.apply_filter([&](const item_meta& m) { return m.name.find(needle) != std::string::npos; });
    return {};
  }

  dynamic do_command_result(const dynamic& args) {
    bool ok = false;
    const std::string text = common::command_result_text(args, ok);
    list_.set_status(text, ok, /*first_line_only=*/true);
    return {};
  }

  dynamic do_set_text(const dynamic& args) {
    viewer_.set_title(common::str_of(args, "title"_key));
    viewer_.set_text(common::str_of(args, "text"_key));
    return {};
  }

  // { names: "a,b", values: "1,2" }
  dynamic do_push_sample(const dynamic& args) {
    std::map<std::string, float> values;
    std::string names = common::str_of(args, "names"_key) + ",";
    std::string nums = common::str_of(args, "values"_key) + ",";
    size_t a = 0, b = 0;
    while (true) {
      size_t ca = names.find(',', a), cb = nums.find(',', b);
      if (ca == std::string::npos || cb == std::string::npos || ca == a)
        break;
      values[names.substr(a, ca - a)] = std::stof(nums.substr(b, cb - b));
      a = ca + 1;
      b = cb + 1;
    }
    plot_.push(values);
    return {};
  }

  dynamic do_show_message(const dynamic& args) {
    show_message("Oops", common::str_of(args, "message"_key));
    return {};
  }

  common::rolling_plot plot_;

 protected:
  void on_init() override {
    internal_root_key_ = next_available_key("__common_test_");
    list_.build(*this, internal_root_key_, kListLayout);
    console_.build(
        *this,
        internal_root_key_ + "_console",
        {.title = "Log",
         .table_id = "##test_log",
         .width = 700,
         .command_width = 300,
         .closable = false,
         .on_cleared = [this] { emit("log_cleared"_key); },
         .on_copied = [this] { emit("log_copied"_key); }});
    viewer_.build(
        *this,
        internal_root_key_ + "_details",
        {.title = "Details", .language = "yaml", .file_stem = "details", .on_refresh = [this] {
           emit("refresh_requested"_key);
         }});
    bison::key_t plot_window;
    build_window(internal_root_key_ + "_stats", kPlotLayout, plot_window, [&](wish::ui_tree& tree) {
      tree.with("vbox.plot", [&](const auto& e) {
        common::rolling_plot::configure_axes(e, common::rolling_plot::y_axis::percent);
        plot_.init(*this, e, "Total");
      });
    });
  }

  void on_event(bison::key_t id, bison::key_t event, const dynamic&) override {
    if (event == "closed"_key && id == viewer_.window_id()) {
      viewer_.remove_file();
      return;
    }
    if (event == "clicked"_key)
      dispatch_click(id);
  }

 private:
  static dynamic named(const std::string& name) {
    dynamic d;
    d["name"_key] = name;
    return d;
  }

  common::list_panel<item_meta> list_;
  common::console_panel console_;
  common::text_viewer_panel viewer_;
};

void register_common_test_form() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto proto = dynamic_ptr{"CommonTestForm"_key, {}};
    auto add_method = [&](bison::key_t name, dynamic (common_test_form::*fn)(const dynamic&)) {
      proto->addMethod(name, bison::method{[fn](dynamic& self, const dynamic& args) -> dynamic {
                         return (static_cast<common_test_form&>(self).*fn)(args);
                       }});
    };
    add_method("append_log"_key, &common_test_form::do_append_log);
    add_method("set_rows"_key, &common_test_form::do_set_rows);
    add_method("filter"_key, &common_test_form::do_filter);
    add_method("command_result"_key, &common_test_form::do_command_result);
    add_method("set_text"_key, &common_test_form::do_set_text);
    add_method("push_sample"_key, &common_test_form::do_push_sample);
    add_method("show_message"_key, &common_test_form::do_show_message);
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        bison::key_t{0U},
        dynamic::make_factory<common_test_form>("wish"_key, "CommonTestForm"_key));
  });
}

// ── Fixture ─────────────────────────────────────────────────────────────────

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

dynamic log_args(const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
  dynamic a;
  a["command"_key] = command;
  a["exit_code"_key] = exit_code;
  a["ok"_key] = ok;
  a["output"_key] = output;
  return a;
}

dynamic one(bison::key_t k, const std::string& v) {
  dynamic d;
  d[k] = v;
  return d;
}

} // namespace

class BdgServerCommonTest : public ::testing::Test {
 protected:
  void SetUp() override {
    register_common_test_form();
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "CommonTestForm"_key).get());
    ASSERT_TRUE(proxy_->valid());
    for (const auto& [k, _] : objects())
      if (k.rfind("__common_test_", 0) == 0 && k.find('.') == std::string::npos && k.find('_', 14) == std::string::npos)
        root_ = k;
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  wish::ui_tree& objects() const {
    return srv_->last_session->ui_objects;
  }

  void call(bison::key_t method, dynamic args = {}) {
    proxy_->call(method, std::move(args)).get();
  }

  wish::ui_element_ptr at(const std::string& path) const {
    auto it = objects().find(path);
    return it == objects().end() ? nullptr : it->second;
  }

  static std::vector<dynamic_ptr> children(const dynamic_ptr& parent) {
    std::vector<dynamic_ptr> out;
    if (!parent)
      return out;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (cf && *cf)
      (*cf)->forEach([&](bison::key_t k, const field& f) {
        if (k.id < 0x10000 && f.is<dynamic_ptr>() && f.as<dynamic_ptr>()) // numeric (C++-added) keys
          out.push_back(f.as<dynamic_ptr>());
      });
    return out;
  }

  std::vector<dynamic_ptr> rows(const std::string& table_path) const {
    return children(at(table_path));
  }

  // __wish_id of the item labelled @p label in the last cell (MenuButton or
  // ContextMenu) of row @p row.
  bison::key_t item_id(const std::string& table_path, size_t row, const std::string& label) const {
    auto r = rows(table_path);
    if (row >= r.size())
      return {};
    auto cells = children(r[row]);
    if (cells.empty())
      return {};
    for (auto& item : children(cells.back())) {
      auto* lf = item->findField<std::string>("label"_key);
      if (lf && *lf == label)
        return item->as<bison::key_t>("__wish_id"_key);
    }
    return {};
  }

  static std::string text_of(const dynamic_ptr& el) {
    auto* f = el ? el->findField<std::string>("text"_key) : nullptr;
    return f ? *f : std::string{};
  }

  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event) {
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{handler_root});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end()) << handler_root;
    h->second->on_event(id, event, dynamic{});
  }

  // The main root is registered by form::init(); its events reach the form
  // directly.
  // (Every root of the form shares one handler: the form itself.)
  void fire_main(bison::key_t id, bison::key_t event) {
    fire_at(root_ + "_console", id, event);
  }

  std::string message_box_root() const {
    for (const auto& [k, _] : objects())
      if (k.rfind("__message_box_", 0) == 0 && k.find('.') == std::string::npos)
        return k;
    return {};
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bison::rmi::client> client_;
  std::optional<bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
};

// ── ui_helpers ──────────────────────────────────────────────────────────────

TEST(BdgServerCommonHelpers, CommandResultText) {
  dynamic ok_args = log_args("helm list", 0, true, "ignored");
  bool ok = false;
  EXPECT_EQ(common::command_result_text(ok_args, ok), "helm list: OK");
  EXPECT_TRUE(ok);

  dynamic failed = log_args("helm list", 1, false, "boom");
  EXPECT_EQ(common::command_result_text(failed, ok), "helm list failed: boom");
  EXPECT_FALSE(ok);

  dynamic silent = log_args("helm list", 1, false, "");
  EXPECT_EQ(common::command_result_text(silent, ok), "helm list failed: unknown error");
}

TEST(BdgServerCommonHelpers, PayloadReadersAndText) {
  dynamic d;
  d["s"_key] = std::string{"x"};
  d["b"_key] = true;
  EXPECT_EQ(common::str_of(d, "s"_key), "x");
  EXPECT_EQ(common::str_of(d, "missing"_key), "");
  EXPECT_EQ(common::str_of(d, "b"_key), ""); // wrong type
  EXPECT_TRUE(common::flag_of(d, "b"_key));
  EXPECT_FALSE(common::flag_of(d, "s"_key));
  EXPECT_EQ(common::plural(1, "pod"), "1 pod");
  EXPECT_EQ(common::plural(0, "pod"), "0 pods");
  EXPECT_EQ(common::lower("MiXeD"), "mixed");
}

TEST(BdgServerCommonHelpers, ActionIconsMatchFirstWordCaseInsensitively) {
  EXPECT_EQ(common::icon_path("settings"), "res/icons/settings.png");
  EXPECT_EQ(common::action_icon("Refresh"), "refresh");
  EXPECT_EQ(common::action_icon("remove"), "delete");
  EXPECT_EQ(common::action_icon("Prune stopped..."), "delete");
  EXPECT_EQ(common::action_icon("Rollback to this revision"), "undo");
  EXPECT_EQ(common::action_icon("Logs"), "document");
  EXPECT_EQ(common::action_icon("Frobnicate"), "");
  EXPECT_EQ(common::action_icon(""), "");
}

// ── console_panel ───────────────────────────────────────────────────────────

TEST_F(BdgServerCommonTest, ConsoleUsesOptionsAndAppendsColouredRows) {
  auto window = at(root_ + "_console");
  ASSERT_NE(window, nullptr);
  EXPECT_EQ(window->as<std::string>("title"_key), "Log");
  EXPECT_FALSE(window->as<bool>("closable"_key));
  EXPECT_EQ(at(root_ + "_console.vbox.table")->as<std::string>("id"_key), "##test_log");
  EXPECT_FLOAT_EQ(at(root_ + "_console.vbox.table.col_command")->as<float>("init_width"_key), 300.0f);

  call("append_log"_key, log_args("tool ok", 0, true, ""));
  call("append_log"_key, log_args("tool bad", 2, false, "nope"));
  auto r = rows(root_ + "_console.vbox.table");
  ASSERT_EQ(r.size(), 2u);
  auto cells = children(r[1]);
  ASSERT_EQ(cells.size(), 5u); // # / Command / Exit / Output / ContextMenu
  EXPECT_EQ(text_of(cells[0]), "2");
  EXPECT_EQ(text_of(cells[1]), "tool bad");
  EXPECT_EQ(text_of(cells[2]), "2");
  EXPECT_EQ(cells[1]->as<std::string>("text_color_light"_key), common::kBad.light);
  EXPECT_EQ(children(r[0])[1]->as<std::string>("text_color_dark"_key), common::kOk.dark);
}

TEST_F(BdgServerCommonTest, ConsoleEvictsOldestRowAndItsObjectsPastTheCap) {
  call("append_log"_key, log_args("first", 0, true, ""));
  const auto first_row_id = rows(root_ + "_console.vbox.table")[0]->as<bison::key_t>("__wish_id"_key);
  ASSERT_TRUE(srv_->last_session && first_row_id.id);

  for (size_t i = 1; i <= common::console_panel::kMaxRows; ++i)
    call("append_log"_key, log_args("cmd " + std::to_string(i), 0, true, ""));

  auto r = rows(root_ + "_console.vbox.table");
  ASSERT_EQ(r.size(), common::console_panel::kMaxRows);
  EXPECT_EQ(text_of(children(r.front())[1]), "cmd 1"); // "first" was evicted
  for (auto& row : r)
    EXPECT_NE(row->as<bison::key_t>("__wish_id"_key), first_row_id);
}

TEST_F(BdgServerCommonTest, ConsoleCopyAndClearNotifyAndClearRestartsSequence) {
  call("append_log"_key, log_args("a", 0, true, ""));
  call("append_log"_key, log_args("b", 0, true, ""));
  auto copy = item_id(root_ + "_console.vbox.table", 1, "Copy Entry");
  ASSERT_NE(copy.id, 0u);
  auto copy_rows = rows(root_ + "_console.vbox.table");
  auto ctx_menu = children(children(copy_rows[1]).back());
  EXPECT_EQ(ctx_menu[0]->as<std::string>("copy_text"_key), "b\nexit: 0\n");

  size_t copied_since = srv_->events->mark();
  fire_at(root_ + "_console", copy, "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("log_copied"_key, copied_since));

  auto clear = item_id(root_ + "_console.vbox.table", 0, "Clear Log");
  ASSERT_NE(clear.id, 0u);
  size_t since = srv_->events->mark();
  fire_at(root_ + "_console", clear, "clicked"_key);
  EXPECT_TRUE(rows(root_ + "_console.vbox.table").empty());
  EXPECT_TRUE(srv_->events->wait_for("log_cleared"_key, since));

  call("append_log"_key, log_args("c", 0, true, ""));
  EXPECT_EQ(text_of(children(rows(root_ + "_console.vbox.table")[0])[0]), "1");
}

// ── list_panel / tool_form ──────────────────────────────────────────────────

TEST_F(BdgServerCommonTest, ListRowsCarryMenuAndRebuildReplacesThem) {
  call("set_rows"_key, one("names"_key, "alpha,beta"));
  EXPECT_EQ(rows(root_ + ".vbox.table").size(), 2u);
  EXPECT_EQ(text_of(at(root_ + ".vbox.status")), "2 items");
  EXPECT_NE(item_id(root_ + ".vbox.table", 0, "Delete...").id, 0u); // confirm adds "..."
  auto button = children(rows(root_ + ".vbox.table")[0]).back();
  EXPECT_EQ(button->as<std::string>("label"_key), "");
  EXPECT_EQ(button->as<std::string>("icon"_key), "res/icons/more.png");
  auto menu = children(button);
  ASSERT_EQ(menu.size(), 3u);
  EXPECT_EQ(menu[0]->as<std::string>("icon"_key), "res/icons/folder_open.png"); // "Open"
  EXPECT_EQ(menu[1]->as<bison::key_t>(dynamic::CLASS), "Separator"_key);
  EXPECT_EQ(menu[2]->as<std::string>("icon"_key), "res/icons/delete.png"); // "Delete..."

  call("set_rows"_key, one("names"_key, "gamma"));
  EXPECT_EQ(rows(root_ + ".vbox.table").size(), 1u);
  EXPECT_EQ(text_of(at(root_ + ".vbox.status")), "1 item");
}

TEST_F(BdgServerCommonTest, ListMenuItemRunsItsCallback) {
  call("set_rows"_key, one("names"_key, "alpha,beta"));
  size_t since = srv_->events->mark();
  fire_main(item_id(root_ + ".vbox.table", 1, "Open"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for(
      "opened"_key, since, [](const dynamic& p) { return common::str_of(p, "name"_key) == "beta"; }));
}

TEST_F(BdgServerCommonTest, ListFilterTogglesRowVisibility) {
  call("set_rows"_key, one("names"_key, "alpha,beta,alphabet"));
  call("filter"_key, one("needle"_key, "alpha"));
  auto r = rows(root_ + ".vbox.table");
  ASSERT_EQ(r.size(), 3u);
  EXPECT_TRUE(r[0]->as<bool>("visible"_key));
  EXPECT_FALSE(r[1]->as<bool>("visible"_key));
  EXPECT_TRUE(r[2]->as<bool>("visible"_key));
}

TEST_F(BdgServerCommonTest, CommandResultWritesFirstLineInRed) {
  call("command_result"_key, log_args("tool x", 1, false, "line one\nline two"));
  auto status = at(root_ + ".vbox.status");
  EXPECT_EQ(text_of(status), "tool x failed: line one");
  EXPECT_EQ(status->as<std::string>("text_color_light"_key), common::kBad.light);
  call("command_result"_key, log_args("tool x", 0, true, ""));
  EXPECT_EQ(status->as<std::string>("text_color_light"_key), common::kIdle.light);
}

TEST_F(BdgServerCommonTest, ConfirmYesRunsOnYesAndNoRunsOnNo) {
  call("set_rows"_key, one("names"_key, "alpha"));

  size_t since = srv_->events->mark();
  fire_main(item_id(root_ + ".vbox.table", 0, "Delete..."), "clicked"_key);
  std::string box = message_box_root();
  ASSERT_FALSE(box.empty());
  EXPECT_EQ(at(box)->as<std::string>("title"_key), "Confirm Delete");
  EXPECT_NE(text_of(at(box + ".body.message")).find("alpha"), std::string::npos);
  EXPECT_FALSE(srv_->events->wait_for("deleted"_key, since, {}, std::chrono::milliseconds(50)));
  fire_at(box, at(box + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key), "clicked"_key); // Yes
  EXPECT_TRUE(srv_->events->wait_for("deleted"_key, since));

  since = srv_->events->mark();
  fire_main(item_id(root_ + ".vbox.table", 0, "Delete..."), "clicked"_key);
  box = message_box_root();
  ASSERT_FALSE(box.empty());
  fire_at(box, at(box + ".buttons.btn1")->as<bison::key_t>("__wish_id"_key), "clicked"_key); // No
  EXPECT_TRUE(srv_->events->wait_for("kept"_key, since));
  EXPECT_FALSE(srv_->events->wait_for("deleted"_key, since, {}, std::chrono::milliseconds(50)));
}

TEST_F(BdgServerCommonTest, ShowMessageOpensAnOkBox) {
  call("show_message"_key, one("message"_key, "it broke"));
  std::string box = message_box_root();
  ASSERT_FALSE(box.empty());
  EXPECT_EQ(at(box)->as<std::string>("title"_key), "Oops");
  EXPECT_NE(text_of(at(box + ".body.message")).find("it broke"), std::string::npos);
  EXPECT_EQ(at(box + ".buttons.btn1"), nullptr); // a single OK button
}

// ── text_viewer_panel ───────────────────────────────────────────────────────

TEST_F(BdgServerCommonTest, TextViewerUsesOptionsAndRefreshButton) {
  EXPECT_EQ(at(root_ + "_details")->as<std::string>("title"_key), "Details");
  auto editor = at(root_ + "_details.vbox.editor");
  ASSERT_NE(editor, nullptr);
  EXPECT_EQ(editor->as<std::string>("language"_key), "yaml");
  EXPECT_TRUE(editor->as<bool>("read_only"_key));
  EXPECT_EQ(text_of(at(root_ + "_details.vbox.toolbar.target")), "(nothing selected)");

  size_t since = srv_->events->mark();
  fire_at(
      root_ + "_details",
      at(root_ + "_details.vbox.toolbar.btn_refresh")->as<bison::key_t>("__wish_id"_key),
      "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("refresh_requested"_key, since));
}

TEST_F(BdgServerCommonTest, TextViewerWritesSandboxFilesAndDeletesTheOldOne) {
  const auto dir = srv_->last_session->resource_dir;
  auto editor = at(root_ + "_details.vbox.editor");
  auto read = [&](const std::string& rel) {
    std::ifstream in(dir / rel, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
  };

  dynamic a;
  a["title"_key] = std::string{"release: web"};
  a["text"_key] = std::string{"key: 1\n"};
  call("set_text"_key, a.clone());
  const std::string first = editor->as<std::string>("file_path"_key);
  EXPECT_EQ(first.rfind("private/" + root_ + "_details_", 0), 0u) << first;
  EXPECT_EQ(read(first), "key: 1\n");
  EXPECT_EQ(text_of(at(root_ + "_details.vbox.toolbar.target")), "release: web");

  a["text"_key] = std::string{"key: 2\n"};
  call("set_text"_key, a.clone());
  const std::string second = editor->as<std::string>("file_path"_key);
  EXPECT_NE(second, first);
  EXPECT_EQ(read(second), "key: 2\n");
  EXPECT_FALSE(std::filesystem::exists(dir / first));

  fire_at(root_ + "_details", at(root_ + "_details")->as<bison::key_t>("__wish_id"_key), "closed"_key);
  EXPECT_FALSE(std::filesystem::exists(dir / second));
}

// ── rolling_plot ────────────────────────────────────────────────────────────

TEST_F(BdgServerCommonTest, RollingPlotKeepsAggregateAndTopSeries) {
  auto plot = at(root_ + "_stats.vbox.plot");
  ASSERT_NE(plot, nullptr);
  EXPECT_FLOAT_EQ(plot->as<float>("y_max"_key), 100.0f);
  ASSERT_EQ(children(plot).size(), 1u); // the aggregate line

  dynamic s;
  s["names"_key] = std::string{"a,b"};
  s["values"_key] = std::string{"1,2"};
  call("push_sample"_key, s.clone());
  EXPECT_EQ(children(plot).size(), 3u);
  for (auto& line : children(plot))
    if (line->as<std::string>("label"_key) == "Total")
      EXPECT_FLOAT_EQ(line->as<std::vector<float>>("ys"_key).back(), 3.0f);

  s["names"_key] = std::string{"b,c"};
  s["values"_key] = std::string{"5,1"};
  call("push_sample"_key, s.clone());
  EXPECT_EQ(children(plot).size(), 3u); // "a" left, "c" joined
  auto lines = children(plot);
  for (auto& line : lines) {
    auto label = line->as<std::string>("label"_key);
    auto ys = line->as<std::vector<float>>("ys"_key);
    EXPECT_EQ(ys.size(), 2u) << label;
    if (label == "c")
      EXPECT_TRUE(std::isnan(ys[0])); // late series lines up with the aggregate
  }
}

TEST_F(BdgServerCommonTest, RollingPlotCapsSeriesAndHistory) {
  std::string names, values;
  for (size_t i = 0; i < common::rolling_plot::kMaxSeries + 5; ++i) {
    names += (i ? "," : "") + std::string{"s"} + std::to_string(i);
    values += (i ? "," : "") + std::to_string(i);
  }
  dynamic s;
  s["names"_key] = names;
  s["values"_key] = values;
  for (size_t i = 0; i < common::rolling_plot::kMaxHistory + 3; ++i)
    call("push_sample"_key, s.clone());

  auto lines = children(at(root_ + "_stats.vbox.plot"));
  EXPECT_EQ(lines.size(), common::rolling_plot::kMaxSeries + 1);
  for (auto& line : lines) {
    EXPECT_EQ(line->as<std::vector<float>>("ys"_key).size(), common::rolling_plot::kMaxHistory);
    EXPECT_NE(line->as<std::string>("label"_key), "s0"); // the smallest series lost its line
  }
}
