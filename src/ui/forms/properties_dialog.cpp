// MIT License © 2025 Binary Dice Games
/// @file properties_dialog.cpp
/// @brief Implementation of the PropertiesDialog form.
#include "properties_dialog.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <ui/ui_importer.hpp>

#include "ui/ui_elements/object_inspector.hpp"

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// Applies one ObjectInspector::field_edit onto `target` in place. Mirrors
// message_box::on_construct()'s "extract the concrete value and assign that"
// idiom (see its doc comment): `edit.new_value[edit.field_name]` is itself an
// attribute-less field (freshly built by handle_changed(), never cloned from
// a prototype), so assigning it onto `target`'s own field via a raw field
// copy-assignment would wipe whatever Enum/EnumFlags/etc. attribute that
// field's prototype declaration carries. Going through the templated
// field::operator=(const T&) instead -- same as every other value type
// handled here -- preserves it, and for a std::string committed onto an
// int32_t Enum/EnumFlags field (handle_changed()'s encoding for those, see
// its own doc comment) actually parses it via that attribute.
void apply_field_edit(const dynamic_ptr& target, const object_inspector::field_edit& edit) {
  if (!target)
    return;
  auto& v = edit.new_value[edit.field_name];
  if (v.is<bool>())
    (*target)[edit.field_name] = v.as<bool>();
  else if (v.is<int32_t>())
    (*target)[edit.field_name] = v.as<int32_t>();
  else if (v.is<float>())
    (*target)[edit.field_name] = v.as<float>();
  else if (v.is<std::string>())
    (*target)[edit.field_name] = v.as<std::string>();
  else if (v.is<std::vector<float>>())
    (*target)[edit.field_name] = v.as<std::vector<float>>();
}

// The ObjectInspector itself is deliberately NOT declared here -- unlike
// every other element in this layout, import_json()/build_ui_node() always
// constructs a plain ui_element (see ui_element.hpp's "All wish element
// instances created by ui_importer are ui_element objects"), never the
// registered C++ subclass a "type" name might have a factory for. A plain
// ui_element with CLASS == "ObjectInspector" would dynamic_cast to nullptr
// and never build any rows. rebuild() below instead constructs it via
// dynamic::create_instance() (which does consult the registered factory --
// see register_object_inspector()) and splices it into "vbox"'s children
// manually, exactly as object_inspector.hpp's own doc comment prescribes
// for direct C++ construction. "height": -1 mirrors mc.cpp's
// left_table_/right_table_ technique: "vbox" (this Window's sole direct
// child) hands "sep"/"close_row" their own natural size first, then gives
// the inspector whatever's left, so a long field list scrolls inside its
// own Table instead of pushing "close_row" off the bottom of the window --
// set alongside "read_only"/"show_description_panel" in rebuild() once the
// real instance exists, not here.
static constexpr const char* kLayout = R"({
  "type": "Window", "title": "", "modal": true, "flags": "NoCollapse",
  "width": 800, "height": 420,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "sep": { "type": "Separator" },
        "close_row": { "type": "HorizontalLayout", "children": {
          "btn_close": { "type": "Button", "label": "Close", "height": 32 }
        } }
      }
    }
  }
})";

} // namespace

// ── properties_dialog ────────────────────────────────────────────────────────

properties_dialog::properties_dialog(dynamic&& base) : cloneable_ui_element(std::move(base)) {}

void properties_dialog::on_init() {
  rebuild();
}

void properties_dialog::release_inspector() {
  if (!inspector_elem_)
    return;
  if (auto* insp = dynamic_cast<object_inspector*>(inspector_elem_.get())) {
    auto do_release = [insp](context& s) { insp->release(s); };
    // Mirrors message_box::request_close()'s own dispatch/non-dispatch
    // branching: this may run from on_event() (outside dispatch, e.g. the
    // window's own "closed" event) or from rebuild() (either context) --
    // but never from a destructor, see this method's doc comment.
    if (detail::current_context) {
      do_release(*detail::current_context);
    } else {
      auto lock = context_wlock{*sync_ctx_};
      do_release(*lock);
    }
  }
  inspector_elem_.reset();
}

void properties_dialog::rebuild() {
  // Release the PREVIOUS inspector's built rows first -- they live at their
  // own independent ui_objects paths (see object_inspector::set_target()'s
  // doc comment), not nested under internal_root_key_, so the
  // remove_internal_objects() below would never reach them on its own.
  release_inspector();

  auto* title_f = findField<std::string>("title"_key);
  std::string title = title_f ? *title_f : std::string{"Properties"};

  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__properties_dialog_");

  auto tree = import_json(kLayout);
  (*tree[""])["title"_key] = title;

  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.close_row.btn_close", [&](const auto& e) { close_id_ = wish_id_of(e); });

  ui_element_ptr vbox_elem;
  tree.with("vbox", [&](const auto& e) { vbox_elem = e; });

  // Built via the registered factory (see kLayout's comment above), not
  // import_json(), so this is a real object_inspector C++ instance --
  // dynamic::create_instance() returns dynamic_ptr (its generic return
  // type), downcast to ui_element_ptr since it's actually an object_inspector,
  // a ui_element subclass, to match what context::ui_objects itself stores.
  dynamic_ptr inspector_dyn = dynamic::create_instance(key_t{hash("wish")}, "ObjectInspector"_key);
  inspector_elem_ = ui_element_ptr{std::static_pointer_cast<ui_element>(std::shared_ptr<dynamic>(inspector_dyn))};
  inspector_elem_["height"_key] = -1.0f;

  // Forwarded verbatim from this form's own fields (see
  // register_properties_dialog()) -- read_only defaults true and
  // show_description_panel defaults false, matching the always-read-only
  // viewer behavior this form had before these became configurable.
  auto* read_only_f = findField<bool>("read_only"_key);
  bool read_only = !read_only_f || *read_only_f;
  auto* show_description_f = findField<bool>("show_description_panel"_key);
  bool show_description_panel = show_description_f && *show_description_f;
  inspector_elem_["read_only"_key] = read_only;
  inspector_elem_["show_description_panel"_key] = show_description_panel;
  key_t inspector_id = rmi::shared::generate_id();
  c.put_object(inspector_id, inspector_dyn);
  inspector_elem_["__wish_id"_key] = inspector_id;
  // Sorts before "sep"/"close_row" (both order 0 by default) -- see
  // ui_element::refresh_children_order()'s ascending-order sort.
  inspector_elem_["order"_key] = int32_t{-1};

  if (vbox_elem) {
    auto* children_p = vbox_elem->findField<dynamic_ptr>("children"_key);
    if (children_p && *children_p) {
      auto& children = *children_p;
      (*children)[inspector_id] = inspector_elem_;
      vbox_elem->refresh_children_order();
    }
  }

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
  sess().ui_objects[internal_root_key_ + ".vbox.inspector"] = inspector_elem_;

  if (auto* inspector = dynamic_cast<object_inspector*>(inspector_elem_.get())) {
    inspector->init(ctx(), sync_ctx_);
    inspector->set_target(sess(), target_);
  }
}

void properties_dialog::register_root() {
  // Mirrors message_box::register_root() -- needed here because form::init()
  // already ran this once (for on_init()'s default, empty-target build)
  // before __construct fires, so on_construct()'s rebuild must redo it for
  // the new internal_root_key_.
  auto& s = sess();
  auto it = s.ui_objects.find(internal_root_key_);
  if (it == s.ui_objects.end())
    return;
  s.top_level_objects[key_t{internal_root_key_}] = it->second;
  (*it->second)["__path__"_key] = internal_root_key_;
  s.top_level_handlers[key_t{internal_root_key_}] = this;
}

void properties_dialog::on_construct(const dynamic& params) {
  // Apply the instantiate()-time params onto this object's own fields --
  // mirrors message_box::on_construct()'s doc comment for why on_init()'s
  // default build never sees these otherwise.
  params.forEach([this](key_t k, const field& v) {
    if (k == dynamic::CLASS || k == dynamic::PARENT)
      return;
    if (v.is<std::string>())
      (*this)[k] = v.as<std::string>();
    else if (v.is<bool>())
      (*this)[k] = v.as<bool>();
    else if (v.is<dynamic_ptr>())
      (*this)[k] = v.as<dynamic_ptr>();
  });

  auto* target_f = findField<dynamic_ptr>("target"_key);
  target_ = target_f ? *target_f : dynamic_ptr{};

  remove_internal_objects();
  rebuild();
  register_root();
}

void properties_dialog::set_target(dynamic_ptr target) {
  target_ = std::move(target);
  (*this)["target"_key] = target_;
  if (auto* insp = dynamic_cast<object_inspector*>(inspector_elem_.get()))
    insp->set_target(sess(), target_);
}

void properties_dialog::on_event(key_t id, key_t event, const dynamic& payload) {
  // Forward every event to the inspector first: row_selected/row_activated
  // keep the (optional) description panel in sync, and -- only relevant
  // when read_only is false -- a value widget's "changed" event resolves to
  // a field_edit committed straight onto target_, so it always reflects the
  // live-edited state by the time on_result reports it below. Both no-op
  // harmlessly for ids/events that aren't theirs (e.g. window_id_'s
  // "closed"), so this is safe to run unconditionally before the
  // window/button-specific handling.
  if (auto* insp = dynamic_cast<object_inspector*>(inspector_elem_.get())) {
    insp->handle_row_event(id, event, payload);
    if (event == "changed"_key) {
      if (auto edit = insp->handle_changed(id, payload))
        apply_field_edit(target_, *edit);
    }
  }

  // Mirrors message_box::on_event()'s own window "closed" handling: only
  // safe to tear the tree down once the Window's own "closed" event
  // confirms ImGui actually closed the popup requested via
  // request_close_at() below. Reports target_ (possibly edited above) back
  // to the caller first -- this is the only place a close can be observed
  // from, regardless of whether it came from the Close button or the
  // window's own X button.
  if (id == window_id_ && event == "closed"_key) {
    dynamic result;
    result["target"_key] = target_;
    emit("on_result"_key, std::move(result));
    release_inspector();
    remove_internal_objects();
    return;
  }

  if (id == close_id_ && event == "clicked"_key)
    request_close_at(internal_root_key_);
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_properties_dialog() {
  auto proto = dynamic_ptr{"PropertiesDialog"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Properties"},
          attr<DisplayName>("Title"),
          attr<Description>("Dialog window title."),
          attr<Category>("Appearance")});

  proto->addField(
      "target"_key,
      field{
          dynamic_ptr{},
          attr<DisplayName>("Target"),
          attr<Description>("The object whose fields this dialog displays -- must be an instance "
                            "of a class registered in this process (see object_inspector.hpp's "
                            "\"Reflection is process-local\" note)."),
          attr<Category>("Content")});

  proto->addField(
      "read_only"_key,
      field{
          true,
          attr<DisplayName>("Read Only"),
          attr<Description>("When true (the default), every field renders as a plain read-only "
                            "row -- a pure viewer. Set to false to make this an in-place editor: "
                            "each edit is committed onto `target` as the user makes it, and the "
                            "(possibly edited) target is reported back in the on_result event's "
                            "payload when the dialog closes. Forwarded verbatim to the internal "
                            "ObjectInspector's own `read_only` field."),
          attr<Category>("Behavior")});

  proto->addField(
      "show_description_panel"_key,
      field{
          false,
          attr<DisplayName>("Show Description Panel"),
          attr<Description>("When true, a description panel below the field table shows the "
                            "selected row's field description. Defaults to false -- the extra "
                            "panel has no room to earn its keep in a dialog this small, but is "
                            "available for a larger or more self-explanatory target class. "
                            "Forwarded verbatim to the internal ObjectInspector's own "
                            "`show_description_panel` field."),
          attr<Category>("Appearance")});

  // Lets instantiate(..., params) configure title/target at construction
  // time, same rationale as message_box's own __construct hook.
  proto->addMethod("__construct"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     static_cast<properties_dialog&>(s).on_construct(p);
                     return dynamic{};
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("PropertiesDialog"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("\"Properties\" dialog: an ObjectInspector reflecting over `target`'s "
                        "registered class (via bison attributes) to show (read_only: true, the "
                        "default) or edit (read_only: false) one row per visible field, plus a "
                        "Close button. Closing the dialog emits on_result with the (possibly "
                        "edited) target. See properties_dialog.hpp."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U},
      dynamic::make_factory<properties_dialog>("wish"_key, "PropertiesDialog"_key));
}

} // namespace bdg::wish
