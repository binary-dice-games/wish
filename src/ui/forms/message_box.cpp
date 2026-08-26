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

// "icon" and "buttons" are Enum-attributed int32_t fields (see
// register_message_box()) rather than raw strings -- bison validates/parses
// them on assignment and formats them back to their name on read, so an RMI
// client can still set/get e.g. "warning" or "yes_no_cancel" as a string.
constexpr const char* kIconNames[] = {"none", "info", "warning", "error", "question"};
constexpr int32_t kIconCount = int32_t(sizeof(kIconNames) / sizeof(kIconNames[0]));

const Enum::table& icon_enum_table() {
  static const Enum::table t{{"none", 0}, {"info", 1}, {"warning", 2}, {"error", 3}, {"question", 4}};
  return t;
}

const Enum::table& buttons_enum_table() {
  static const Enum::table t{
      {"ok", 0}, {"ok_cancel", 1}, {"yes_no", 2}, {"yes_no_cancel", 3}, {"retry_cancel", 4},
      {"abort_retry_ignore", 5}};
  return t;
}

// ── Hardcoded per-preset layouts ────────────────────────────────────────────
//
// One fixed layout per Win32-style MB_* button preset -- there are only six,
// so each is spelled out rather than assembled at runtime (unlike
// file_dialog.cpp's single reusable layout, the button row here differs by
// preset, not by field values). "title" and "body.message"'s "text" are
// placeholders, overwritten in on_init() from the form's own fields, same
// idiom as file_dialog.cpp's title/confirm_label stamping. "body.icon"'s
// "src" stays empty (render_image() reserves its declared 32x32 without
// drawing anything for an empty src -- see imgui_ui_renderer.cpp) unless
// icon != "none", in which case rebuild() points it at the matching
// msgbox_*.png under resources/embedded/icons/.
//
// "flags" includes AlwaysAutoResize, unlike every other form's Window (e.g.
// bc.cpp's "NoResize" alone): every message_box instance -- across
// ALL SIX presets, with very different content sizes -- shares the SAME
// pool of recycled stable ids (see form::next_available_key()'s doc
// comment; e.g. "__message_box_0" for whichever instance happens to be
// first/only one open). Without AlwaysAutoResize, ImGui only auto-fits a
// window to its content on the frame it first appears, then keeps that
// size (or a size loaded from imgui.ini, keyed by the same id) on every
// later frame -- so a short "OK" box shown first would leave a small
// persisted size that a much taller "Abort/Retry/Ignore" box reusing the
// same id later gets stuck with (needing a scrollbar to fit its own
// content), and vice versa. AlwaysAutoResize forces a fresh content-based
// fit every single frame regardless of whatever size is on disk, so it
// alone is enough to fix the size side of this problem.
//
// Deliberately NOT NoSavedSettings: an earlier version of this file added
// it alongside AlwaysAutoResize on the (mistaken) assumption that avoiding
// a stale ini *size* entry also required suppressing ini persistence
// entirely, but NoSavedSettings blocks *position* persistence too --
// AlwaysAutoResize already makes the size half moot (it's recomputed fresh
// every frame no matter what's in imgui.ini), so nothing on the size side
// actually needed it. The user-visible effect was every message_box
// resetting to ImGui's default (0, 0) placement on each new server
// session instead of reopening wherever the user last left it, unlike
// every other form's Window (e.g. file_dialog.cpp, which persists position
// via the ini normally). Leaving NoSavedSettings off restores that same
// persisted-position behavior for message_box.
//
// No "width"/"height" on the *buttons* below (rebuild() never stamps them
// either): a HorizontalLayout child with an explicit "width" gets wrapped
// in its own ImGui::BeginChild() (see render_horizontal_layout() in
// imgui_ui_renderer.cpp) for column-stretch purposes, and nested BeginChild
// content doesn't get correct hover/click detection inside a
// BeginPopupModal window -- a real, reproducible core interaction bug
// between Window.modal and HorizontalLayout's width-hint column mechanism,
// not just a message_box-specific workaround. The icon Image *does* carry
// an explicit width/height (needed for render_image's own sizing, and for
// Dummy() to reserve consistent space per the comment there) and was
// re-verified via automation not to trip this same BeginChild bug for a
// non-interactive Image -- if that regresses, suspect this interaction
// first.

static constexpr const char* kLayoutOk = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn0": { "type": "Button", "label": "OK", "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutOkCancel = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn0": { "type": "Button", "label": "OK", "height": 32 },
      "btn1": { "type": "Button", "label": "Cancel", "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutYesNo = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn0": { "type": "Button", "label": "Yes", "height": 32 },
      "btn1": { "type": "Button", "label": "No", "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutYesNoCancel = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn0": { "type": "Button", "label": "Yes", "height": 32 },
      "btn1": { "type": "Button", "label": "No", "height": 32 },
      "btn2": { "type": "Button", "label": "Cancel", "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutRetryCancel = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn0": { "type": "Button", "label": "Retry", "height": 32 },
      "btn1": { "type": "Button", "label": "Cancel", "height": 32 }
    } }
  }
})";

static constexpr const char* kLayoutAbortRetryIgnore = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoResize|NoCollapse|AlwaysAutoResize",
  "children": {
    "body": { "type": "HorizontalLayout", "spacing": 12, "children": {
      "icon": { "type": "Image", "src": "", "width": 32, "height": 32 },
      "message": { "type": "Label", "text": "" }
    } },
    "sep": { "type": "Separator" },
    "buttons": { "type": "HorizontalLayout", "spacing": 6, "children": {
      "btn0": { "type": "Button", "label": "Abort", "height": 32 },
      "btn1": { "type": "Button", "label": "Retry", "height": 32 },
      "btn2": { "type": "Button", "label": "Ignore", "height": 32 }
    } }
  }
})";

// Layout + the result string ("ok", "cancel", ...) reported for each button,
// in "buttons.btnN" order, for a given "buttons" field preset (the int32_t
// value parsed via buttons_enum_table() -- see is field's doc comment).
// Unrecognized/out-of-range values fall back to the single-OK-button layout.
struct preset {
  const char* layout;
  std::array<const char*, 3> results; // unused trailing slots are nullptr
};

const preset& preset_for(int32_t buttons) {
  static const preset kOk{kLayoutOk, {"ok", nullptr, nullptr}};
  static const preset kOkCancel{kLayoutOkCancel, {"ok", "cancel", nullptr}};
  static const preset kYesNo{kLayoutYesNo, {"yes", "no", nullptr}};
  static const preset kYesNoCancel{kLayoutYesNoCancel, {"yes", "no", "cancel"}};
  static const preset kRetryCancel{kLayoutRetryCancel, {"retry", "cancel", nullptr}};
  static const preset kAbortRetryIgnore{kLayoutAbortRetryIgnore, {"abort", "retry", "ignore"}};

  switch (buttons) {
    case 1:
      return kOkCancel;
    case 2:
      return kYesNo;
    case 3:
      return kYesNoCancel;
    case 4:
      return kRetryCancel;
    case 5:
      return kAbortRetryIgnore;
    default:
      return kOk;
  }
}

} // namespace

// ── message_box ──────────────────────────────────────────────────────────────

message_box::message_box(dynamic&& base) : cloneable_ui_element(std::move(base)) {}

void message_box::on_init() {
  rebuild();
}

void message_box::rebuild() {
  auto* title_f = findField<std::string>("title"_key);
  auto* message_f = findField<std::string>("message"_key);
  auto* icon_f = findField<int32_t>("icon"_key);
  auto* buttons_f = findField<int32_t>("buttons"_key);

  std::string title = title_f ? *title_f : std::string{"Message"};
  std::string message = message_f ? *message_f : std::string{};
  int32_t icon_val = icon_f ? *icon_f : 1; // "info" -- see the icon field's default.
  int32_t buttons_val = buttons_f ? *buttons_f : 0;

  if (icon_val < 0 || icon_val >= kIconCount)
    icon_val = 0;
  std::string icon = kIconNames[icon_val];

  const preset& p = preset_for(buttons_val);

  button_results_.clear();

  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__message_box_");

  auto tree = import_json(p.layout);

  // Stamp form-field values onto the imported tree (same idiom as
  // file_dialog.cpp's title/confirm_label stamping).
  (*tree[""])["title"_key] = title;
  tree.with("body.message", [&](const auto& e) { e["text"_key] = message; });
  if (icon != "none") {
    tree.with("body.icon", [&](const auto& e) {
      e["src"_key] = "res/icons/msgbox_" + icon + ".png";
      // The msgbox_*.png icons are white/monochrome so they can be tinted;
      // without this they're invisible against the light theme's white
      // background -- see render_image()'s "__tint_to_text_color__"
      // handling (imgui_ui_renderer.cpp), same fix as the file-type icons
      // in file_browser_utils.cpp's make_name_cell().
      e["__tint_to_text_color__"_key] = true;
    });
  }

  // Assign every element a bison RMI ID. See bc.cpp's on_init() for
  // why put_object() (not a direct ctx().objects[id.id] assignment) matters.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();

  for (size_t i = 0; i < p.results.size() && p.results[i]; ++i) {
    std::string path = "buttons.btn" + std::to_string(i);
    tree.with(path, [&](const auto& e) { button_results_[wish_id_of(e)] = p.results[i]; });
  }

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

void message_box::register_root() {
  // Mirrors form::init()'s own post-on_init() registration (form.cpp) --
  // needed here because form::init() already ran this once before
  // __construct fires (see handle_instantiate: on_create_object, which
  // calls form::init(), happens strictly before __construct), so a rebuild
  // triggered from on_construct() must redo it for the new internal_root_key_.
  auto& s = sess();
  auto it = s.ui_objects.find(internal_root_key_);
  if (it == s.ui_objects.end())
    return;
  s.top_level_objects[key_t{internal_root_key_}] = it->second;
  (*it->second)["__path__"_key] = internal_root_key_;
  s.top_level_handlers[key_t{internal_root_key_}] = this;
}

void message_box::on_construct(const dynamic& params) {
  // Apply the instantiate()-time params onto this object's own fields.
  // Without this, "buttons"/"icon"/"title"/"message" passed to
  // instantiate(..., params) would never reach rebuild(): __construct fires
  // after on_init() already built the default "ok"-only tree from the
  // prototype's default field values.
  //
  // Deliberately NOT `(*this)[k] = v` (copying the whole incoming field
  // object) -- unlike field::operator=(T) for a concrete value type T, that
  // resolves to field's compiler-generated copy-assignment operator (an
  // exact non-template match beats the templated one), which replaces the
  // target's variant *and* its attributes wholesale from the (attribute-less)
  // incoming field. For "icon"/"buttons" that silently discards the
  // Enum attribute this field was registered with, so a string value like
  // "yes_no_cancel" is no longer recognized as one and the target quietly
  // keeps its previous value instead of being parsed. Extracting the
  // concrete value and assigning *that* instead goes through the templated
  // field::operator=(const T&), which -- for a std::string assigned to a
  // field still holding int32_t -- explicitly checks for an Enum/EnumFlags
  // attribute and parses it, exactly like a client set() call would.
  params.forEach([this](key_t k, const field& v) {
    if (k == dynamic::CLASS || k == dynamic::PARENT)
      return;
    if (v.is<std::string>())
      (*this)[k] = v.as<std::string>();
    else if (v.is<int32_t>())
      (*this)[k] = v.as<int32_t>();
    else if (v.is<float>())
      (*this)[k] = v.as<float>();
    else if (v.is<bool>())
      (*this)[k] = v.as<bool>();
    else if (v.is<dynamic_ptr>())
      (*this)[k] = v.as<dynamic_ptr>();
  });

  remove_internal_objects();
  rebuild();
  register_root();
}

void message_box::request_close() {
  auto set_flag = [this](context& s) {
    auto it = s.ui_objects.find(internal_root_key_);
    if (it != s.ui_objects.end() && it->second)
      (*it->second)["__request_close__"_key] = true;
  };
  // Mirrors remove_internal_objects()'s own dispatch/non-dispatch branching
  // (form.cpp): on_event() is documented to run outside the session lock,
  // so sess() (which requires an active dispatch) cannot be used here.
  if (detail::current_context) {
    set_flag(*detail::current_context);
  } else {
    auto lock = context_wlock{*sync_ctx_};
    set_flag(*lock);
  }
}

void message_box::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  // The internal Window's own "closed" event fires once render_window()
  // confirms ImGui actually closed the popup requested via request_close()
  // below -- only now is it safe to tear down the tree (see
  // request_close()'s doc comment for why this can't happen immediately on
  // the button click that triggers it).
  if (id == window_id_ && event == "closed"_key) {
    remove_internal_objects();
    return;
  }

  if (event != "clicked"_key)
    return;

  auto it = button_results_.find(id);
  if (it == button_results_.end())
    return;

  dynamic result;
  result["button"_key] = it->second;
  emit("on_result"_key, std::move(result));
  request_close();
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
          int32_t{1}, // "info" -- see kIconNames; a MessageBox with no icon
                      // set at all reads as incomplete/accidental rather
                      // than intentionally plain, so default to something
                      // visible instead of "none".
          attr<DisplayName>("Icon"),
          attr<Description>("Icon severity: \"none\", \"info\", \"warning\", \"error\", or \"question\". "
                            "Defaults to \"info\" when unset. Out-of-range values are treated as \"none\"."),
          attr<Category>("Appearance"),
          attr<Enum>(icon_enum_table())});

  proto->addField(
      "buttons"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Buttons"),
          attr<Description>("Button preset, mirroring Win32 MessageBox styles: \"ok\", "
                            "\"ok_cancel\", \"yes_no\", \"yes_no_cancel\", \"retry_cancel\", "
                            "or \"abort_retry_ignore\". Out-of-range values fall back to \"ok\"."),
          attr<Category>("Behavior"),
          attr<Enum>(buttons_enum_table())});

  // Lets instantiate(..., params) configure title/message/icon/buttons at
  // construction time (mirroring Win32 MessageBox()'s single-call API) --
  // without this, bison::rmi::server::handle_instantiate() silently drops
  // construct-time params, since on_init() (which builds the tree from
  // field values) runs before params are otherwise ever applied.
  proto->addMethod("__construct"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     static_cast<message_box&>(s).on_construct(p);
                     return dynamic{};
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("MessageBox"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("High-level modal message dialog, modeled on the Win32 MessageBox API. "
                        "Set title/message/icon/buttons, then listen for the on_result event "
                        "(payload: {button: \"ok\"|\"cancel\"|\"yes\"|\"no\"|\"retry\"|\"abort\"|\"ignore\"})."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<message_box>("wish"_key, "MessageBox"_key));
}

} // namespace bdg::wish
