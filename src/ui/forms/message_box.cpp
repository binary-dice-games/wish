// MIT License © 2025 Binary Dice Games
/// @file message_box.cpp
/// @brief Implementation of the MessageBox form.
#include "message_box.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/ui_importer.hpp>

#include <array>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

bool is_known_icon(const std::string& icon) {
  return icon == "none" || icon == "info" || icon == "warning" || icon == "error" || icon == "question";
}

// ── Hardcoded per-preset layouts ────────────────────────────────────────────
//
// One fixed layout per Win32-style MB_* button preset -- there are only six,
// so each is spelled out rather than assembled at runtime (unlike
// file_dialog.cpp's single reusable layout, the button row here differs by
// preset, not by field values). "title" and "body.message"'s "text" are
// placeholders, overwritten in on_init() from the form's own fields, same
// idiom as file_dialog.cpp's title/confirm_label stamping. "body.icon"'s
// "src" stays empty (render_image() no-ops on an empty src) unless icon !=
// "none". ImGuiWindowFlags: NoResize(1<<1=2) | NoCollapse(1<<5=32) = 34.

static constexpr const char* kLayoutOk = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "align": "right", "children": {
      "btn0": { "type": "Button", "label": "OK", "width": 84, "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutOkCancel = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "align": "right", "children": {
      "btn0": { "type": "Button", "label": "OK", "width": 84, "height": 32 },
      "btn1": { "type": "Button", "label": "Cancel", "width": 84, "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutYesNo = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "align": "right", "children": {
      "btn0": { "type": "Button", "label": "Yes", "width": 84, "height": 32 },
      "btn1": { "type": "Button", "label": "No", "width": 84, "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutYesNoCancel = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "align": "right", "children": {
      "btn0": { "type": "Button", "label": "Yes", "width": 84, "height": 32 },
      "btn1": { "type": "Button", "label": "No", "width": 84, "height": 32 },
      "btn2": { "type": "Button", "label": "Cancel", "width": 84, "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutRetryCancel = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "align": "right", "children": {
      "btn0": { "type": "Button", "label": "Retry", "width": 84, "height": 32 },
      "btn1": { "type": "Button", "label": "Cancel", "width": 84, "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutAbortRetryIgnore = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "align": "right", "children": {
      "btn0": { "type": "Button", "label": "Abort", "width": 84, "height": 32 },
      "btn1": { "type": "Button", "label": "Retry", "width": 84, "height": 32 },
      "btn2": { "type": "Button", "label": "Ignore", "width": 84, "height": 32 }
    } }
  }
})";

// Layout + the result string ("ok", "cancel", ...) reported for each button,
// in "buttons.btnN" order, for a given "buttons" field preset. Unrecognized
// presets fall back to the single-OK-button layout.
struct preset {
  const char* layout;
  std::array<const char*, 3> results; // unused trailing slots are nullptr
};

const preset& preset_for(const std::string& buttons) {
  static const preset kOk{kLayoutOk, {"ok", nullptr, nullptr}};
  static const preset kOkCancel{kLayoutOkCancel, {"ok", "cancel", nullptr}};
  static const preset kYesNo{kLayoutYesNo, {"yes", "no", nullptr}};
  static const preset kYesNoCancel{kLayoutYesNoCancel, {"yes", "no", "cancel"}};
  static const preset kRetryCancel{kLayoutRetryCancel, {"retry", "cancel", nullptr}};
  static const preset kAbortRetryIgnore{kLayoutAbortRetryIgnore, {"abort", "retry", "ignore"}};

  if (buttons == "ok_cancel")
    return kOkCancel;
  if (buttons == "yes_no")
    return kYesNo;
  if (buttons == "yes_no_cancel")
    return kYesNoCancel;
  if (buttons == "retry_cancel")
    return kRetryCancel;
  if (buttons == "abort_retry_ignore")
    return kAbortRetryIgnore;
  return kOk;
}

} // namespace

// ── message_box ──────────────────────────────────────────────────────────────

message_box::message_box(dynamic&& base) : form(std::move(base)) {}

void message_box::on_init() {
  auto* title_f = findField<std::string>("title"_key);
  auto* message_f = findField<std::string>("message"_key);
  auto* icon_f = findField<std::string>("icon"_key);
  auto* buttons_f = findField<std::string>("buttons"_key);

  std::string title = title_f ? *title_f : std::string{"Message"};
  std::string message = message_f ? *message_f : std::string{};
  std::string icon = icon_f ? *icon_f : std::string{"none"};
  std::string buttons = buttons_f ? *buttons_f : std::string{"ok"};

  if (!is_known_icon(icon))
    icon = "none";

  const preset& p = preset_for(buttons);

  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__message_box_");

  auto tree = import_json(p.layout);

  // Stamp form-field values onto the imported tree (same idiom as
  // file_dialog.cpp's title/confirm_label stamping).
  (*tree[""])["title"_key] = title;
  tree.with("body.message", [&](const auto& e) { e["text"_key] = message; });
  if (icon != "none")
    tree.with("body.icon", [&](const auto& e) { e["src"_key] = "res/icons/msgbox_" + icon + ".png"; });

  // Assign every element a bison RMI ID. See calculator.cpp's on_init() for
  // why put_object() (not a direct ctx().objects[id.id] assignment) matters.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  for (size_t i = 0; i < p.results.size() && p.results[i]; ++i) {
    std::string path = "buttons.btn" + std::to_string(i);
    tree.with(path, [&](const auto& e) { button_results_[wish_id_of(e)] = p.results[i]; });
  }

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

void message_box::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (event != "clicked"_key)
    return;

  auto it = button_results_.find(id);
  if (it == button_results_.end())
    return;

  dynamic result;
  result["button"_key] = it->second;
  emit("on_result"_key, std::move(result));
  remove_internal_objects();
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_message_box() {
  auto proto = dynamic_ptr{"MessageBox"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Message"},
          attr<DisplayName>("Title"),
          attr<Description>("Dialog window title."),
          attr<Category>("Appearance")});

  proto->addField(
      "message"_key,
      field{
          std::string{""},
          attr<DisplayName>("Message"),
          attr<Description>("Body text shown next to the icon."),
          attr<Category>("Content")});

  proto->addField(
      "icon"_key,
      field{
          std::string{"none"},
          attr<DisplayName>("Icon"),
          attr<Description>("One of \"none\", \"info\", \"warning\", \"error\", \"question\". "
                            "Unrecognized values are treated as \"none\"."),
          attr<Category>("Appearance")});

  proto->addField(
      "buttons"_key,
      field{
          std::string{"ok"},
          attr<DisplayName>("Buttons"),
          attr<Description>("Button preset, mirroring Win32 MessageBox styles: \"ok\", "
                            "\"ok_cancel\", \"yes_no\", \"yes_no_cancel\", \"retry_cancel\", "
                            "or \"abort_retry_ignore\". Unrecognized values fall back to \"ok\"."),
          attr<Category>("Behavior")});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MessageBox"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("High-level modal message dialog, modeled on the Win32 MessageBox API. "
                        "Set title/message/icon/buttons, then listen for the on_result event "
                        "(payload: {button: \"ok\"|\"cancel\"|\"yes\"|\"no\"|\"retry\"|\"abort\"|\"ignore\"})."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<message_box>("wish"_key, "MessageBox"_key));
}

} // namespace bdg::wish
