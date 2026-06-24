// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/registry.hpp>
#include <wish/server.hpp>
#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

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
