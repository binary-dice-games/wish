// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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

dynamic update_args(const std::string& command, float phase = 0.0f, const std::vector<std::string>& lines = {}) {
  dynamic arr;
  size_t i = 0;
  for (auto& line : lines)
    arr[i++] = line;
  dynamic a;
  a["command"_key] = command;
  a["phase"_key] = phase;
  a["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return a;
}

dynamic finish_args(const std::string& error = {}) {
  dynamic a;
  a["error"_key] = error;
  return a;
}

} // namespace

// ── Local (non-RMI) fixture ─────────────────────────────────────────────────

class ProgressBoxLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(ProgressBoxLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "ProgressBox"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "ProgressBox"_key);
}

// ── Session-capturing server ───────────────────────────────────────────────

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};
  /// Every event the session emits (see session_event_recorder.hpp).
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

static std::string find_root_with_prefix(const wish::name_map& objects, const std::string& prefix) {
  for (const auto& [k, _] : objects)
    if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
      return k;
  return {};
}

class ProgressBoxRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "ProgressBox"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_root_with_prefix(srv_->last_session->ui_objects, "__progress_box_");
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

  // file_path of the TextEditor at @p path ("" if missing/unset).
  std::string editor_file(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    auto* f = it->second->findField<std::string>("file_path"_key);
    return f ? *f : std::string{};
  }

  // Contents of the sandbox file the TextEditor at @p path displays.
  std::string editor_text(const std::string& path) const {
    auto rel = editor_file(path);
    if (rel.empty())
      return {};
    std::ifstream in(srv_->last_session->resource_dir / rel, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
  }

  size_t row_count(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  static dynamic_ptr nth_child(const dynamic_ptr& parent, size_t index) {
    if (!parent)
      return nullptr;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    auto& f = (*cf)->at(index);
    return f.is<dynamic_ptr>() ? f.as<dynamic_ptr>() : nullptr;
  }

  dynamic_ptr row_in(const std::string& table_path, size_t index) const {
    auto it = srv_->last_session->ui_objects.find(table_path);
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    return nth_child(it->second, index);
  }

  // __wish_id of a row's MenuItem, found by label (robust to the
  // state-dependent item ordering / separator slots).
  bison::key_t menu_id_in(const std::string& table_path, size_t row, const std::string& label) const {
    auto row_ptr = row_in(table_path, row);
    if (!row_ptr)
      return {};
    auto* rcf = row_ptr->findField<dynamic_ptr>("children"_key);
    if (!rcf || !*rcf)
      return {};
    size_t last = (*rcf)->size() - 1; // last cell is the MenuButton
    auto menu = (*rcf)->at(last).as<dynamic_ptr>();
    if (!menu)
      return {};
    auto* mcf = menu->findField<dynamic_ptr>("children"_key);
    bison::key_t found{};
    if (mcf && *mcf)
      (*mcf)->forEach([&](bison::key_t, const field& f) {
        if (found.id || !f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
          return;
        auto* lf = f.as<dynamic_ptr>()->findField<std::string>("label"_key);
        if (lf && *lf == label)
          found = f.as<dynamic_ptr>()->as<bison::key_t>("__wish_id"_key);
      });
    return found;
  }

  bool row_visible(const std::string& table_path, size_t row) const {
    auto r = row_in(table_path, row);
    return r ? r->as<bool>("visible"_key) : false;
  }

  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }
  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }

  bison::key_t id_at(const std::string& abs_path) const {
    auto it = srv_->last_session->ui_objects.find(abs_path);
    return it == srv_->last_session->ui_objects.end() ? bison::key_t{}
                                                      : it->second->as<bison::key_t>("__wish_id"_key);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};


#define BOX_TEXT_AT(path) srv_->last_session->ui_objects.at(path)->as<std::string>("text"_key)

TEST_F(ProgressBoxRmiTest, OpensAsAModalAndCollectsOutput) {
  auto& objects = srv_->last_session->ui_objects;
  ASSERT_TRUE(objects.count(root_));
  EXPECT_TRUE(objects.at(root_)->as<bool>("modal"_key));
  EXPECT_EQ(objects.at(root_)->as<std::string>("title"_key), "Working");
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_}));

  call("update"_key, update_args("pip install requests", 1.0f, {"Collecting requests"}));
  EXPECT_EQ(BOX_TEXT_AT(root_ + ".vbox.command"), "pip install requests");
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 2u) << "the command line + one output line";
  const float first = objects.at(root_ + ".vbox.bar")->as<float>("value"_key);
  EXPECT_LT(first, 0.0f) << "negative = indeterminate";

  call("update"_key, update_args("pip install requests", 2.0f, {"Downloading requests", "Installing"}));
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 4u) << "same command: no second header row";
  EXPECT_LT(objects.at(root_ + ".vbox.bar")->as<float>("value"_key), first) << "the phase animates the bar";
  call("update"_key, update_args("pip list", 0.1f));
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 5u) << "a new command adds its header row";
}

TEST_F(ProgressBoxRmiTest, TitleCanBeSetAtInstantiation) {
  dynamic params;
  params["title"_key] = std::string{"Running helm"};
  auto other = client_->instantiate("wish"_key, "ProgressBox"_key, std::move(params)).get();
  bool found = false;
  for (const auto& [k, e] : srv_->last_session->ui_objects)
    if (k.rfind("__progress_box_", 0) == 0 && k.find('.') == std::string::npos && k != root_)
      found = e->as<std::string>("title"_key) == "Running helm";
  EXPECT_TRUE(found);
}

TEST_F(ProgressBoxRmiTest, FinishWithoutErrorClosesAndEmitsClosed) {
  auto& objects = srv_->last_session->ui_objects;
  call("update"_key, update_args("git fetch", 1.0f));
  call("finish"_key, finish_args());
  ASSERT_TRUE(objects.count(root_)) << "torn down only once the renderer confirms the close";
  EXPECT_TRUE(objects.at(root_)->as<bool>("__request_close__"_key));

  size_t since = srv_->events->mark();
  fire(id_at(root_), "closed"_key);
  EXPECT_FALSE(objects.count(root_));
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since));

  // The same instance serves the next operation.
  call("update"_key, update_args("git pull", 0.5f, {"Already up to date."}));
  const std::string again = find_root_with_prefix(objects, "__progress_box_");
  ASSERT_FALSE(again.empty());
  EXPECT_EQ(BOX_TEXT_AT(again + ".vbox.command"), "git pull");
  EXPECT_EQ(row_count(again + ".vbox.table"), 2u);
}

TEST_F(ProgressBoxRmiTest, FinishWithErrorStaysOpenUntilClosed) {
  auto& objects = srv_->last_session->ui_objects;
  call("update"_key, update_args("pip install nope", 1.0f, {"ERROR: No matching distribution"}));
  call("finish"_key, finish_args("install nope failed: ERROR: No matching distribution found for nope"));

  ASSERT_TRUE(objects.count(root_));
  EXPECT_FALSE(objects.at(root_)->findField<bool>("__request_close__"_key)) << "left open to show the error";
  EXPECT_NE(BOX_TEXT_AT(root_ + ".vbox.result").find("No matching distribution"), std::string::npos);
  EXPECT_TRUE(objects.at(root_ + ".vbox.result")->as<bool>("visible"_key));
  EXPECT_FALSE(objects.at(root_ + ".vbox.bar")->as<bool>("visible"_key));
  EXPECT_EQ(objects.at(root_ + ".vbox.btn_cancel")->as<std::string>("label"_key), "Close");
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 2u) << "the output that led to it is kept";

  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.btn_cancel"), "clicked"_key);
  EXPECT_TRUE(objects.at(root_)->as<bool>("__request_close__"_key));
  EXPECT_FALSE(srv_->events->wait_for("cancel_requested"_key, since, nullptr, std::chrono::milliseconds{100}))
      << "Close is not Cancel";
}

TEST_F(ProgressBoxRmiTest, CancelEmitsOnceAndClosesEvenIfTheOperationFailed) {
  auto& objects = srv_->last_session->ui_objects;
  call("update"_key, update_args("docker pull big", 1.0f));

  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.btn_cancel"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("cancel_requested"_key, since));
  EXPECT_EQ(BOX_TEXT_AT(root_ + ".vbox.command"), "Cancelling ...");

  since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.btn_cancel"), "clicked"_key);
  EXPECT_FALSE(srv_->events->wait_for("cancel_requested"_key, since, nullptr, std::chrono::milliseconds{100}));

  call("finish"_key, finish_args("pull failed: cancelled"));
  EXPECT_TRUE(objects.at(root_)->as<bool>("__request_close__"_key));
}

TEST_F(ProgressBoxRmiTest, ReopensWhenAnUpdateArrivesWhileClosing) {
  auto& objects = srv_->last_session->ui_objects;
  call("update"_key, update_args("helm install a", 1.0f));
  call("finish"_key, finish_args());
  call("update"_key, update_args("helm install b", 0.5f)); // before the renderer closed it

  size_t since = srv_->events->mark();
  fire(id_at(root_), "closed"_key);
  const std::string again = find_root_with_prefix(objects, "__progress_box_");
  ASSERT_FALSE(again.empty()) << "rebuilt for the new operation";
  EXPECT_FALSE(objects.at(again)->findField<bool>("__request_close__"_key));
  EXPECT_FALSE(srv_->events->wait_for("closed"_key, since, nullptr, std::chrono::milliseconds{100}))
      << "it never went away from the user's point of view";
  call("update"_key, update_args("helm install b", 0.7f));
  EXPECT_EQ(BOX_TEXT_AT(again + ".vbox.command"), "helm install b");
}

TEST_F(ProgressBoxRmiTest, LogIsCappedToTheNewestLines) {
  std::vector<std::string> lines;
  for (int i = 0; i < 320; ++i)
    lines.push_back("line " + std::to_string(i));
  call("update"_key, update_args("noisy", 1.0f, lines));
  // row_count() is "highest index + 1"; count the rows that are really there.
  size_t rows = 0;
  std::string first;
  auto* cf = srv_->last_session->ui_objects.at(root_ + ".vbox.table")->findField<dynamic_ptr>("children"_key);
  ASSERT_TRUE(cf && *cf);
  (*cf)->forEach([&](bison::key_t, const field& f) {
    if (!f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
      return;
    auto row = f.as<dynamic_ptr>();
    if (row->as<bison::key_t>(dynamic::CLASS) != "TableRow"_key)
      return;
    if (rows++ == 0)
      first = nth_child(row, 0)->as<std::string>("text"_key);
  });
  EXPECT_EQ(rows, 300u);
  EXPECT_EQ(first, "line 20") << "the oldest rows (the header and lines 0-19) were dropped";
}
