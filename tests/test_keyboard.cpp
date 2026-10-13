// MIT License © 2026 Binary Dice Games
/// @file test_keyboard.cpp
/// @brief Keyboard-only control: access keys ("&OK" + Alt+O), shortcut
///        strings ("Ctrl+S", "F5"), the Shortcut element and Tab navigation
///        across layout child windows.
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <imgui/imgui_keyboard.hpp>
#include <imgui/imgui_renderer.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <imgui.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace bdg::bison;
using bdg::wish::access_key_label;
using bdg::wish::context;
using bdg::wish::imgui_renderer;
using bdg::wish::parse_access_key;
using bdg::wish::parse_key_chord;
using bdg::wish::ui_element;

// ── parse_access_key ─────────────────────────────────────────────────────────

namespace {

struct parsed {
  std::string text;
  int index;
  ImGuiKey key;
};

parsed parse(const std::string& label) {
  std::string storage;
  access_key_label ak = parse_access_key(label, storage);
  return {ak.text, ak.index, ak.key};
}

} // namespace

TEST(AccessKey, MarksFirstLetterAfterAmpersand) {
  auto p = parse("&File");
  EXPECT_EQ(p.text, "File");
  EXPECT_EQ(p.index, 0);
  EXPECT_EQ(p.key, ImGuiKey_F);

  p = parse("Save &as...");
  EXPECT_EQ(p.text, "Save as...");
  EXPECT_EQ(p.index, 5);
  EXPECT_EQ(p.key, ImGuiKey_A);

  p = parse("Option &2");
  EXPECT_EQ(p.key, ImGuiKey_2);
}

TEST(AccessKey, DoubleAmpersandAndSpacedAmpersandAreLiteral) {
  auto p = parse("R&&D");
  EXPECT_EQ(p.text, "R&D");
  EXPECT_EQ(p.key, ImGuiKey_None);

  // Existing labels such as "Sliders & Drags" must render unchanged.
  p = parse("Sliders & Drags");
  EXPECT_EQ(p.text, "Sliders & Drags");
  EXPECT_EQ(p.key, ImGuiKey_None);

  p = parse("Trailing&");
  EXPECT_EQ(p.text, "Trailing&");
  EXPECT_EQ(p.key, ImGuiKey_None);
}

TEST(AccessKey, OnlyFirstMarkerCountsAndIdSuffixIsKept) {
  auto p = parse("&One &Two##id&X");
  EXPECT_EQ(p.text, "One &Two##id&X");
  EXPECT_EQ(p.key, ImGuiKey_O);

  p = parse("Plain##&X");
  EXPECT_EQ(p.text, "Plain##&X");
  EXPECT_EQ(p.key, ImGuiKey_None);
}

// ── parse_key_chord ──────────────────────────────────────────────────────────

TEST(KeyChord, ParsesModifiersAndKeys) {
  EXPECT_EQ(parse_key_chord("Ctrl+S"), ImGuiMod_Ctrl | ImGuiKey_S);
  EXPECT_EQ(parse_key_chord("ctrl+shift+n"), ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N);
  EXPECT_EQ(parse_key_chord("Alt+F4"), ImGuiMod_Alt | ImGuiKey_F4);
  EXPECT_EQ(parse_key_chord("F12"), ImGuiKey_F12);
  EXPECT_EQ(parse_key_chord("F24"), ImGuiKey_F24);
  EXPECT_EQ(parse_key_chord("Del"), ImGuiKey_Delete);
  EXPECT_EQ(parse_key_chord("Control + PgDn"), ImGuiMod_Ctrl | ImGuiKey_PageDown);
  EXPECT_EQ(parse_key_chord("Ctrl++"), ImGuiMod_Ctrl | ImGuiKey_Equal);
  EXPECT_EQ(parse_key_chord("Ctrl+Plus"), ImGuiMod_Ctrl | ImGuiKey_Equal);
  EXPECT_EQ(parse_key_chord("Ctrl+-"), ImGuiMod_Ctrl | ImGuiKey_Minus);
  EXPECT_EQ(parse_key_chord("Esc"), ImGuiKey_Escape);
  EXPECT_EQ(parse_key_chord("Super+Up"), ImGuiMod_Super | ImGuiKey_UpArrow);
  EXPECT_EQ(parse_key_chord("Num5"), ImGuiKey_Keypad5);
  EXPECT_EQ(parse_key_chord("NumPlus"), ImGuiKey_KeypadAdd);
  EXPECT_EQ(parse_key_chord("Ctrl+NumMinus"), ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract);
  EXPECT_EQ(parse_key_chord("NumEnter"), ImGuiKey_KeypadEnter);
}

TEST(KeyChord, RejectsInvalidStrings) {
  EXPECT_EQ(parse_key_chord(""), 0);
  EXPECT_EQ(parse_key_chord("Ctrl"), 0);
  EXPECT_EQ(parse_key_chord("Ctrl+"), 0);
  EXPECT_EQ(parse_key_chord("F0"), 0);
  EXPECT_EQ(parse_key_chord("F25"), 0);
  EXPECT_EQ(parse_key_chord("Ctrl+Bogus"), 0);
  EXPECT_EQ(parse_key_chord("A+B"), 0);
}

// ── Rendering with injected keys ─────────────────────────────────────────────

class KeyboardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID(ImTextureID{1});
    io.MousePos = ImVec2(-1.0f, -1.0f);
    bdg::wish::enable_keyboard_navigation(io);
    sess_ = std::make_unique<context>(bdg::bison::rmi::shared::generate_id());
    renderer_ = std::make_unique<imgui_renderer>();
  }

  void TearDown() override {
    renderer_.reset();
    sess_.reset();
    ImGui::DestroyContext(ctx_);
  }

  /// Renders @p root for one frame: inside a full-screen test window, or
  /// directly when @p root is itself a Window.
  void frame(const ui_element& root, bool own_window = false) {
    renderer_->begin_frame();
    if (own_window) {
      renderer_->render_node(root, *sess_);
    } else {
      ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
      ImGui::Begin("TestWindow", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
      renderer_->render_node(root, *sess_);
      ImGui::End();
    }
    renderer_->end_frame();
  }

  void frames(const ui_element& root, int n, bool own_window = false) {
    for (int i = 0; i < n; ++i)
      frame(root, own_window);
  }

  /// Presses and releases @p chord (modifiers included) over a few frames.
  void tap(const ui_element& root, ImGuiKeyChord chord, bool own_window = false) {
    set_keys(chord, true);
    frame(root, own_window);
    set_keys(chord, false);
    frames(root, 2, own_window);
  }

  /// Names of the widget events enqueued so far, then clears them.
  std::vector<std::string> take_events() {
    std::vector<std::string> names;
    // Only the widget events under test; a Window also emits its own
    // lifecycle events, which are not interesting here.
    for (auto& ev : sess_->pending_events) {
      if (ev.event_name == "clicked"_key)
        names.push_back("clicked");
      else if (ev.event_name == "changed"_key)
        names.push_back("changed");
      else if (ev.event_name == "pressed"_key)
        names.push_back("pressed");
    }
    sess_->pending_events.clear();
    return names;
  }

  static void set_keys(ImGuiKeyChord chord, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    if (chord & ImGuiMod_Ctrl) {
      io.AddKeyEvent(ImGuiKey_LeftCtrl, down);
      io.AddKeyEvent(ImGuiMod_Ctrl, down);
    }
    if (chord & ImGuiMod_Shift) {
      io.AddKeyEvent(ImGuiKey_LeftShift, down);
      io.AddKeyEvent(ImGuiMod_Shift, down);
    }
    if (chord & ImGuiMod_Alt) {
      io.AddKeyEvent(ImGuiKey_LeftAlt, down);
      io.AddKeyEvent(ImGuiMod_Alt, down);
    }
    if (ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_); key != ImGuiKey_None)
      io.AddKeyEvent(key, down);
  }

  ImGuiContext* ctx_ = nullptr;
  std::unique_ptr<context> sess_;
  std::unique_ptr<imgui_renderer> renderer_;
};

TEST_F(KeyboardTest, AltLetterClicksButtonNestedInLayouts) {
  // Nested layouts wrap the button in child windows; NavFlattened keeps the
  // access key routed to the (focused) parent window.
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "row":{"type":"HorizontalLayout","children":{
        "cancel":{"type":"Button","label":"Cancel"},
        "ok":{"type":"Button","label":"&OK"}}}}})");
  frames(*map[""], 3);
  ASSERT_TRUE(take_events().empty());

  tap(*map[""], ImGuiMod_Alt | ImGuiKey_O);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});

  // The bare letter (no Alt) does nothing outside a popup menu.
  tap(*map[""], ImGuiKey_O);
  EXPECT_TRUE(take_events().empty());
}

TEST_F(KeyboardTest, AltLetterTogglesCheckbox) {
  auto map = bdg::wish::import_json(R"({"type":"Checkbox","label":"&Enable","value":false})");
  frames(*map[""], 3);
  tap(*map[""], ImGuiMod_Alt | ImGuiKey_E);
  EXPECT_EQ(take_events(), std::vector<std::string>{"changed"});
  EXPECT_TRUE(map[""]->get_as<bool>("value"_key, false));
}

TEST_F(KeyboardTest, ButtonShortcutAndShortcutElementFire) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "refresh":{"type":"Button","label":"Refresh","shortcut":"F5"},
      "help":{"type":"Shortcut","keys":"F1"},
      "off":{"type":"Shortcut","keys":"F2","enabled":false}}})");
  frames(*map[""], 3);

  tap(*map[""], ImGuiKey_F5);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
  tap(*map[""], ImGuiKey_F1);
  EXPECT_EQ(take_events(), std::vector<std::string>{"pressed"});
  tap(*map[""], ImGuiKey_F2);
  EXPECT_TRUE(take_events().empty());
}

namespace {

constexpr const char* kMenuWindow = R"({"type":"Window","title":"Editor","width":400,"height":300,
  "children":{
    "bar":{"type":"MenuBar","children":{
      "file":{"type":"Menu","label":"&File","children":{
        "new":{"type":"MenuItem","label":"&New"},
        "save":{"type":"MenuItem","label":"&Save","shortcut":"Ctrl+S"},
        "gone":{"type":"MenuItem","label":"Gone","shortcut":"Ctrl+D","enabled":false}}},
      "edit":{"type":"Menu","label":"Edit","enabled":false,"children":{
        "undo":{"type":"MenuItem","label":"Undo","shortcut":"Ctrl+Z"}}}}},
    "body":{"type":"Label","text":"hello"}}})";

} // namespace

TEST_F(KeyboardTest, MenuItemShortcutFiresWhileMenuClosed) {
  auto map = bdg::wish::import_json(kMenuWindow);
  frames(*map[""], 3, true);

  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_S, true);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});

  // Disabled item, and an item inside a disabled Menu: nothing.
  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_D, true);
  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_Z, true);
  EXPECT_TRUE(take_events().empty());
}

TEST_F(KeyboardTest, AltLetterOpensMenuAndLetterActivatesItem) {
  auto map = bdg::wish::import_json(kMenuWindow);
  frames(*map[""], 3, true);

  tap(*map[""], ImGuiMod_Alt | ImGuiKey_F, true);
  frames(*map[""], 3, true);
  ASSERT_TRUE(take_events().empty());
  ASSERT_TRUE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));

  tap(*map[""], ImGuiKey_N, true);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
  frames(*map[""], 2, true);
  EXPECT_FALSE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
}

TEST_F(KeyboardTest, TabMovesBetweenInputsAcrossLayoutChildren) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "r1":{"type":"HorizontalLayout","children":{"a":{"type":"InputText","label":"A","value":"a"}}},
      "r2":{"type":"HorizontalLayout","children":{"b":{"type":"InputText","label":"B","value":"b"}}}}})");
  frames(*map[""], 3);

  tap(*map[""], ImGuiKey_Tab); // focus A
  tap(*map[""], ImGuiKey_Tab); // focus B
  ImGui::GetIO().AddInputCharacter('x');
  frames(*map[""], 2);

  // Tab-focus selects the field's text, so typing replaces it.
  EXPECT_EQ(map["r1.a"]->get_as<std::string>("value"_key, ""), "a");
  EXPECT_EQ(map["r2.b"]->get_as<std::string>("value"_key, ""), "x");
}

TEST_F(KeyboardTest, SpaceAndEnterPressFocusedButton) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "first":{"type":"InputText","label":"Name","value":""},
      "go":{"type":"Button","label":"Go"}}})");
  frames(*map[""], 3);

  tap(*map[""], ImGuiKey_Tab); // Name
  tap(*map[""], ImGuiKey_Tab); // Go
  take_events();
  tap(*map[""], ImGuiKey_Space);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
  tap(*map[""], ImGuiKey_Enter);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
}

TEST_F(KeyboardTest, DuplicateShortcutInOneWindowFiresOnlyFirst) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "first":{"type":"Button","label":"First","shortcut":"Ctrl+S"},
      "second":{"type":"Button","label":"Second","shortcut":"Ctrl+S"}}})");
  frames(*map[""], 3);
  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_S);
  ASSERT_EQ(sess_->pending_events.size(), 1u);
  EXPECT_EQ(sess_->pending_events[0].event_name, "clicked"_key);
  EXPECT_EQ(sess_->pending_events[0].id, map["first"]->wish_id());
}

TEST_F(KeyboardTest, ShortcutAlternativesEachFire) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "five":{"type":"Button","label":"5","shortcut":"5, Num5"},
      "comma":{"type":"Shortcut","keys":"Ctrl+,"}}})");
  frames(*map[""], 3);
  tap(*map[""], ImGuiKey_5);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
  tap(*map[""], ImGuiKey_Keypad5);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_Comma);
  EXPECT_EQ(take_events(), std::vector<std::string>{"pressed"});
}

TEST_F(KeyboardTest, EnterShortcutYieldsToKeyboardFocusedControl) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "ok":{"type":"Button","label":"OK","shortcut":"Enter"},
      "cancel":{"type":"Button","label":"Cancel","shortcut":"Esc"}}})");
  frames(*map[""], 3);

  // Nothing focused: Enter is the default button.
  tap(*map[""], ImGuiKey_Enter);
  ASSERT_EQ(sess_->pending_events.size(), 1u);
  EXPECT_EQ(sess_->pending_events[0].id, map["ok"]->wish_id());
  sess_->pending_events.clear();

  // Tab to OK, Tab to Cancel: Enter now presses the focused Cancel only.
  tap(*map[""], ImGuiKey_Tab);
  tap(*map[""], ImGuiKey_Tab);
  sess_->pending_events.clear();
  tap(*map[""], ImGuiKey_Enter);
  ASSERT_EQ(sess_->pending_events.size(), 1u);
  EXPECT_EQ(sess_->pending_events[0].id, map["cancel"]->wish_id());
}

// ── Shortcut scope and typing ────────────────────────────────────────────────

namespace {

/// Two windows side by side; click_x picks which one gets focus.
constexpr const char* kLeftWindow = R"({"type":"Window","title":"Left","pos_x":0,"pos_y":0,
    "width":300,"height":200,"children":{
      "app_key":{"type":"Shortcut","keys":"F6","scope":"app"},
      "win_key":{"type":"Shortcut","keys":"F7"}}})";
constexpr const char* kRightWindow = R"({"type":"Window","title":"Right","pos_x":400,"pos_y":0,
    "width":300,"height":200,"children":{"text":{"type":"Label","text":"right"}}})";

} // namespace

class KeyboardScopeTest : public KeyboardTest {
 protected:
  /// One frame of left+right (same session) and, optionally, other (a
  /// second session).
  void frame2() {
    renderer_->begin_frame();
    renderer_->render_node(*left_[""], *sess_);
    renderer_->render_node(*right_[""], *sess_);
    if (other_sess_)
      renderer_->render_node(*other_[""], *other_sess_);
    renderer_->end_frame();
  }

  void click_at(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame2();
    io.AddMouseButtonEvent(0, true);
    frame2();
    io.AddMouseButtonEvent(0, false);
    frame2();
    frame2();
  }

  void tap2(ImGuiKeyChord chord) {
    set_keys(chord, true);
    frame2();
    set_keys(chord, false);
    frame2();
    frame2();
  }

  // Imported in SetUp(): the base SetUp() registers the widget classes.
  void SetUp() override {
    KeyboardTest::SetUp();
    left_ = bdg::wish::import_json(kLeftWindow);
    right_ = bdg::wish::import_json(kRightWindow);
    other_ = bdg::wish::import_json(
        R"({"type":"Window","title":"Other","pos_x":0,"pos_y":300,"width":300,"height":200,
            "children":{"t":{"type":"Label","text":"other"}}})");
  }

  bdg::wish::ui_tree left_;
  bdg::wish::ui_tree right_;
  bdg::wish::ui_tree other_;
  std::unique_ptr<context> other_sess_;
};

TEST_F(KeyboardScopeTest, AppScopeFiresFromAnyWindowOfTheSameApp) {
  frame2();
  click_at(500, 100); // focus "Right"
  take_events();
  tap2(ImGuiKey_F6); // app scope: fires although "Left" is not focused
  tap2(ImGuiKey_F7); // window scope: does not
  EXPECT_EQ(take_events(), std::vector<std::string>{"pressed"});

  click_at(100, 100); // focus "Left": both fire
  take_events();
  tap2(ImGuiKey_F6);
  tap2(ImGuiKey_F7);
  EXPECT_EQ(take_events(), (std::vector<std::string>{"pressed", "pressed"}));
}

TEST_F(KeyboardScopeTest, AppScopeIgnoresOtherAppsWindows) {
  other_sess_ = std::make_unique<context>(bdg::bison::rmi::shared::generate_id());
  frame2();
  click_at(100, 400); // focus the other session's window
  take_events();
  tap2(ImGuiKey_F6);
  EXPECT_TRUE(take_events().empty());
}

TEST_F(KeyboardTest, ModifierShortcutsFireWhileTypingButEditingChordsDoNot) {
  auto map = bdg::wish::import_json(R"({"type":"VerticalLayout","children":{
      "name":{"type":"InputText","label":"Name","value":""},
      "save":{"type":"Button","label":"Save","shortcut":"Ctrl+S"},
      "all":{"type":"Button","label":"All","shortcut":"Ctrl+A"},
      "go":{"type":"Button","label":"Go","shortcut":"G"}}})");
  frames(*map[""], 3);
  tap(*map[""], ImGuiKey_Tab); // start editing "Name"
  ASSERT_TRUE(ImGui::GetIO().WantTextInput);
  take_events();

  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_S);
  EXPECT_EQ(take_events(), std::vector<std::string>{"clicked"});
  tap(*map[""], ImGuiMod_Ctrl | ImGuiKey_A); // select-all in the field instead
  tap(*map[""], ImGuiKey_G); // types 'g'
  EXPECT_TRUE(take_events().empty());
}
