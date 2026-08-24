// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// ── A small reflectable class used as the dialog's target ───────────────────

void ensure_target_class_registered() {
  static bool done = false;
  if (done)
    return;
  done = true;

  auto proto = dynamic_ptr{"__PropertiesDialogTarget"_key, {}};
  proto->addField("enabled"_key, field{false, attr<DisplayName>("Enabled"), attr<Description>("On/off.")});
  proto->addField("name"_key, field{std::string{"widget"}, attr<DisplayName>("Name")});
  // Global namespace (0U), matching the single-arg dynamic::instantiate()
  // overload this file's tests use to create target instances.
  dynamic::addClass(bison::key_t{0U}, std::move(proto));
}

} // namespace

// ── Local (non-RMI) fixture — checks prototype defaults ───────────────────────

class PropertiesDialogLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(PropertiesDialogLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "PropertiesDialog"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "PropertiesDialog"_key);
}

TEST_F(PropertiesDialogLocalTest, DefaultReadOnlyIsTrue) {
  // Preserves this form's original always-read-only behavior for every
  // caller that never sets read_only explicitly (e.g. top/mc's own
  // process/file properties dialogs).
  auto obj = dynamic::instantiate("wish"_key, "PropertiesDialog"_key);
  auto* f = obj.findField("read_only"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->as<bool>());
}

TEST_F(PropertiesDialogLocalTest, DefaultShowDescriptionPanelIsFalse) {
  auto obj = dynamic::instantiate("wish"_key, "PropertiesDialog"_key);
  auto* f = obj.findField("show_description_panel"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(f->as<bool>());
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
// "__properties_dialog_", no dot — i.e. it is the top-level entry not a
// child path).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__properties_dialog_", 0) == 0 && k.find('.') == std::string::npos)
      return k;
  }
  return {};
}

// Helper: find the value-cell widget stamped by the internal ObjectInspector
// for the field whose DisplayName is @p display_name -- mirrors
// test_object_inspector.cpp's own find_value_widget() (same synthetic
// ".name"/".value" path convention, see object_inspector.cpp's stamp()).
static wish::ui_element_ptr find_value_widget(const wish::name_map& objects, const std::string& display_name) {
  for (auto& [path, elem] : objects) {
    if (path.size() < 5 || path.substr(path.size() - 5) != ".name")
      continue;
    auto* text = elem->findField<std::string>("text"_key);
    if (text && *text == display_name) {
      std::string value_path = path.substr(0, path.size() - 5) + ".value";
      auto it = objects.find(value_path);
      if (it != objects.end())
        return it->second;
    }
  }
  return {};
}

static bool has_description_panel(const wish::name_map& objects) {
  for (auto& [path, elem] : objects) {
    if (path.size() >= 12 && path.substr(path.size() - 12) == ".description")
      return true;
  }
  return false;
}

class PropertiesDialogWindowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensure_target_class_registered();
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bison::rmi::client>(transport_.connect());
    client_->connect();
  }

  void TearDown() override {
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bison::rmi::client> client_;
};

TEST_F(PropertiesDialogWindowTest, SessionObjectsHasFormRoot) {
  client_->instantiate("wish"_key, "PropertiesDialog"_key).get();
  std::string root = find_form_root(srv_->last_session->ui_objects);
  EXPECT_FALSE(root.empty()) << "No __properties_dialog_... root key in session.objects";
}

// ── read_only / show_description_panel configurability ───────────────────────

TEST_F(PropertiesDialogWindowTest, DefaultReadOnlyRendersLabelValueWidget) {
  auto target = dynamic_ptr{dynamic::instantiate("__PropertiesDialogTarget"_key)};
  dynamic params;
  params["target"_key] = target;
  client_->instantiate("wish"_key, "PropertiesDialog"_key, std::move(params)).get();

  auto w = find_value_widget(srv_->last_session->ui_objects, "Enabled");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "Label"_key);
}

TEST_F(PropertiesDialogWindowTest, ReadOnlyFalseRendersEditableValueWidget) {
  auto target = dynamic_ptr{dynamic::instantiate("__PropertiesDialogTarget"_key)};
  dynamic params;
  params["target"_key] = target;
  params["read_only"_key] = false;
  client_->instantiate("wish"_key, "PropertiesDialog"_key, std::move(params)).get();

  auto w = find_value_widget(srv_->last_session->ui_objects, "Enabled");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "Checkbox"_key);
}

TEST_F(PropertiesDialogWindowTest, DefaultShowDescriptionPanelOmitsPanel) {
  auto target = dynamic_ptr{dynamic::instantiate("__PropertiesDialogTarget"_key)};
  dynamic params;
  params["target"_key] = target;
  client_->instantiate("wish"_key, "PropertiesDialog"_key, std::move(params)).get();

  EXPECT_FALSE(has_description_panel(srv_->last_session->ui_objects));
}

TEST_F(PropertiesDialogWindowTest, ShowDescriptionPanelTrueAddsPanel) {
  auto target = dynamic_ptr{dynamic::instantiate("__PropertiesDialogTarget"_key)};
  dynamic params;
  params["target"_key] = target;
  params["show_description_panel"_key] = true;
  client_->instantiate("wish"_key, "PropertiesDialog"_key, std::move(params)).get();

  EXPECT_TRUE(has_description_panel(srv_->last_session->ui_objects));
}

// ── Editable target: edits commit onto target_, close reports it back ───────

struct CapturedEvent {
  bison::key_t name;
  dynamic payload;
};

class PropertiesDialogEditTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensure_target_class_registered();
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bison::rmi::client>(transport_.connect());
    client_->connect();

    dynamic params;
    params["target"_key] = dynamic_ptr{dynamic::instantiate("__PropertiesDialogTarget"_key)};
    params["read_only"_key] = false;
    proxy_.emplace(client_->instantiate("wish"_key, "PropertiesDialog"_key, std::move(params)).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);

    // Wrap emit_event to capture high-level events emitted by form::emit().
    auto prev = std::move(srv_->last_session->emit_event);
    events_ = std::make_shared<std::vector<CapturedEvent>>();
    auto evts = events_;
    srv_->last_session->emit_event = [prev, evts](bison::key_t id, bison::key_t event, dynamic payload) {
      if (event == "on_result"_key)
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

  bool wait_for_event(bison::key_t name, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
    auto has = [&] {
      for (auto& e : *events_)
        if (e.name.id == name.id)
          return true;
      return false;
    };
    auto t0 = std::chrono::steady_clock::now();
    while (!has() && std::chrono::steady_clock::now() - t0 < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return has();
  }

  const CapturedEvent* find_event(bison::key_t name) const {
    for (auto& e : *events_)
      if (e.name.id == name.id)
        return &e;
    return nullptr;
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bison::rmi::client> client_;
  std::optional<bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
  std::shared_ptr<std::vector<CapturedEvent>> events_;
};

TEST_F(PropertiesDialogEditTest, ChangingCheckboxCommitsOntoTarget) {
  ASSERT_FALSE(root_.empty());
  auto w = find_value_widget(srv_->last_session->ui_objects, "Enabled");
  ASSERT_TRUE(w);
  auto widget_id = w->as<bison::key_t>("__wish_id"_key);

  dynamic payload; // Checkbox's own "changed" contract: {value: bool}
  payload["value"_key] = true;
  auto h = srv_->last_session->top_level_handlers.find(root_);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(widget_id, "changed"_key, payload);

  // The edit is committed onto the form's own internal target_ member, which
  // shares identity with its own "target" field (see set_target()'s doc
  // comment) but not with any dynamic_ptr the test constructed locally --
  // instantiate()'s construct params cross the RMI dispatch boundary, which
  // clones the payload (see server::handle_instantiate()). Re-fetching the
  // field through the proxy observes the same server-side object the edit
  // was actually applied to.
  auto snapshot = proxy_->get().get();
  auto returned_target = snapshot.as<dynamic_ptr>("target"_key);
  ASSERT_TRUE(returned_target);
  auto* f = returned_target->findField<bool>("enabled"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(*f);
}

TEST_F(PropertiesDialogEditTest, ClosingAfterEditEmitsOnResultWithUpdatedTarget) {
  ASSERT_FALSE(root_.empty());
  auto w = find_value_widget(srv_->last_session->ui_objects, "Enabled");
  ASSERT_TRUE(w);
  auto widget_id = w->as<bison::key_t>("__wish_id"_key);

  dynamic payload;
  payload["value"_key] = true;
  auto h = srv_->last_session->top_level_handlers.find(root_);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(widget_id, "changed"_key, payload);

  // Simulate the Window's own "closed" event -- render_window() fires this
  // once ImGui confirms the modal actually closed (see on_event()'s doc
  // comment), the same event that follows either a Close-button click or
  // the window's own title-bar X.
  auto& objs = srv_->last_session->ui_objects;
  auto win_id = objs.at(root_)->as<bison::key_t>("__wish_id"_key);
  h->second->on_event(win_id, "closed"_key, dynamic{});

  ASSERT_TRUE(wait_for_event("on_result"_key));
  auto* ev = find_event("on_result"_key);
  ASSERT_NE(ev, nullptr);
  auto returned_target = ev->payload.as<dynamic_ptr>("target"_key);
  ASSERT_TRUE(returned_target);
  auto* f = returned_target->findField<bool>("enabled"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(*f);
}

TEST_F(PropertiesDialogEditTest, ClosingRemovesInternalWindowRoot) {
  auto& objs = srv_->last_session->ui_objects;
  auto win_id = objs.at(root_)->as<bison::key_t>("__wish_id"_key);
  auto h = srv_->last_session->top_level_handlers.find(root_);
  ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
  h->second->on_event(win_id, "closed"_key, dynamic{});

  EXPECT_TRUE(find_form_root(srv_->last_session->ui_objects).empty());
}
