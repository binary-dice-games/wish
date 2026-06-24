// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/registry.hpp>
#include <wish/server.hpp>
#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <mutex>
#include <string>

using namespace bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class OpenFileDialogLocalTest : public ::testing::Test {
 protected:
  void SetUp() override { bdg::wish::register_all(); }
};

TEST_F(OpenFileDialogLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "OpenFileDialog"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<key_t>(), "OpenFileDialog"_key);
}

TEST_F(OpenFileDialogLocalTest, DefaultTitleIsOpenFile) {
  auto obj = dynamic::instantiate("wish"_key, "OpenFileDialog"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Open File");
}

TEST_F(OpenFileDialogLocalTest, FilesFieldIsDynamicPtr) {
  auto obj = dynamic::instantiate("wish"_key, "OpenFileDialog"_key);
  auto* f = obj.findField("files"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<dynamic_ptr>());
}

TEST_F(OpenFileDialogLocalTest, DefaultFilenameIsEmpty) {
  auto obj = dynamic::instantiate("wish"_key, "OpenFileDialog"_key);
  auto* f = obj.findField("filename"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "");
}

TEST_F(OpenFileDialogLocalTest, FiltersFieldIsDynamicPtr) {
  auto obj = dynamic::instantiate("wish"_key, "OpenFileDialog"_key);
  auto* f = obj.findField("filters"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<dynamic_ptr>());
}

TEST_F(OpenFileDialogLocalTest, DefaultConfirmLabelIsOpen) {
  auto obj = dynamic::instantiate("wish"_key, "OpenFileDialog"_key);
  auto* f = obj.findField("confirm_label"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
  EXPECT_EQ(f->as<std::string>(), "Open");
}

// ── Session-capturing server for internal-tree tests ──────────────────────────

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t,
                          std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}

  wish::session* last_session{nullptr};

 protected:
  void on_session_created(wish::session& s) override { last_session = &s; }
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

class OpenFileDialogWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(
        transport_, std::make_unique<wish::null_renderer>());
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
    client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
    EXPECT_NE(srv_->last_session, nullptr);
    return find_form_root(srv_->last_session->objects);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
};

TEST_F(OpenFileDialogWindowTest, SessionObjectsHasFormRoot) {
  std::string root = instantiate_and_get_root();
  EXPECT_FALSE(root.empty()) << "No __form_... root key in session.objects";
}

TEST_F(OpenFileDialogWindowTest, FormRootIsWindow) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& obj = srv_->last_session->objects.at(root);
  EXPECT_EQ(obj->findField(dynamic::CLASS)->as<key_t>(), "Window"_key);
}

TEST_F(OpenFileDialogWindowTest, WindowHasVerticalLayoutChild) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->objects;
  ASSERT_TRUE(objs.count(root + ".vbox"));
  EXPECT_EQ(objs.at(root + ".vbox")->findField(dynamic::CLASS)->as<key_t>(),
            "VerticalLayout"_key);
}

TEST_F(OpenFileDialogWindowTest, TreeContainsFileTable) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.file_table"));
}

TEST_F(OpenFileDialogWindowTest, TreeContainsFilenameInput) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->objects.count(
      root + ".vbox.filename_row.filename_input"));
}

TEST_F(OpenFileDialogWindowTest, TreeContainsBtnOpen) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.btn_row.btn_open"));
}

TEST_F(OpenFileDialogWindowTest, TreeContainsBtnCancel) {
  std::string root = instantiate_and_get_root();
  ASSERT_FALSE(root.empty());
  EXPECT_TRUE(srv_->last_session->objects.count(root + ".vbox.btn_row.btn_cancel"));
}

TEST_F(OpenFileDialogWindowTest, WindowTitleMatchesFormTitleField) {
  client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
  std::string root = find_form_root(srv_->last_session->objects);
  ASSERT_FALSE(root.empty());
  auto& win = srv_->last_session->objects.at(root);
  EXPECT_EQ(win->findField("title"_key)->as<std::string>(), "Open File");
}

TEST_F(OpenFileDialogWindowTest, BtnOpenLabelMatchesConfirmLabel) {
  client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
  std::string root = find_form_root(srv_->last_session->objects);
  ASSERT_FALSE(root.empty());
  auto& objs = srv_->last_session->objects;
  ASSERT_TRUE(objs.count(root + ".vbox.btn_row.btn_open"));
  EXPECT_EQ(objs.at(root + ".vbox.btn_row.btn_open")->findField("label"_key)
                ->as<std::string>(),
            "Open");
}

// ── RMI fixture — checks server round-trips ───────────────────────────────────

class OpenFileDialogRMITest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<wish::server>(
        transport_, std::make_unique<wish::null_renderer>());
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

TEST_F(OpenFileDialogRMITest, InstantiateReturnsValidProxy) {
  auto proxy = client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
  EXPECT_TRUE(proxy.valid());
}

TEST_F(OpenFileDialogRMITest, SetTitleRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
  dynamic params;
  params["title"_key] = std::string{"Choose a file"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "Choose a file");
}

TEST_F(OpenFileDialogRMITest, SetFilenameRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
  dynamic params;
  params["filename"_key] = std::string{"report.pdf"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("filename"_key), "report.pdf");
}

TEST_F(OpenFileDialogRMITest, SetConfirmLabelRoundTrips) {
  auto proxy = client_->instantiate("wish"_key, "OpenFileDialog"_key).get();
  dynamic params;
  params["confirm_label"_key] = std::string{"Select"};
  proxy.set(std::move(params)).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("confirm_label"_key), "Select");
}
