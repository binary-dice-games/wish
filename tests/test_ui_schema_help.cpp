// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <ui/ui_schema_help.hpp>
#include "src/bison/bison_object.hpp"

#include <algorithm>

using namespace bdg::bison;
using namespace bdg::wish;

// ── scan_cursor_context ──────────────────────────────────────────────────────
//
// Pure logic, no registry/session needed.

TEST(ScanCursorContextTest, MidTypingTypeValue) {
  auto ctx = scan_cursor_context(R"({"type": "But)", {0, 13});
  EXPECT_EQ(ctx.kind, cursor_context_kind::type_value);
  EXPECT_EQ(ctx.partial_text, "But");
}

TEST(ScanCursorContextTest, FieldKeyAtObjectStart) {
  // Cursor right after "Window", ' at the position just before the closing
  // quote would still typing the object's own type value -- test the key
  // position that follows, right after the comma+space.
  std::string src = R"({"type": "Window", )";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_key);
  EXPECT_EQ(ctx.enclosing_type, "Window");
  EXPECT_TRUE(ctx.partial_text.empty());
}

TEST(ScanCursorContextTest, FieldKeyMidPartial) {
  std::string src = R"({"type": "Window", "wid)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_key);
  EXPECT_EQ(ctx.enclosing_type, "Window");
  EXPECT_EQ(ctx.partial_text, "wid");
}

TEST(ScanCursorContextTest, FieldKeyExcludesAlreadyPresentSiblingsNotItself) {
  std::string src = R"({"type": "Window", "title": "Hi", "wid)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_key);
  ASSERT_EQ(ctx.existing_field_names.size(), 2u);
  EXPECT_EQ(ctx.existing_field_names[0], "type");
  EXPECT_EQ(ctx.existing_field_names[1], "title");
}

TEST(ScanCursorContextTest, FieldValuePartial) {
  std::string src = R"({"type": "Window", "title": "Hel)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_value);
  EXPECT_EQ(ctx.enclosing_type, "Window");
  EXPECT_EQ(ctx.field_name, "title");
  EXPECT_EQ(ctx.partial_text, "Hel");
}

TEST(ScanCursorContextTest, EnumFieldValuePartial) {
  std::string src = R"({"type": "Window", "flags": "No)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_value);
  EXPECT_EQ(ctx.enclosing_type, "Window");
  EXPECT_EQ(ctx.field_name, "flags");
  EXPECT_EQ(ctx.partial_text, "No");
}

TEST(ScanCursorContextTest, CursorBeforeTypeIsWrittenYieldsNoEnclosingType) {
  // Genuine limitation (see scan_cursor_context's doc comment): "type" isn't
  // written anywhere in this object at all (not even after the cursor), so
  // even the forward lookahead finds nothing.
  std::string src = R"({"foo": "bar", )";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_key);
  EXPECT_TRUE(ctx.enclosing_type.empty());
}

TEST(ScanCursorContextTest, CursorAfterOpenBraceFindsTypeWrittenLaterInSameObject) {
  // Cursor sits right after a named child's opening "{", before "type" has
  // been typed -- the backward scan finds nothing, but "type" does appear
  // later in this same child object, so the forward lookahead should find it.
  std::string line0 = R"({"type":"Window","children":{"btn":{)";
  std::string line1 = R"("type":"Button","label":"OK"}}})";
  std::string src = line0 + "\n" + line1;
  auto ctx = scan_cursor_context(src, {0, line0.size()});
  EXPECT_EQ(ctx.enclosing_type, "Button");
}

TEST(ScanCursorContextTest, CursorOnBlankLineBeforeTypeFindsTypeWrittenLater) {
  // Matches the user-reported repro: cursor on a blank, pretty-printed line
  // right after an object's opening "{", before "type" is typed on a
  // following line.
  std::string src =
      "{\n"
      "  \"type\": \"Window\",\n"
      "  \"children\": {\n"
      "    \"btn\": {\n"
      "      \n"
      "      \"type\": \"Button\"\n"
      "    }\n"
      "  }\n"
      "}";
  auto ctx = scan_cursor_context(src, {4, 6});
  EXPECT_EQ(ctx.enclosing_type, "Button");
}

TEST(ScanCursorContextTest, SiblingArrayFieldBeforeTypeIsSkippedByLookahead) {
  // A sibling array field precedes "type" in the same object; the lookahead
  // must skip over it via depth tracking rather than bailing at its "[".
  std::string src = R"({"tags":["a","b"],"type":"Button"})";
  auto ctx = scan_cursor_context(src, {0, 1}); // right after the opening "{"
  EXPECT_EQ(ctx.enclosing_type, "Button");
}

TEST(ScanCursorContextTest, MalformedTailAfterCursorDoesNotAffectResult) {
  // Everything after the cursor is broken (unbalanced quote/brace); the
  // scanner must still succeed since it never reads past the cursor.
  std::string prefix = R"({"type": "Window", "flags": ")";
  std::string src = prefix + R"(No", "unterminated: {[)";
  auto ctx = scan_cursor_context(src, {0, prefix.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_value);
  EXPECT_EQ(ctx.field_name, "flags");
  EXPECT_EQ(ctx.partial_text, "");
}

TEST(ScanCursorContextTest, NestedObjectTypeValueDoesNotSeeParentType) {
  // Typing the inner object's own "type" value: kind is type_value
  // regardless of any enclosing object's type.
  std::string src = R"({"type": "Window", "children": {"a": {"type": "But)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::type_value);
  EXPECT_EQ(ctx.partial_text, "But");
}

TEST(ScanCursorContextTest, NestedObjectFieldValueReflectsChildTypeNotParent) {
  std::string src = R"({"type": "Window", "children": {"a": {"type": "Button", "label": "Hi)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_value);
  EXPECT_EQ(ctx.enclosing_type, "Button"); // not "Window"
  EXPECT_EQ(ctx.field_name, "label");
  EXPECT_EQ(ctx.partial_text, "Hi");
}

TEST(ScanCursorContextTest, FieldKeyInNestedObjectReflectsChildType) {
  std::string src = R"({"type": "Window", "children": {"a": {"type": "Button", "lab)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::field_key);
  EXPECT_EQ(ctx.enclosing_type, "Button");
  EXPECT_EQ(ctx.partial_text, "lab");
}

TEST(ScanCursorContextTest, RightAfterClosingBraceIsUnknown) {
  std::string src = R"({"type": "Window", "children": {"a": {"type": "Button"}} )";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_EQ(ctx.kind, cursor_context_kind::unknown);
}

TEST(ScanCursorContextTest, TopLevelBeforeAnyObjectIsUnknown) {
  auto ctx = scan_cursor_context("   ", {0, 3});
  EXPECT_EQ(ctx.kind, cursor_context_kind::unknown);
}

// ── element_path ──────────────────────────────────────────────────────────────

TEST(ScanCursorContextTest, RootElementPathIsEmptyString) {
  // Mid-typing the root's own "type" value: element_path is set (the root's
  // structural path is known regardless of whether "type" is complete),
  // even though enclosing_type/kind reflect the in-progress token.
  auto ctx = scan_cursor_context(R"({"type": "Win)", {0, 13});
  EXPECT_EQ(ctx.kind, cursor_context_kind::type_value);
  ASSERT_TRUE(ctx.element_path.has_value());
  EXPECT_EQ(*ctx.element_path, "");
}

TEST(ScanCursorContextTest, NamedChildElementPath) {
  std::string src = R"({"type": "Window", "children": {"btn": {"type": "Button", "lab)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  ASSERT_TRUE(ctx.element_path.has_value());
  EXPECT_EQ(*ctx.element_path, "btn");
}

TEST(ScanCursorContextTest, NestedNamedChildElementPath) {
  std::string src =
      R"({"type": "Window", "children": {"a": {"type": "Layout", "children": {"b": {"type": "Button", "lab)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  ASSERT_TRUE(ctx.element_path.has_value());
  EXPECT_EQ(*ctx.element_path, "a.b");
}

TEST(ScanCursorContextTest, ChildrenWrapperGapFallsBackToOwnerPath) {
  // Cursor positioned right after "children": { , before any child is named
  // yet -- element_path should be the *owning* element's path (root, here),
  // not empty/nullopt, so there's still a sensible highlight target.
  std::string src = R"({"type": "Window", "children": {)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  ASSERT_TRUE(ctx.element_path.has_value());
  EXPECT_EQ(*ctx.element_path, "");
}

TEST(ScanCursorContextTest, ChildrenWrapperGapInsideNestedOwnerFallsBackToOwnerPath) {
  std::string src = R"({"type": "Window", "children": {"a": {"type": "Layout", "children": {)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  ASSERT_TRUE(ctx.element_path.has_value());
  EXPECT_EQ(*ctx.element_path, "a");
}

TEST(ScanCursorContextTest, ArrayIndexedChildHasNoElementPath) {
  // Unnamed/array-indexed children get no path from import_json() either
  // (only named object entries in a "children" map do) -- element_path must
  // be nullopt, not fabricated.
  std::string src = R"({"type": "Window", "children": [{"type": "Button", "lab)";
  auto ctx = scan_cursor_context(src, {0, src.size()});
  EXPECT_FALSE(ctx.element_path.has_value());
}

// ── enumerate_ui_element_classes / find_ui_element_class ────────────────────

class UiSchemaHelpRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(UiSchemaHelpRegistryTest, ButtonAppearsWithInheritedElementFields) {
  auto found = find_ui_element_class("Button");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->name, "Button");

  auto has_field = [&](const std::string& name) {
    for (auto& f : found->fields)
      if (f.name == name)
        return true;
    return false;
  };
  EXPECT_TRUE(has_field("label")); // Button's own field
  EXPECT_TRUE(has_field("visible")); // inherited from Element
  EXPECT_TRUE(has_field("children")); // inherited from Element
  EXPECT_TRUE(has_field("order")); // inherited from Element
}

TEST_F(UiSchemaHelpRegistryTest, FormClassesAreExcluded) {
  auto all = enumerate_ui_element_classes();
  for (auto& c : all) {
    EXPECT_NE(c.name, "Editor");
    EXPECT_NE(c.name, "MessageBox");
    EXPECT_NE(c.name, "FileDialog");
  }
  EXPECT_FALSE(find_ui_element_class("Editor").has_value());
  EXPECT_FALSE(find_ui_element_class("MessageBox").has_value());
}

TEST_F(UiSchemaHelpRegistryTest, ElementItselfIsExcluded) {
  auto all = enumerate_ui_element_classes();
  for (auto& c : all)
    EXPECT_NE(c.name, "Element");
  EXPECT_FALSE(find_ui_element_class("Element").has_value());
}

TEST_F(UiSchemaHelpRegistryTest, UnknownTypeNameReturnsNullopt) {
  EXPECT_FALSE(find_ui_element_class("NotARealClassName").has_value());
}

TEST_F(UiSchemaHelpRegistryTest, WindowFlagsIsEnumFlagsWithNamedValues) {
  auto found = find_ui_element_class("Window");
  ASSERT_TRUE(found.has_value());

  const element_field_info* flags = nullptr;
  for (auto& f : found->fields)
    if (f.name == "flags")
      flags = &f;
  ASSERT_NE(flags, nullptr);
  EXPECT_TRUE(flags->is_enum_flags);
  EXPECT_NE(std::find(flags->enum_values.begin(), flags->enum_values.end(), "NoTitleBar"), flags->enum_values.end());
}

TEST_F(UiSchemaHelpRegistryTest, RequiredFieldIsFlagged) {
  auto found = find_ui_element_class("TextEditor");
  ASSERT_TRUE(found.has_value());

  const element_field_info* file_path = nullptr;
  for (auto& f : found->fields)
    if (f.name == "file_path")
      file_path = &f;
  ASSERT_NE(file_path, nullptr);
  EXPECT_TRUE(file_path->required);
}
