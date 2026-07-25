// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include "src/bison/bison_object.hpp"

using namespace bdg::bison;

class RegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

// ── register_all() ────────────────────────────────────────────────────────────

TEST_F(RegistryTest, RegisterAllDoesNotThrow) {
  EXPECT_NO_THROW(bdg::wish::register_all());
}

TEST_F(RegistryTest, RegisterAllIsIdempotent) {
  // Second call must not throw or corrupt the registry.
  EXPECT_NO_THROW(bdg::wish::register_all());
  EXPECT_NO_THROW(bdg::wish::register_all());
}

// ── Window ────────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, WindowCanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Window"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bdg::bison::key_t>(), "Window"_key);
}

TEST_F(RegistryTest, WindowInheritsVisibleFromElement) {
  auto obj = dynamic::instantiate("wish"_key, "Window"_key);
  auto* f = obj.findField("visible"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<bool>());
  EXPECT_TRUE(f->as<bool>());
}

TEST_F(RegistryTest, WindowHasTitleAsString) {
  auto obj = dynamic::instantiate("wish"_key, "Window"_key);
  auto* f = obj.findField("title"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
}

TEST_F(RegistryTest, WindowHasIntegerDimensions) {
  auto obj = dynamic::instantiate("wish"_key, "Window"_key);
  EXPECT_TRUE(obj.findField("width"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("height"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("pos_x"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("pos_y"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("flags"_key)->is<int32_t>());
}

TEST_F(RegistryTest, WindowInheritsChildrenFromElement) {
  auto obj = dynamic::instantiate("wish"_key, "Window"_key);
  auto* f = obj.findField("children"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<dynamic_ptr>());
}

// ── Layout hierarchy ──────────────────────────────────────────────────────────

TEST_F(RegistryTest, VerticalLayoutHasSpacingInheritedFromLayout) {
  auto obj = dynamic::instantiate("wish"_key, "VerticalLayout"_key);
  auto* f = obj.findField("spacing"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<float>());
  EXPECT_FLOAT_EQ(f->as<float>(), 0.0f);
}

TEST_F(RegistryTest, HorizontalLayoutHasSpacingInheritedFromLayout) {
  auto obj = dynamic::instantiate("wish"_key, "HorizontalLayout"_key);
  auto* f = obj.findField("spacing"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<float>());
}

TEST_F(RegistryTest, LayoutInheritsVisibleFromElement) {
  auto obj = dynamic::instantiate("wish"_key, "VerticalLayout"_key);
  auto* f = obj.findField("visible"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<bool>());
}

// ── Button ────────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, ButtonHasLabelAsString) {
  auto obj = dynamic::instantiate("wish"_key, "Button"_key);
  auto* f = obj.findField("label"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
}

TEST_F(RegistryTest, ButtonInheritsVisibleFromElement) {
  auto obj = dynamic::instantiate("wish"_key, "Button"_key);
  ASSERT_NE(obj.findField("visible"_key), nullptr);
}

// ── Checkbox ──────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, CheckboxHasValueAsBool) {
  auto obj = dynamic::instantiate("wish"_key, "Checkbox"_key);
  auto* f = obj.findField("value"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<bool>());
  EXPECT_FALSE(f->as<bool>());
}

TEST_F(RegistryTest, CheckboxHasLabelAsString) {
  auto obj = dynamic::instantiate("wish"_key, "Checkbox"_key);
  EXPECT_TRUE(obj.findField("label"_key)->is<std::string>());
}

// ── Sliders ───────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, SliderFloatHasFloatFields) {
  auto obj = dynamic::instantiate("wish"_key, "SliderFloat"_key);
  EXPECT_TRUE(obj.findField("value"_key)->is<float>());
  EXPECT_TRUE(obj.findField("min"_key)->is<float>());
  EXPECT_TRUE(obj.findField("max"_key)->is<float>());
  EXPECT_TRUE(obj.findField("format"_key)->is<std::string>());
}

TEST_F(RegistryTest, SliderIntHasIntFields) {
  auto obj = dynamic::instantiate("wish"_key, "SliderInt"_key);
  EXPECT_TRUE(obj.findField("value"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("min"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("max"_key)->is<int32_t>());
}

// ── InputText ─────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, InputTextHasStringAndIntFields) {
  auto obj = dynamic::instantiate("wish"_key, "InputText"_key);
  EXPECT_TRUE(obj.findField("label"_key)->is<std::string>());
  EXPECT_TRUE(obj.findField("value"_key)->is<std::string>());
  EXPECT_TRUE(obj.findField("hint"_key)->is<std::string>());
  EXPECT_TRUE(obj.findField("max_length"_key)->is<int32_t>());
  EXPECT_EQ(obj.findField("max_length"_key)->as<int32_t>(), 256);
}

// ── Image ─────────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, ImageHasExpectedFields) {
  auto obj = dynamic::instantiate("wish"_key, "Image"_key);
  EXPECT_TRUE(obj.findField("src"_key)->is<std::string>());
  EXPECT_TRUE(obj.findField("width"_key)->is<int32_t>());
  EXPECT_TRUE(obj.findField("height"_key)->is<int32_t>());
}

// ── Separator ─────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, SeparatorCanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "Separator"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bdg::bison::key_t>(), "Separator"_key);
}

TEST_F(RegistryTest, SeparatorInheritsVisibleFromElement) {
  auto obj = dynamic::instantiate("wish"_key, "Separator"_key);
  ASSERT_NE(obj.findField("visible"_key), nullptr);
}

// ── DisplayName on CLASS field ────────────────────────────────────────────────

TEST_F(RegistryTest, ClassFieldHasDisplayNameViaProto) {
  // DisplayName is stored on the prototype's CLASS field, not the instance's.
  auto obj = dynamic::instantiate("wish"_key, "Button"_key);
  auto class_key = obj.as<bdg::bison::key_t>(bdg::bison::dynamic::CLASS);
  auto* proto = obj.findClass(class_key);
  ASSERT_NE(proto, nullptr) << "findClass should find the Button prototype";
  auto* cls = proto->findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  auto* dn = cls->findAttribute<DisplayName>();
  ASSERT_NE(dn, nullptr) << "DisplayName should be accessible on prototype CLASS field";
  EXPECT_EQ(dn->name(), "Button");
}

// ── Label ─────────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, LabelHasTextAsString) {
  auto obj = dynamic::instantiate("wish"_key, "Label"_key);
  EXPECT_TRUE(obj.findField("text"_key)->is<std::string>());
}

// ── MenuButton ────────────────────────────────────────────────────────────────

TEST_F(RegistryTest, MenuButtonHasLabelAsString) {
  auto obj = dynamic::instantiate("wish"_key, "MenuButton"_key);
  auto* f = obj.findField("label"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<std::string>());
}

TEST_F(RegistryTest, MenuButtonInheritsChildrenFromElement) {
  auto obj = dynamic::instantiate("wish"_key, "MenuButton"_key);
  auto* f = obj.findField("children"_key);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->is<dynamic_ptr>());
}
