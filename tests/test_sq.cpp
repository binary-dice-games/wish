// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include "modules/bdg/dev/sq/server/sq.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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

using proxy_t = bdg::bison::rmi::proxy::dynamic;

dynamic_ptr make_obj() {
  return std::make_shared<dynamic>();
}

// { handle, driver, location } entries + active handle.
dynamic make_connections(const std::string& active, const std::vector<std::pair<std::string, std::string>>& conns) {
  dynamic arr;
  size_t i = 0;
  for (auto& [handle, driver] : conns) {
    auto e = make_obj();
    (*e)["handle"_key] = handle;
    (*e)["driver"_key] = driver;
    (*e)["location"_key] = std::string{"loc-of-"} + handle;
    arr[i++] = dynamic_ptr{e};
  }
  dynamic args;
  args["active"_key] = active;
  args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

// One "album" table (3 columns, one FK) and one view.
dynamic make_schema(const std::string& driver = "sqlite3") {
  auto column = [](const std::string& name, const std::string& type, bool pk, const std::string& fk) {
    auto c = make_obj();
    (*c)["name"_key] = name;
    (*c)["type"_key] = type;
    (*c)["pk"_key] = pk;
    (*c)["nullable"_key] = !pk;
    (*c)["fk"_key] = fk;
    return dynamic_ptr{c};
  };
  auto table = [&](const std::string& name, const std::string& type, std::vector<dynamic_ptr> cols) {
    dynamic carr;
    size_t i = 0;
    for (auto& c : cols)
      carr[i++] = c;
    auto t = make_obj();
    (*t)["name"_key] = name;
    (*t)["type"_key] = type;
    (*t)["rows"_key] = int32_t{2};
    (*t)["columns"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(carr))};
    return dynamic_ptr{t};
  };
  dynamic tarr;
  tarr[size_t{0}] = table(
      "album", "table", {column("id", "INTEGER", true, ""), column("title", "TEXT", false, ""), column("artist_id", "INTEGER", false, "artist(id)")});
  tarr[size_t{1}] = table("v_a", "view", {column("id", "INTEGER", false, "")});
  dynamic args;
  args["handle"_key] = std::string{"@demo"};
  args["driver"_key] = driver;
  args["product"_key] = std::string{"SQLite3"};
  args["tables"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(tarr))};
  return args;
}

// Columns {a, b}; rows are vectors of cells ("\x01" == NULL).
dynamic make_result(const std::vector<std::vector<std::string>>& rows, int32_t total, bool truncated) {
  dynamic cols;
  cols[size_t{0}] = [] { auto c = make_obj(); (*c)["name"_key] = std::string{"a"}; return dynamic_ptr{c}; }();
  cols[size_t{1}] = [] { auto c = make_obj(); (*c)["name"_key] = std::string{"b"}; return dynamic_ptr{c}; }();
  dynamic rarr;
  size_t i = 0;
  for (auto& r : rows) {
    auto row = make_obj();
    size_t j = 0;
    for (auto& cell : r)
      (*row)[j++] = cell;
    rarr[i++] = dynamic_ptr{row};
  }
  dynamic args;
  args["ok"_key] = true;
  args["sql"_key] = std::string{"select a, b from t"};
  args["elapsed_ms"_key] = int32_t{5};
  args["total_rows"_key] = total;
  args["truncated"_key] = truncated;
  args["columns"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(cols))};
  args["rows"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(rarr))};
  return args;
}


// ── Session-capturing server ────────────────────────────────────────────────

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

// The windows this form registers are keyed "__sq_N" (the Containers
// main root) and "__sq_N_<suffix>" for images/volumes/networks/logs/
// inspect -- find the bare main root, mirroring test_git.cpp's
// find_form_root().
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__sq_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__sq_N_<suffix>": a second '_' after the numeric index.
    if (k.find('_', 5) != std::string::npos)
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


class SqRmiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "SqFrontend"_key).get());
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

  dynamic_ptr obj(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    return it == srv_->last_session->ui_objects.end() ? nullptr : it->second;
  }

  size_t child_elem_count(const std::string& path) const {
    auto o = obj(path);
    if (!o)
      return 0;
    auto* cf = o->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    size_t n = 0;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
        ++n;
    });
    return n;
  }

  // Child of @p parent whose class is @p cls, the @p n-th of them.
  static std::vector<dynamic_ptr> children_of(const dynamic_ptr& parent) {
    std::vector<dynamic_ptr> out;
    if (!parent)
      return out;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return out;
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (f.is<dynamic_ptr>() && f.as<dynamic_ptr>())
        out.push_back(f.as<dynamic_ptr>());
    });
    return out;
  }

  static std::string text_of(const dynamic_ptr& label) {
    return label ? label->as<std::string>("text"_key) : std::string{};
  }

  // Recursively finds the first element under @p node whose field @p field
  // equals @p value.
  static dynamic_ptr find_by(const dynamic_ptr& node, const char* field_name, const std::string& value) {
    if (!node)
      return nullptr;
    auto* f = node->findField<std::string>(bison::key_t{field_name});
    if (f && *f == value)
      return node;
    for (auto& c : children_of(node))
      if (auto hit = find_by(c, field_name, value))
        return hit;
    return nullptr;
  }

  // The editor is a TextEditor bound to a sandbox file: tests "type" by
  // writing the file the widget points at, and read the text back from it.
  std::filesystem::path sql_file() const {
    auto ed = obj(root_ + ".vbox.sql");
    return srv_->last_session->resource_dir / ed->as<std::string>("file_path"_key);
  }
  void set_sql(const std::string& text) {
    std::ofstream(sql_file(), std::ios::binary | std::ios::trunc) << text;
  }
  std::string read_sql() const {
    std::ifstream f(sql_file(), std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(f), {}};
  }

  static bison::key_t element_id(const dynamic_ptr& e) {
    return e ? e->as<bison::key_t>("__wish_id"_key) : bison::key_t{};
  }

  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }

  struct emitted {
    bison::key_t name;
    dynamic payload;
  };
  // Records every emitted event whose name is in @p names.
  void capture(std::vector<emitted>& out) {
    auto prev = std::move(srv_->last_session->emit_event);
    srv_->last_session->emit_event = [&out, prev](bison::key_t id, bison::key_t event, dynamic payload) {
      if (event.id != "clicked"_key.id && event.id != "changed"_key.id && event.id != "closed"_key.id) {
        emitted e;
        e.name = event;
        e.payload = payload.clone();
        out.push_back(std::move(e));
      }
      if (prev)
        prev(id, event, std::move(payload));
    };
  }

  // Events reach the client callback asynchronously; poll until @p out holds
  // at least @p n of them (or 2 s pass).
  static void wait_events(const std::vector<emitted>& out, size_t n) {
    auto t0 = std::chrono::steady_clock::now();
    while (out.size() < n && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

// ── Construction ───────────────────────────────────────────────────────────

TEST_F(SqRmiTest, InstantiationBuildsAllSixWindows) {
  for (const char* suffix : {"", "_connections", "_navigator", "_structure", "_results", "_console"})
    EXPECT_TRUE(obj(root_ + suffix)) << suffix;
  EXPECT_TRUE(obj(root_ + ".vbox.sql"));
  EXPECT_TRUE(obj(root_ + ".vbox.toolbar.btn_run"));
  EXPECT_TRUE(obj(root_ + "_results.vbox.export.btn_export"));
}

TEST_F(SqRmiTest, QuoteIdentifierFollowsTheDriver) {
  using bdg::wish::sq_frontend;
  EXPECT_EQ(sq_frontend::quote_identifier("sqlite3", "my table"), "\"my table\"");
  EXPECT_EQ(sq_frontend::quote_identifier("postgres", "a\"b"), "\"a\"\"b\"");
  EXPECT_EQ(sq_frontend::quote_identifier("mysql", "t`x"), "`t``x`");
  EXPECT_EQ(sq_frontend::quote_identifier("sqlserver", "a]b"), "[a]]b]");
}

// ── Connections ────────────────────────────────────────────────────────────

TEST_F(SqRmiTest, UpdateConnectionsFillsTableAndPicker) {
  call("update_connections"_key, make_connections("@b", {{"@a", "sqlite3"}, {"@b", "postgres"}}));
  EXPECT_EQ(child_elem_count(root_ + "_connections.vbox.table") - 5u, 2u); // 5 JSON columns
  auto combo = obj(root_ + ".vbox.toolbar.conn");
  ASSERT_TRUE(combo);
  EXPECT_EQ(combo->as<std::string>("items"_key), "@a\n@b");
  EXPECT_EQ(combo->as<int32_t>("value"_key), 1);
}

TEST_F(SqRmiTest, RebuildingConnectionsIsIdempotentInRowCount) {
  auto args = make_connections("@a", {{"@a", "sqlite3"}});
  call("update_connections"_key, args.clone());
  call("update_connections"_key, args.clone());
  EXPECT_EQ(child_elem_count(root_ + "_connections.vbox.table") - 5u, 1u);
}

TEST_F(SqRmiTest, PickerChangeEmitsActivateForAnotherHandle) {
  call("update_connections"_key, make_connections("@a", {{"@a", "sqlite3"}, {"@b", "postgres"}}));
  std::vector<emitted> got;
  capture(got);
  const auto combo_id = element_id(obj(root_ + ".vbox.toolbar.conn"));

  dynamic same;
  same["value"_key] = int32_t{0};
  fire_at(root_, combo_id, "changed"_key, std::move(same));
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // events are async
  EXPECT_TRUE(got.empty()) << "selecting the already-active handle is a no-op";

  dynamic other;
  other["value"_key] = int32_t{1};
  fire_at(root_, combo_id, "changed"_key, std::move(other));
  wait_events(got, 1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "activate_requested"_key);
  EXPECT_EQ(got[0].payload.as<std::string>("handle"_key), "@b");
}

TEST_F(SqRmiTest, RemoveIsHeldBackUntilConfirmed) {
  call("update_connections"_key, make_connections("@a", {{"@a", "sqlite3"}, {"@b", "postgres"}}));
  std::vector<emitted> got;
  capture(got);

  auto item = find_by(obj(root_ + "_connections.vbox.table"), "label", "Remove...");
  ASSERT_TRUE(item);
  fire_at(root_ + "_connections", element_id(item), "clicked"_key);
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // events are async
  EXPECT_TRUE(got.empty());
  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}

TEST_F(SqRmiTest, AddConnectionEmitsFormFieldsAndClearsPassword) {
  auto loc = obj(root_ + "_connections.vbox.new_row_loc.location");
  auto pw = obj(root_ + "_connections.vbox.new_row2.password");
  ASSERT_TRUE(loc && pw);
  (*loc)["value"_key] = std::string{"postgres://u@h/db"};
  (*pw)["value"_key] = std::string{"pw"};

  std::vector<emitted> got;
  capture(got);
  fire_at(root_ + "_connections", element_id(obj(root_ + "_connections.vbox.new_row2.btn_add")), "clicked"_key);
  wait_events(got, 1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "add_connection_requested"_key);
  EXPECT_EQ(got[0].payload.as<std::string>("location"_key), "postgres://u@h/db");
  EXPECT_EQ(got[0].payload.as<std::string>("password"_key), "pw");
  EXPECT_EQ(pw->as<std::string>("value"_key), "");
}

TEST_F(SqRmiTest, AddWithoutLocationDoesNotEmit) {
  std::vector<emitted> got;
  capture(got);
  fire_at(root_ + "_connections", element_id(obj(root_ + "_connections.vbox.new_row2.btn_add")), "clicked"_key);
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // events are async
  EXPECT_TRUE(got.empty());
}

// ── Navigator / Structure ──────────────────────────────────────────────────

TEST_F(SqRmiTest, UpdateSchemaBuildsATreeOfTablesViewsAndColumns) {
  call("update_schema"_key, make_schema());
  auto nav = obj(root_ + "_navigator.vbox.tree");
  ASSERT_TRUE(nav);
  EXPECT_TRUE(find_by(nav, "label", "Tables (1)"));
  EXPECT_TRUE(find_by(nav, "label", "Views (1)"));
  EXPECT_TRUE(find_by(nav, "label", "album  [2 rows]"));
  EXPECT_TRUE(find_by(nav, "label", "id : INTEGER  [PK]"));
  EXPECT_TRUE(find_by(nav, "label", "artist_id : INTEGER  -> artist(id)"));
}

TEST_F(SqRmiTest, SchemaErrorReplacesTheTree) {
  auto args = make_schema();
  args["error"_key] = std::string{"boom"};
  call("update_schema"_key, std::move(args));
  auto nav = obj(root_ + "_navigator.vbox.tree");
  EXPECT_TRUE(find_by(nav, "text", "boom"));
  EXPECT_FALSE(find_by(nav, "label", "Tables (1)"));
}

TEST_F(SqRmiTest, ViewDataButtonPutsQuotedSelectInEditorAndEmitsQuery) {
  call("update_schema"_key, make_schema());
  auto nav = obj(root_ + "_navigator.vbox.tree");
  auto album = find_by(nav, "label", "album  [2 rows]");
  ASSERT_TRUE(album);
  auto button = find_by(album, "label", "View data");
  ASSERT_TRUE(button);

  std::vector<emitted> got;
  capture(got);
  fire_at(root_ + "_navigator", element_id(button), "clicked"_key);
  wait_events(got, 1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "query_requested"_key);
  EXPECT_EQ(got[0].payload.as<std::string>("sql"_key), "SELECT * FROM \"album\"");
  EXPECT_EQ(got[0].payload.as<int32_t>("max_rows"_key), 500);
  EXPECT_EQ(read_sql(), "SELECT * FROM \"album\"");
}

TEST_F(SqRmiTest, StructureButtonFillsTheColumnTable) {
  call("update_schema"_key, make_schema());
  auto button = find_by(find_by(obj(root_ + "_navigator.vbox.tree"), "label", "album  [2 rows]"), "label", "Structure");
  ASSERT_TRUE(button);
  fire_at(root_ + "_navigator", element_id(button), "clicked"_key);

  EXPECT_EQ(text_of(obj(root_ + "_structure.vbox.title")), "album  (table, 2 rows)");
  EXPECT_EQ(child_elem_count(root_ + "_structure.vbox.table") - 6u, 3u); // 6 JSON columns
  EXPECT_TRUE(find_by(obj(root_ + "_structure.vbox.table"), "text", "artist(id)"));
  EXPECT_TRUE(find_by(obj(root_ + "_structure.vbox.table"), "text", "PK"));
}

TEST_F(SqRmiTest, NewSchemaResetsTheStructureWindow) {
  call("update_schema"_key, make_schema());
  auto button = find_by(find_by(obj(root_ + "_navigator.vbox.tree"), "label", "album  [2 rows]"), "label", "Structure");
  fire_at(root_ + "_navigator", element_id(button), "clicked"_key);
  call("update_schema"_key, make_schema());
  EXPECT_EQ(child_elem_count(root_ + "_structure.vbox.table"), 6u);
}

// ── Editor / Results ───────────────────────────────────────────────────────

TEST_F(SqRmiTest, RunEmitsTheEditorTextAndRowLimit) {
  set_sql("select 1");
  (*obj(root_ + ".vbox.toolbar.rows"))["value"_key] = int32_t{2};
  std::vector<emitted> got;
  capture(got);
  fire_at(root_, element_id(obj(root_ + ".vbox.toolbar.btn_run")), "clicked"_key);
  wait_events(got, 1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "query_requested"_key);
  EXPECT_EQ(got[0].payload.as<std::string>("sql"_key), "select 1");
  EXPECT_EQ(got[0].payload.as<int32_t>("max_rows"_key), 1000);
}

TEST_F(SqRmiTest, EditorIsASqlTextEditorFillingItsWindow) {
  auto ed = obj(root_ + ".vbox.sql");
  ASSERT_TRUE(ed);
  EXPECT_EQ(ed->as<std::string>("language"_key), "sql");
  EXPECT_EQ(ed->as<int32_t>("height"_key), -1);
  EXPECT_EQ(ed->as<int32_t>("width"_key), -1);
  EXPECT_FALSE(ed->as<std::string>("file_path"_key).empty());
}

TEST_F(SqRmiTest, ViewDataReplacesTheEditorFileSoTheWidgetReloads) {
  call("update_schema"_key, make_schema());
  const auto before = obj(root_ + ".vbox.sql")->as<std::string>("file_path"_key);
  set_sql("select 42"); // pending user text
  auto button = find_by(find_by(obj(root_ + "_navigator.vbox.tree"), "label", "album  [2 rows]"), "label", "View data");
  fire_at(root_ + "_navigator", element_id(button), "clicked"_key);
  EXPECT_NE(obj(root_ + ".vbox.sql")->as<std::string>("file_path"_key), before);
  EXPECT_EQ(read_sql(), "SELECT * FROM \"album\"");
}

TEST_F(SqRmiTest, RunWithBlankEditorDoesNotEmit) {
  std::vector<emitted> got;
  capture(got);
  fire_at(root_, element_id(obj(root_ + ".vbox.toolbar.btn_run")), "clicked"_key);
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // events are async
  EXPECT_TRUE(got.empty());
  EXPECT_NE(text_of(obj(root_ + ".vbox.status")).find("query"), std::string::npos);
}

TEST_F(SqRmiTest, UpdateResultBuildsColumnsRowsAndNullCells) {
  call("update_result"_key, make_result({{"1", "x"}, {"2", "\x01"}}, 2, false));
  auto table = obj(root_ + "_results.vbox.table");
  ASSERT_TRUE(table);
  EXPECT_EQ(table->as<int32_t>("columns"_key), 3); // "#" + a + b
  EXPECT_EQ(child_elem_count(root_ + "_results.vbox.table"), 3u + 2u);
  EXPECT_TRUE(find_by(table, "label", "a"));
  EXPECT_TRUE(find_by(table, "text", "x"));
  EXPECT_TRUE(find_by(table, "text", "NULL"));
  EXPECT_EQ(text_of(obj(root_ + "_results.vbox.status")).rfind("2 rows in 5 ms", 0), 0u);
}

TEST_F(SqRmiTest, EachResultGetsAFreshTableIdAndReplacesTheLastOne) {
  call("update_result"_key, make_result({{"1", "x"}}, 1, false));
  auto table = obj(root_ + "_results.vbox.table");
  const auto first_id = table->as<std::string>("id"_key);
  call("update_result"_key, make_result({{"1", "x"}, {"2", "y"}, {"3", "z"}}, 3, false));
  EXPECT_NE(table->as<std::string>("id"_key), first_id);
  EXPECT_EQ(child_elem_count(root_ + "_results.vbox.table"), 3u + 3u);
}

TEST_F(SqRmiTest, TruncatedResultSaysSoInTheStatus) {
  call("update_result"_key, make_result({{"1", "x"}}, 9000, true));
  const auto status = text_of(obj(root_ + "_results.vbox.status"));
  EXPECT_NE(status.find("9000 rows"), std::string::npos);
  EXPECT_NE(status.find("first 1"), std::string::npos);
}

TEST_F(SqRmiTest, FailedResultShowsTheErrorAndClearsTheGrid) {
  call("update_result"_key, make_result({{"1", "x"}}, 1, false));
  dynamic args;
  args["ok"_key] = false;
  args["error"_key] = std::string{"no such table: nope"};
  call("update_result"_key, std::move(args));
  EXPECT_EQ(text_of(obj(root_ + ".vbox.status")), "no such table: nope");
  EXPECT_EQ(child_elem_count(root_ + "_results.vbox.table"), 0u);
}

TEST_F(SqRmiTest, ExportEmitsPathAndOverwriteFlag) {
  (*obj(root_ + "_results.vbox.export.path"))["value"_key] = std::string{"/tmp/out.csv"};
  (*obj(root_ + "_results.vbox.export.overwrite"))["value"_key] = true;
  std::vector<emitted> got;
  capture(got);
  fire_at(root_, element_id(obj(root_ + "_results.vbox.export.btn_export")), "clicked"_key);
  wait_events(got, 1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].name, "export_requested"_key);
  EXPECT_EQ(got[0].payload.as<std::string>("path"_key), "/tmp/out.csv");
  EXPECT_TRUE(got[0].payload.as<bool>("overwrite"_key));
}

// ── Console / status / unavailable ─────────────────────────────────────────

TEST_F(SqRmiTest, ConsoleRowsAreAppendedAndFifoCapped) {
  for (int i = 0; i < 502; ++i) {
    dynamic args;
    args["command"_key] = std::string{"sq ls"};
    args["exit_code"_key] = int32_t{0};
    args["ok"_key] = true;
    args["output"_key] = std::string{};
    call("append_command_log"_key, std::move(args));
  }
  EXPECT_EQ(child_elem_count(root_ + "_console.vbox.table") - 4u, 500u); // 4 JSON columns
}

TEST_F(SqRmiTest, ResultsScopedCommandResultWritesTheResultsStatus) {
  dynamic args;
  args["scope"_key] = std::string{"results"};
  args["ok"_key] = true;
  args["message"_key] = std::string{"Exported"};
  call("command_result"_key, std::move(args));
  EXPECT_EQ(text_of(obj(root_ + "_results.vbox.status")), "Exported");
}

TEST_F(SqRmiTest, CommandResultWritesTheScopedStatusLabel) {
  dynamic args;
  args["scope"_key] = std::string{"connections"};
  args["ok"_key] = false;
  args["message"_key] = std::string{"boom"};
  call("command_result"_key, std::move(args));
  EXPECT_EQ(text_of(obj(root_ + "_connections.vbox.status")), "boom");
  EXPECT_NE(text_of(obj(root_ + ".vbox.status")), "boom");
}

TEST_F(SqRmiTest, ShowUnavailableSetsBannersAndOpensADialog) {
  dynamic args;
  args["message"_key] = std::string{"sq not found: https://github.com/neilotoole/sq"};
  args["url"_key] = std::string{"https://github.com/neilotoole/sq"};
  call("show_unavailable"_key, std::move(args));
  EXPECT_NE(text_of(obj(root_ + ".vbox.status")).find("github.com/neilotoole/sq"), std::string::npos);
  EXPECT_NE(text_of(obj(root_ + "_connections.vbox.status")).find("github.com/neilotoole/sq"), std::string::npos);
  EXPECT_FALSE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}

TEST_F(SqRmiTest, ClosingAnyWindowEmitsClosedAndTearsDownEveryRoot) {
  bool closed = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      closed = true;
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(root_ + "_results", element_id(obj(root_ + "_results")), "closed"_key);
  for (int i = 0; i < 400 && !closed; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(closed);
  for (const char* suffix : {"_connections", "_navigator", "_structure", "_results", "_console"})
    EXPECT_FALSE(obj(root_ + suffix)) << suffix;
}

} // namespace
