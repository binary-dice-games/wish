// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class FileDialogLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(FileDialogLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "FileDialog"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "FileDialog"_key);
}

TEST_F(FileDialogLocalTest, DefaultTitleIsOpenFile) {
  auto obj = dynamic::instantiate("wish"_key, "FileDialog"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Open File");
}

TEST_F(FileDialogLocalTest, FilesFieldIsDynamicPtr) {
  auto obj = dynamic::instantiate("wish"_key, "FileDialog"_key);
  auto* f = obj.findField("files"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<dynamic_ptr>());
}

TEST_F(FileDialogLocalTest, DefaultFilenameIsEmpty) {
  auto obj = dynamic::instantiate("wish"_key, "FileDialog"_key);
  auto* f = obj.findField("filename"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "");
}

TEST_F(FileDialogLocalTest, FiltersFieldIsDynamicPtr) {
  auto obj = dynamic::instantiate("wish"_key, "FileDialog"_key);
  auto* f = obj.findField("filters"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<dynamic_ptr>());
}

TEST_F(FileDialogLocalTest, DefaultConfirmLabelIsOpen) {
  auto obj = dynamic::instantiate("wish"_key, "FileDialog"_key);
  auto* f = obj.findField("confirm_label"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Open");
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

// Helper: find the root key for the internal form tree (starts with "__form_",
// no dot — i.e. it is the top-level entry not a child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__form_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// ── Step 5: internal Window construction ─────────────────────────────────────

class FileDialogWindowTest : public ::testing::Test {
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

  // Instantiate the dialog and return the root key in session.objects.
  std::string instantiate_and_get_root() {
    client_->instantiate("wish"_key, "FileDialog"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->ui_objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(FileDialogWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __form_... root key in session.objects";
}

TEST_F(FileDialogWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<bison::key_t>(), "Window"_key);
}

TEST_F(FileDialogWindowTest, WindowHasVerticalLayoutChild) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_TRUE(objs.count(root + ".vbox"));
  EXPECT_EQ(objs.at(root + ".vbox")->findField(dynamic::CLASS)->as<bison::key_t>(), "VerticalLayout"_key);
}

TEST_F(FileDialogWindowTest, TreeContainsPathInput) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.path_input"));
}

TEST_F(FileDialogWindowTest, TreeContainsFileTable) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.file_table"));
}

TEST_F(FileDialogWindowTest, TreeContainsFilenameInput) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.filename_input"));
}

TEST_F(FileDialogWindowTest, TreeContainsBtnOpen) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.btn_row.btn_open"));
}

TEST_F(FileDialogWindowTest, TreeContainsBtnCancel) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root + ".vbox.btn_row.btn_cancel"));
}

TEST_F(FileDialogWindowTest, WindowTitleMatchesFormTitleField) {
  client_->instantiate("wish"_key, "FileDialog"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& win = srv_->last_session->ui_objects.at(root);
  EXPECT_EQ(win->findField("title"_key)->as<std::string>(), "Open File");
}

TEST_F(FileDialogWindowTest, BtnOpenLabelMatchesConfirmLabel) {
  client_->instantiate("wish"_key, "FileDialog"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->ui_objects;
  ASSERT_TRUE(objs.count(root + ".vbox.btn_row.btn_open"));
  EXPECT_EQ(objs.at(root + ".vbox.btn_row.btn_open")->findField("label"_key)->as<std::string>(), "Open");
}

// ── Step 6: file list synchronization ────────────────────────────────────────

// Helper: build a files dynamic_ptr like {0:{name,type}, 1:{name,type}, ...}
static dynamic make_files(std::initializer_list<std::pair<std::string, std::string>> entries) {
  dynamic files;
  size_t i = 0;
  for (auto& [name, type] : entries) {
    auto e = dynamic_ptr{bison::key_t{0U}, {}};
    (*e)["name"_key] = name;
    (*e)["type"_key] = type;
    files[i++] = e;
  }
  return files;
}

class FileDialogFilesTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "FileDialog"_key).get());
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

  // Set the form's files field from a files dynamic.
  void set_files(dynamic files_dyn) {
    dynamic params;
    params["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files_dyn))};
    proxy_->set(std::move(params)).get();
  }

  // Count the indexed (row) children of the internal Table widget.
  size_t table_row_count() const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.file_table");
    if (it == objs.end() || !it->second)
      return 0;
    auto* cf = it->second->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>())
      return 0;
    return cf->as<dynamic_ptr>()->size();
  }

  // Get the text of the Label at row[row_idx], column[col_idx] in the Table.
  //
  // Column 0 isn't a plain Label -- rebuild_file_table() (file_dialog.cpp)
  // wraps it in a HorizontalLayout ("icon_row") holding a type icon Image
  // at children[0] and the actual name Label at children[1], Windows-Explorer
  // style (see icon_for_entry()'s doc comment). Every other column is still
  // a direct Label. Unwrap one extra level whenever the cell itself has no
  // "text" field of its own, rather than hardcoding "column 0 means unwrap"
  // -- keeps this helper correct if a future column gains the same wrapping.
  std::string table_cell_text(size_t row_idx, size_t col_idx) const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.file_table");
    if (it == objs.end() || !it->second)
      return {};
    auto* cf = it->second->findField("children"_key);
    if (!cf || !cf->is<dynamic_ptr>())
      return {};
    auto& ch = *cf->as<dynamic_ptr>();

    const auto& row_f = ch.at(row_idx);
    if (!row_f.is<dynamic_ptr>())
      return {};
    auto& row = *row_f.as<dynamic_ptr>();

    auto* rcf = row.findField("children"_key);
    if (!rcf || !rcf->is<dynamic_ptr>())
      return {};
    const auto& cell_f = rcf->as<dynamic_ptr>()->at(col_idx);
    if (!cell_f.is<dynamic_ptr>())
      return {};
    auto& cell = *cell_f.as<dynamic_ptr>();

    if (auto* text_f = cell.findField("text"_key); text_f && text_f->is<std::string>())
      return text_f->as<std::string>();

    // Cell has no "text" of its own -- it's a wrapper (e.g. icon_row); the
    // name Label is its second child (children[1]).
    auto* wrapped_cf = cell.findField("children"_key);
    if (!wrapped_cf || !wrapped_cf->is<dynamic_ptr>())
      return {};
    const auto& inner_f = wrapped_cf->as<dynamic_ptr>()->at(size_t{1});
    if (!inner_f.is<dynamic_ptr>())
      return {};
    return inner_f.as<dynamic_ptr>()->as<std::string>("text"_key);
  }

  // Simulate a row_selected event on the internal Table.
  void simulate_row_selected(int32_t idx) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.file_table");
    ASSERT_NE(it, objs.end());
    auto table_id = (*it->second)["__wish_id"_key].as<bison::key_t>();

    dynamic payload;
    payload["index"_key] = idx;

    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(table_id, "row_selected"_key, std::move(payload));
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

TEST_F(FileDialogFilesTest, SetFilesBuildsTableRows) {
  set_files(make_files({{"a.txt", "file"}, {"docs", "dir"}}));
  EXPECT_EQ(table_row_count(), 2u);
}

TEST_F(FileDialogFilesTest, TableRowsHaveCorrectNames) {
  set_files(make_files({{"a.txt", "file"}, {"docs", "dir"}}));
  EXPECT_EQ(table_cell_text(0, 0), "a.txt");
  EXPECT_EQ(table_cell_text(1, 0), "docs");
}

TEST_F(FileDialogFilesTest, TableRowsHaveCorrectTypes) {
  set_files(make_files({{"a.txt", "file"}, {"docs", "dir"}}));
  EXPECT_EQ(table_cell_text(0, 1), "file");
  EXPECT_EQ(table_cell_text(1, 1), "dir");
}

TEST_F(FileDialogFilesTest, ClearFilesEmptiesTableRows) {
  set_files(make_files({{"a.txt", "file"}}));
  EXPECT_EQ(table_row_count(), 1u);
  set_files(make_files({}));
  EXPECT_EQ(table_row_count(), 0u);
}

TEST_F(FileDialogFilesTest, RowSelectedSetsFilenameToFirstEntry) {
  set_files(make_files({{"a.txt", "file"}, {"docs", "dir"}}));
  simulate_row_selected(0);
  auto snapshot = proxy_->get().get();
  std::string fn = snapshot.as<std::string>("filename"_key);
  EXPECT_EQ(fn, "a.txt");
}

TEST_F(FileDialogFilesTest, RowSelectedSetsFilenameToSecondEntry) {
  set_files(make_files({{"a.txt", "file"}, {"docs", "dir"}}));
  simulate_row_selected(1);
  auto snapshot = proxy_->get().get();
  std::string fn = snapshot.as<std::string>("filename"_key);
  EXPECT_EQ(fn, "docs");
}

TEST_F(FileDialogFilesTest, RowSelectedUpdatesFilenameInputWidget) {
  set_files(make_files({{"report.pdf", "file"}}));
  simulate_row_selected(0);
  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_ + ".vbox.filename_input");
  ASSERT_NE(it, objs.end());
  EXPECT_EQ(it->second->findField("value"_key)->as<std::string>(), "report.pdf");
}

// ── RMI fixture — checks server round-trips ───────────────────────────────────

class FileDialogRMITest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<wish::server>(transport_, std::make_unique<wish::null_renderer>());
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

  memory_server_transport transport_;
  std::unique_ptr<wish::server> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(FileDialogRMITest, InstantiateReturnsValidProxy) {
  auto proxy = client_->instantiate("wish"_key, "FileDialog"_key).get();
  EXPECT_TRUE(proxy.valid());
}

TEST_F(FileDialogRMITest, SetTitleRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "FileDialog"_key).get();
  dynamic params;
  params["title"_key] = std::string{"Choose a file"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "Choose a file");
}

TEST_F(FileDialogRMITest, SetFilenameRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "FileDialog"_key).get();
  dynamic params;
  params["filename"_key] = std::string{"report.pdf"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("filename"_key), "report.pdf");
}

TEST_F(FileDialogRMITest, SetConfirmLabelRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "FileDialog"_key).get();
  dynamic params;
  params["confirm_label"_key] = std::string{"Select"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("confirm_label"_key), "Select");
}

// ── Step 7: high-level event emission ────────────────────────────────────────

// Records form-level events (on_open, on_cancel, on_navigate) emitted by the
// form through sess().emit_event, without interfering with internal routing.
struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class FileDialogEventsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "FileDialog"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());

    // Wrap emit_event to capture high-level events emitted by form::emit().
    auto prev = std::move(srv_->last_session->emit_event);
    events_ = std::make_shared<std::vector<CapturedEvent>>();
    auto evts = events_;
    srv_->last_session->emit_event = [prev, evts](bison::key_t id, bison::key_t event, dynamic payload) {
      if (event == "on_open"_key || event == "on_cancel"_key || event == "on_navigate"_key)
        evts->push_back({event, payload});
      if (prev)
        prev(id, event, std::move(payload));
    };
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  void set_files(dynamic files_dyn) {
    dynamic params;
    params["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files_dyn))};
    proxy_->set(std::move(params)).get();
  }

  void set_filename(const std::string& name) {
    dynamic params;
    params["filename"_key] = name;
    proxy_->set(std::move(params)).get();
  }

  void simulate_btn_click(const std::string& btn_key) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.btn_row." + btn_key);
    ASSERT_NE(it, objs.end()) << "button not found: " << btn_key;
    auto btn_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(btn_id, "clicked"_key, dynamic{});
  }

  void simulate_row_activated(int32_t idx) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.file_table");
    ASSERT_NE(it, objs.end());
    auto table_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    dynamic payload;
    payload["index"_key] = idx;
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(table_id, "row_activated"_key, std::move(payload));
  }

  // Simulate the user typing a path and pressing Enter in the path_input.
  void simulate_path_input_changed(const std::string& path) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.path_input");
    ASSERT_NE(it, objs.end()) << "path_input not found";
    auto input_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    dynamic payload;
    payload["value"_key] = path;
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(input_id, "changed"_key, std::move(payload));
  }

  bool has_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name.id == name.id)
        return true;
    return false;
  }

  // form::emit() defers delivery to the render loop's next frame (see
  // session.hpp's contract on emit_event), so a form-level event is not
  // necessarily captured yet the instant simulate_*() returns. Spin briefly
  // for it, same idiom as test_integration.cpp's event round-trip test.
  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto t0 = std::chrono::steady_clock::now();
    while (!has_event(name) && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has_event(name);
  }

  const CapturedEvent* find_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name.id == name.id)
        return &e;
    return nullptr;
  }


  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(FileDialogEventsTest, BtnOpenEmitsOnOpen) {
  set_files(make_files({{"report.pdf", "file"}}));
  set_filename("report.pdf");
  simulate_btn_click("btn_open");
  ASSERT_TRUE(wait_for_event("on_open"_key));
  auto* ev = find_event("on_open"_key);
  EXPECT_EQ(ev->payload.as<std::string>("path"_key), "report.pdf");
}

TEST_F(FileDialogEventsTest, BtnCancelEmitsOnCancelAndRemovesWindow) {
  simulate_btn_click("btn_cancel");
  EXPECT_TRUE(wait_for_event("on_cancel"_key));

  // request_close() (file_dialog.cpp) only sets the hidden __request_close__
  // flag; the internal Window is actually torn down later, once the real
  // ImGui render loop notices the popup closed and fires the Window's own
  // "closed" event back through on_event() (see request_close()'s doc
  // comment -- mirrors message_box.cpp's identical handshake). This
  // fixture's wish::null_renderer has a no-op render_node() (renderer.hpp),
  // so that flag is never processed here -- simulate the render loop's half
  // of the handshake directly instead, same idiom as
  // test_file_explorer.cpp's WindowClosedEmitsClosedAndCleansUp.
  auto& objs = srv_->last_session->ui_objects;
  auto it = objs.find(root_);
  ASSERT_NE(it, objs.end());
  auto window_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
  auto h = srv_->last_session->top_level_handlers.find(root_);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(window_id, "closed"_key, dynamic{});

  // Internal Window must now be removed from session.objects.
  EXPECT_TRUE(find_form_root(srv_->last_session->ui_objects).empty());
}

TEST_F(FileDialogEventsTest, RowActivatedDirEmitsOnNavigate) {
  set_files(make_files({{"docs", "dir"}, {"readme.txt", "file"}}));
  simulate_row_activated(0);
  ASSERT_TRUE(wait_for_event("on_navigate"_key));
  auto* ev = find_event("on_navigate"_key);
  EXPECT_EQ(ev->payload.as<std::string>("name"_key), "docs");
  EXPECT_EQ(ev->payload.as<std::string>("type"_key), "dir");
}

TEST_F(FileDialogEventsTest, RowActivatedFileEmitsOnOpen) {
  set_files(make_files({{"docs", "dir"}, {"readme.txt", "file"}}));
  simulate_row_activated(1);
  ASSERT_TRUE(wait_for_event("on_open"_key));
  auto* ev = find_event("on_open"_key);
  EXPECT_EQ(ev->payload.as<std::string>("path"_key), "readme.txt");
}

TEST_F(FileDialogEventsTest, DotDotFilenameDoesNotEmitOnOpen) {
  set_filename("../escape.txt");
  simulate_btn_click("btn_open");
  EXPECT_FALSE(has_event("on_open"_key));
}

TEST_F(FileDialogEventsTest, AbsolutePathRejectedWhenNotAllowed) {
  // allow_absolute_paths defaults to false on this server instance.
  set_filename("/etc/passwd");
  simulate_btn_click("btn_open");
  EXPECT_FALSE(has_event("on_open"_key));
}

TEST_F(FileDialogEventsTest, PathInputChangedEmitsOnNavigateWithTypePath) {
  simulate_path_input_changed("/home/user/docs");
  ASSERT_TRUE(wait_for_event("on_navigate"_key));
  auto* ev = find_event("on_navigate"_key);
  EXPECT_EQ(ev->payload.as<std::string>("name"_key), "/home/user/docs");
  EXPECT_EQ(ev->payload.as<std::string>("type"_key), "path");
}

// ── Step 8: filename two-way binding and filters Combo ───────────────────────

class FileDialogBindingTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "FileDialog"_key).get());
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

  // Simulate a "changed" event on the filename_input widget.
  void simulate_filename_input_changed(const std::string& value) {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.filename_input");
    ASSERT_NE(it, objs.end()) << "filename_input not found in session.objects";
    auto input_id = (*it->second)["__wish_id"_key].as<bison::key_t>();
    dynamic payload;
    payload["value"_key] = value;
    auto h = srv_->last_session->top_level_handlers.find(root_);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(input_id, "changed"_key, std::move(payload));
  }

  // Set the filters field via the client proxy.
  void set_filters(dynamic filters_dyn) {
    dynamic params;
    params["filters"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(filters_dyn))};
    proxy_->set(std::move(params)).get();
  }

  // Set the confirm_label field via the client proxy.
  void set_confirm_label(const std::string& label) {
    dynamic params;
    params["confirm_label"_key] = label;
    proxy_->set(std::move(params)).get();
  }

  // Read the visible field of filter_row from session.objects.
  bool filter_row_visible() const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.filter_row");
    if (it == objs.end() || !it->second)
      return false;
    auto* f = it->second->findField("visible"_key);
    if (!f)
      return false;
    return f->as<bool>();
  }

  // Read the items string of filter_combo from session.objects.
  std::string filter_combo_items() const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.filter_row.filter_combo");
    if (it == objs.end() || !it->second)
      return {};
    auto* f = it->second->findField("items"_key);
    if (!f || !f->is<std::string>())
      return {};
    return f->as<std::string>();
  }

  // Read btn_open's label field from session.objects.
  std::string btn_open_label() const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.btn_row.btn_open");
    if (it == objs.end() || !it->second)
      return {};
    auto* f = it->second->findField("label"_key);
    if (!f || !f->is<std::string>())
      return {};
    return f->as<std::string>();
  }

  // Read path_input value from session.objects.
  std::string path_input_value() const {
    auto& objs = srv_->last_session->ui_objects;
    auto it = objs.find(root_ + ".vbox.path_input");
    if (it == objs.end() || !it->second)
      return {};
    auto* f = it->second->findField("value"_key);
    if (!f || !f->is<std::string>())
      return {};
    return f->as<std::string>();
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

TEST_F(FileDialogBindingTest, FilenameInputChangedUpdatesFormFilenameField) {
  simulate_filename_input_changed("foo.txt");
  auto snapshot = proxy_->get().get();
  EXPECT_EQ(snapshot.as<std::string>("filename"_key), "foo.txt");
}

TEST_F(FileDialogBindingTest, SetPathUpdatesPathInputWidget) {
  dynamic params;
  params["path"_key] = std::string{"/home/user"};
  proxy_->set(std::move(params)).get();
  EXPECT_EQ(path_input_value(), "/home/user");
}

// Build a filter entry dynamic with a label and optional regex.
static dynamic_ptr make_filter(const std::string& label, const std::string& regex = "") {
  auto e = dynamic_ptr{std::make_shared<dynamic>()};
  (*e)["label"_key] = label;
  (*e)["regex"_key] = regex;
  return e;
}

TEST_F(FileDialogBindingTest, SetFiltersMakesFilterRowVisible) {
  dynamic filters;
  filters[size_t{0}] = make_filter("Text Files (*.txt)", "\\.txt$");
  filters[size_t{1}] = make_filter("Markdown (*.md)", "\\.md$");
  set_filters(std::move(filters));
  EXPECT_TRUE(filter_row_visible());
}

TEST_F(FileDialogBindingTest, SetFiltersPopulatesComboItems) {
  dynamic filters;
  filters[size_t{0}] = make_filter("Text Files (*.txt)", "\\.txt$");
  filters[size_t{1}] = make_filter("Markdown (*.md)", "\\.md$");
  set_filters(std::move(filters));
  EXPECT_EQ(filter_combo_items(), "Text Files (*.txt)\nMarkdown (*.md)");
}

TEST_F(FileDialogBindingTest, ClearFiltersHidesFilterRow) {
  // First make it visible.
  dynamic filters;
  filters[size_t{0}] = make_filter("Text Files (*.txt)", "\\.txt$");
  set_filters(std::move(filters));
  ASSERT_TRUE(filter_row_visible());

  // Clearing should hide it.
  set_filters(dynamic{});
  EXPECT_FALSE(filter_row_visible());
}

TEST_F(FileDialogBindingTest, SetConfirmLabelUpdatesBtnOpenLabel) {
  set_confirm_label("Select");
  EXPECT_EQ(btn_open_label(), "Select");
}
