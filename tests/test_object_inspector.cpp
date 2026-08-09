// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_elements/object_inspector.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <string>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// ── A small reflectable class used as the inspector's target ────────────────

const Enum::table& direction_enum() {
  static const Enum::table t{{"North", 0}, {"East", 1}, {"South", 2}};
  return t;
}

const EnumFlags::table& perms_flags() {
  static const EnumFlags::table t{{"Read", 1 << 0}, {"Write", 1 << 1}};
  return t;
}

void ensure_target_class_registered() {
  static bool done = false;
  if (done)
    return;
  done = true;

  auto proto = dynamic_ptr{"__InspectorTarget"_key, {}};
  proto->addField("active"_key, field{false, attr<DisplayName>("Active"), attr<Description>("On/off.")});
  proto->addField(
      "score"_key, field{int32_t{0}, attr<DisplayName>("Score"), attr<Range>(0, 10), attr<Description>("0-10.")});
  proto->addField("direction"_key, field{int32_t{0}, attr<DisplayName>("Direction"), attr<Enum>(direction_enum())});
  proto->addField("perms"_key, field{int32_t{0}, attr<DisplayName>("Perms"), attr<EnumFlags>(perms_flags())});
  proto->addField(
      "speed"_key, field{0.0f, attr<DisplayName>("Speed"), attr<Range>(0.0, 100.0), attr<Description>("m/s.")});
  proto->addField("name"_key, field{std::string{"a"}, attr<DisplayName>("Name")});
  proto->addField("notes"_key, field{std::string{}, attr<DisplayName>("Notes"), attr<Multiline>(6)});
  proto->addField(
      "color"_key,
      field{std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f}, attr<DisplayName>("Color"), attr<ColorField>()});
  proto->addField("offset"_key, field{std::vector<float>{1.0f, 2.0f}, attr<DisplayName>("Offset")});
  proto->addField("ref"_key, field{dynamic_ptr{}, attr<DisplayName>("Ref"), attr<DropTarget>("MyAsset")});
  proto->addField("secret"_key, field{int32_t{0}, attr<DisplayName>("Secret"), attr<Hidden>()});
  proto->addField("second"_key, field{int32_t{0}, attr<DisplayName>("Second"), attr<Order>(2)});
  proto->addField("first"_key, field{int32_t{0}, attr<DisplayName>("First"), attr<Order>(1)});

  // Global namespace (0U), matching the single-arg dynamic::instantiate()
  // overload this file's tests use to create target instances.
  dynamic::addClass(bison::key_t{0U}, std::move(proto));
}

} // namespace

// ── Session-capturing server ─────────────────────────────────────────────────

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

class ObjectInspectorTest : public ::testing::Test {
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

  /// @brief Find the live `object_inspector*` instance created by the last
  ///        instantiate() call, by scanning the session's raw RMI object
  ///        table for the (unique, in these tests) ObjectInspector.
  wish::object_inspector* find_inspector() {
    for (auto& [id, obj] : srv_->last_session->objects) {
      if (auto* oi = dynamic_cast<wish::object_inspector*>(obj.get()))
        return oi;
    }
    return nullptr;
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bison::rmi::client> client_;
};

// ── Prototype / registration ──────────────────────────────────────────────────

TEST_F(ObjectInspectorTest, CanBeInstantiatedViaRmi) {
  auto proxy = client_->instantiate("wish"_key, "ObjectInspector"_key).get();
  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<bison::key_t>(dynamic::CLASS), "ObjectInspector"_key);
}

TEST_F(ObjectInspectorTest, NoTargetBuildsEmptyTableAndPlaceholderDescription) {
  client_->instantiate("wish"_key, "ObjectInspector"_key).get();
  auto* oi = find_inspector();
  ASSERT_NE(oi, nullptr);

  bool found_row = false;
  for (auto& [path, elem] : srv_->last_session->ui_objects) {
    if (path.find(".table.row") != std::string::npos)
      found_row = true;
  }
  EXPECT_FALSE(found_row);
}

// ── __construct: target supplied at instantiate() time ───────────────────────

TEST_F(ObjectInspectorTest, ConstructWithTargetBuildsOneRowPerVisibleField) {
  auto target = dynamic_ptr{dynamic::instantiate("__InspectorTarget"_key)};

  dynamic params;
  params["target"_key] = target;
  client_->instantiate("wish"_key, "ObjectInspector"_key, std::move(params)).get();

  int32_t row_count = 0;
  for (auto& [path, elem] : srv_->last_session->ui_objects) {
    if (path.find(".table.row") != std::string::npos && path.find(".value") == std::string::npos &&
        path.find(".name") == std::string::npos)
      ++row_count;
  }
  // 13 fields declared minus "secret" (Hidden) = 12 visible rows.
  EXPECT_EQ(row_count, 12);
}

// ── Field -> widget dispatch table ───────────────────────────────────────────

class ObjectInspectorDispatchTest : public ObjectInspectorTest {
 protected:
  void SetUp() override {
    ObjectInspectorTest::SetUp();
    target_ = dynamic_ptr{dynamic::instantiate("__InspectorTarget"_key)};
    dynamic params;
    params["target"_key] = target_;
    client_->instantiate("wish"_key, "ObjectInspector"_key, std::move(params)).get();
  }

  /// @brief Find the value-cell widget stamped for @p field_name by its
  ///        synthetic path suffix (".value" on the row whose name-cell
  ///        Label reads the field's DisplayName).
  wish::ui_element_ptr find_value_widget(const std::string& display_name) {
    for (auto& [path, elem] : srv_->last_session->ui_objects) {
      if (path.size() < 5 || path.substr(path.size() - 5) != ".name")
        continue;
      auto* text = elem->findField<std::string>("text"_key);
      if (text && *text == display_name) {
        std::string value_path = path.substr(0, path.size() - 5) + ".value";
        auto it = srv_->last_session->ui_objects.find(value_path);
        if (it != srv_->last_session->ui_objects.end())
          return it->second;
      }
    }
    return {};
  }

  /// @brief Row index (the "rowN" component of its synthetic path) for the
  ///        field whose name-cell Label reads @p display_name, or -1.
  int find_row_index(const std::string& display_name) {
    for (auto& [path, elem] : srv_->last_session->ui_objects) {
      if (path.size() < 5 || path.substr(path.size() - 5) != ".name")
        continue;
      auto* text = elem->findField<std::string>("text"_key);
      if (!text || *text != display_name)
        continue;
      auto row_pos = path.rfind(".row");
      if (row_pos == std::string::npos)
        continue;
      std::string digits = path.substr(row_pos + 4, path.size() - (row_pos + 4) - std::string(".name").size());
      try {
        return std::stoi(digits);
      } catch (...) {
        return -1;
      }
    }
    return -1;
  }

  dynamic_ptr target_;
};

TEST_F(ObjectInspectorDispatchTest, BoolFieldGetsCheckbox) {
  auto w = find_value_widget("Active");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "Checkbox"_key);
}

TEST_F(ObjectInspectorDispatchTest, RangedIntFieldGetsSliderInt) {
  auto w = find_value_widget("Score");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "SliderInt"_key);
  EXPECT_EQ(w->findField<int32_t>("min"_key) ? *w->findField<int32_t>("min"_key) : -1, 0);
  EXPECT_EQ(w->findField<int32_t>("max"_key) ? *w->findField<int32_t>("max"_key) : -1, 10);
}

TEST_F(ObjectInspectorDispatchTest, EnumIntFieldGetsCombo) {
  auto w = find_value_widget("Direction");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "Combo"_key);
  auto* items = w->findField<std::string>("items"_key);
  ASSERT_NE(items, nullptr);
  EXPECT_NE(items->find("North"), std::string::npos);
  EXPECT_NE(items->find("East"), std::string::npos);
}

TEST_F(ObjectInspectorDispatchTest, EnumFlagsIntFieldGetsInputTextWithFlagText) {
  auto w = find_value_widget("Perms");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "InputText"_key);
}

TEST_F(ObjectInspectorDispatchTest, RangedFloatFieldGetsSliderFloat) {
  auto w = find_value_widget("Speed");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "SliderFloat"_key);
}

TEST_F(ObjectInspectorDispatchTest, PlainStringFieldGetsSingleLineInputText) {
  auto w = find_value_widget("Name");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "InputText"_key);
  auto* ml = w->findField<bool>("multiline"_key);
  EXPECT_FALSE(ml && *ml);
}

TEST_F(ObjectInspectorDispatchTest, MultilineStringFieldGetsMultilineInputText) {
  auto w = find_value_widget("Notes");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "InputText"_key);
  auto* ml = w->findField<bool>("multiline"_key);
  ASSERT_NE(ml, nullptr);
  EXPECT_TRUE(*ml);
}

TEST_F(ObjectInspectorDispatchTest, ColorFieldGetsColorEdit) {
  auto w = find_value_widget("Color");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "ColorEdit"_key);
  auto* v = w->findField<std::vector<float>>("value"_key);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->size(), 4u);
}

TEST_F(ObjectInspectorDispatchTest, PlainVectorFieldGetsCommaTextInputText) {
  auto w = find_value_widget("Offset");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "InputText"_key);
  auto* v = w->findField<std::string>("value"_key);
  ASSERT_NE(v, nullptr);
  EXPECT_NE(v->find("1"), std::string::npos);
  EXPECT_NE(v->find("2"), std::string::npos);
}

TEST_F(ObjectInspectorDispatchTest, DynamicPtrFieldWithDropTargetGetsDropButton) {
  auto w = find_value_widget("Ref");
  ASSERT_TRUE(w);
  EXPECT_EQ(w->as<bison::key_t>(dynamic::CLASS), "Button"_key);
  auto* dt = w->findField<std::string>("drop_type"_key);
  ASSERT_NE(dt, nullptr);
  EXPECT_EQ(*dt, "MyAsset");
}

TEST_F(ObjectInspectorDispatchTest, HiddenFieldIsNotShown) {
  EXPECT_FALSE(find_value_widget("Secret"));
}

TEST_F(ObjectInspectorDispatchTest, HiddenFieldExcludedFromRowCountButOthersPresent) {
  EXPECT_TRUE(find_value_widget("Active"));
  EXPECT_TRUE(find_value_widget("First"));
  EXPECT_TRUE(find_value_widget("Second"));
}

TEST_F(ObjectInspectorDispatchTest, OrderAttributeSortsFirstBeforeSecond) {
  int first_row = find_row_index("First");
  int second_row = find_row_index("Second");
  ASSERT_GE(first_row, 0);
  ASSERT_GE(second_row, 0);
  EXPECT_LT(first_row, second_row);
}

// ── handle_changed / handle_dropped / handle_row_event ───────────────────────

TEST_F(ObjectInspectorDispatchTest, HandleChangedCoercesBoolAndNamesTheEditedField) {
  auto w = find_value_widget("Active");
  ASSERT_TRUE(w);
  auto* oi = find_inspector();
  ASSERT_NE(oi, nullptr);

  bison::key_t widget_id = w->as<bison::key_t>("__wish_id"_key);
  dynamic payload;
  payload["value"_key] = true;

  auto edit = oi->handle_changed(widget_id, payload);
  ASSERT_TRUE(edit.has_value());
  EXPECT_EQ(edit->field_name, "active"_key);
  EXPECT_TRUE(edit->new_value.as<bool>("active"_key));
}

TEST_F(ObjectInspectorDispatchTest, HandleChangedOnEnumFieldCommitsSelectedNameAsString) {
  auto w = find_value_widget("Direction");
  ASSERT_TRUE(w);
  auto* oi = find_inspector();
  ASSERT_NE(oi, nullptr);

  bison::key_t widget_id = w->as<bison::key_t>("__wish_id"_key);
  dynamic payload; // Combo's own "changed" contract: {value: index, text: string}
  payload["value"_key] = int32_t{1};
  payload["text"_key] = std::string{"East"};

  auto edit = oi->handle_changed(widget_id, payload);
  ASSERT_TRUE(edit.has_value());
  EXPECT_EQ(edit->field_name, "direction"_key);
  EXPECT_EQ(edit->new_value.as<std::string>("direction"_key), "East");
}

TEST_F(ObjectInspectorDispatchTest, HandleChangedUnknownWidgetIdReturnsNullopt) {
  auto* oi = find_inspector();
  ASSERT_NE(oi, nullptr);
  dynamic payload;
  payload["value"_key] = true;
  EXPECT_FALSE(oi->handle_changed(bison::key_t{999999U}, payload).has_value());
}

TEST_F(ObjectInspectorDispatchTest, HandleDroppedSurfacesRawPayload) {
  auto w = find_value_widget("Ref");
  ASSERT_TRUE(w);
  auto* oi = find_inspector();
  ASSERT_NE(oi, nullptr);

  bison::key_t widget_id = w->as<bison::key_t>("__wish_id"_key);
  dynamic payload;
  payload["payload"_key] = std::string{"MyAsset|some/path.png"};

  auto drop = oi->handle_dropped(widget_id, payload);
  ASSERT_TRUE(drop.has_value());
  EXPECT_EQ(drop->field_name, "ref"_key);
  EXPECT_EQ(drop->payload, "MyAsset|some/path.png");
}

TEST_F(ObjectInspectorDispatchTest, HandleRowEventUpdatesDescriptionLabel) {
  auto* oi = find_inspector();
  ASSERT_NE(oi, nullptr);

  // Find the table's own __wish_id and the row index for "Active" (whose
  // Description is "On/off.").
  bison::key_t table_id{};
  int32_t active_row = -1;
  for (auto& [path, elem] : srv_->last_session->ui_objects) {
    if (path.size() >= 6 && path.substr(path.size() - 6) == ".table")
      table_id = elem->as<bison::key_t>("__wish_id"_key);
  }
  ASSERT_NE(table_id.id, 0U);

  // Rows are built in Order-then-declaration order; rather than hardcode
  // the index, scan the description text produced by every row index until
  // "On/off." appears.
  std::string description_path;
  for (auto& [path, elem] : srv_->last_session->ui_objects) {
    if (path.size() >= 12 && path.substr(path.size() - 12) == ".description")
      description_path = path;
  }
  ASSERT_FALSE(description_path.empty());
  auto& desc_elem = srv_->last_session->ui_objects.at(description_path);

  bool matched = false;
  for (int32_t i = 0; i < 20 && !matched; ++i) {
    dynamic payload;
    payload["index"_key] = i;
    oi->handle_row_event(table_id, "row_selected"_key, payload);
    auto* text = desc_elem->findField<std::string>("text"_key);
    if (text && *text == "On/off.")
      matched = true;
  }
  EXPECT_TRUE(matched);
}

// ── set_target: re-targeting rebuilds the table ───────────────────────────────

TEST_F(ObjectInspectorTest, SetTargetMethodRebuildsRows) {
  auto proxy = client_->instantiate("wish"_key, "ObjectInspector"_key).get();

  int32_t row_count_before = 0;
  for (auto& [path, elem] : srv_->last_session->ui_objects) {
    if (path.find(".table.row") != std::string::npos && path.find(".value") == std::string::npos &&
        path.find(".name") == std::string::npos)
      ++row_count_before;
  }
  EXPECT_EQ(row_count_before, 0);

  auto target = dynamic_ptr{dynamic::instantiate("__InspectorTarget"_key)};
  dynamic params;
  params["target"_key] = target;
  proxy.call("set_target"_key, std::move(params)).get();

  int32_t row_count_after = 0;
  for (auto& [path, elem] : srv_->last_session->ui_objects) {
    if (path.find(".table.row") != std::string::npos && path.find(".value") == std::string::npos &&
        path.find(".name") == std::string::npos)
      ++row_count_after;
  }
  EXPECT_EQ(row_count_after, 12);
}
