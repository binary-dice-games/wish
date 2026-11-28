// MIT License © 2025 Binary Dice Games
/// @file object_inspector.cpp
/// @brief Implementation of the ObjectInspector element.
#include "src/ui/ui_elements/object_inspector.hpp"

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace bdg::wish {

using namespace bdg::bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

/// @brief Look up a registered class's prototype directly, without
///        instantiating it -- used to enumerate a class's fields (an
///        instance's own field map only contains explicitly-written keys
///        until first read, see `dynamic::findField()`'s doc comment).
///        Generalized over @p ns (not hardcoded to any one namespace),
///        unlike a per-app helper of the same shape.
constexpr float kDegreesPerRadian = 57.29577951308232f;

dynamic_ptr find_class_prototype(key_t ns, key_t klass) {
  auto lp = dynamic::getRegistry().rlock();
  auto ns_it = lp->find(ns);
  if (ns_it == lp->end())
    return {};
  auto cls_it = ns_it->second.find(klass);
  return cls_it != ns_it->second.end() ? cls_it->second : dynamic_ptr{};
}

/// @brief Renders a `std::vector<float>` value as a comma-separated string
///        for editing in a plain InputText widget -- the fallback for a
///        vector field with no `ColorField` attribute.
std::string floats_to_text(const std::vector<float>& values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i)
      out += ", ";
    out += std::to_string(values[i]);
  }
  return out;
}

/// @brief Inverse of `floats_to_text()`: parses up to @p count comma-
///        separated floats from @p text, left-padding missing/unparsable
///        trailing components with `0.0f` rather than rejecting the whole
///        edit.
std::vector<float> text_to_floats(const std::string& text, size_t count) {
  std::vector<float> out(count, 0.0f);
  size_t pos = 0;
  for (size_t i = 0; i < count && pos <= text.size(); ++i) {
    size_t comma = text.find(',', pos);
    std::string token = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    try {
      out[i] = std::stof(token);
    } catch (const std::exception&) {
      // Leave this component at its 0.0f default.
    }
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return out;
}

/// @brief One reflected field ready to become a table row: its key, the
///        prototype field carrying its type/attributes, and its sort key
///        (Order::priority() if present, else 0 -- ties preserve
///        declaration/hash order via std::stable_sort).
struct reflected_field {
  key_t field_key;
  const field* proto_field;
  int32_t sort_key;
  size_t declaration_index;
};

/// @brief Walks @p target's full PARENT chain (most-derived first in the
///        returned vector, matching the merge-by-key rule below), merging
///        each level's own fields by key -- a derived level's field
///        description wins on a collision, matching bison's own field
///        override semantics -- and returns the visible ones (skipping
///        CLASS/PARENT/NAMESPACE and anything tagged Hidden/Obsolete),
///        sorted per reflected_field::sort_key.
std::vector<reflected_field> reflect_fields(const dynamic& target) {
  key_t ns = target.as<key_t>(dynamic::NAMESPACE);
  key_t klass = target.as<key_t>(dynamic::CLASS);

  std::vector<dynamic_ptr> chain; // most-derived first
  for (key_t cur = klass; cur.id != 0U;) {
    auto proto = find_class_prototype(ns, cur);
    if (!proto)
      break;
    chain.push_back(proto);
    cur = proto->as<key_t>(dynamic::PARENT);
  }

  std::vector<key_t> order;
  std::unordered_map<key_t, const field*, key_t, key_t> by_key;
  for (auto& proto : chain) {
    proto->forEach([&](key_t field_key, const field& f) {
      if (field_key == dynamic::CLASS || field_key == dynamic::PARENT || field_key == dynamic::NAMESPACE)
        return;
      if (!by_key.count(field_key))
        order.push_back(field_key);
      by_key[field_key] = &f; // most-derived (processed first) wins
    });
  }

  std::vector<reflected_field> visible;
  for (size_t i = 0; i < order.size(); ++i) {
    const field* f = by_key[order[i]];
    if (f->findAttribute<Hidden>() || f->findAttribute<Obsolete>())
      continue;
    int32_t sort_key = 0;
    if (const auto* o = f->findAttribute<Order>())
      sort_key = o->priority();
    visible.push_back(reflected_field{order[i], f, sort_key, i});
  }
  std::stable_sort(visible.begin(), visible.end(), [](const reflected_field& a, const reflected_field& b) {
    return a.sort_key < b.sort_key;
  });
  return visible;
}

/// @brief Reference display text for a dynamic_ptr field's current value:
///        its "path"/"name" string field if present, else "(none)".
std::string reference_label(const dynamic_ptr& ref) {
  if (!ref)
    return "(none)";
  if (auto* p = ref->findField<std::string>("path"_key))
    return *p;
  if (auto* n = ref->findField<std::string>("name"_key))
    return *n;
  return "(unnamed)";
}

} // namespace

// ── object_inspector ─────────────────────────────────────────────────────────

object_inspector::object_inspector(dynamic&& base) : cloneable_ui_element(std::move(base)) {}

void object_inspector::release(context& s) {
  for (key_t id : built_ids_)
    s.objects.erase(id.id);
  built_ids_.clear();
  for (const auto& path : built_paths_)
    s.ui_objects.erase(path);
  built_paths_.clear();
  table_id_ = key_t{};
  description_label_.reset();
  row_field_order_.clear();
  value_widgets_.clear();
  angle_widgets_.clear();
  drop_widgets_.clear();
  menu_widgets_.clear();
}

context& object_inspector::require_dispatch_session() {
  if (!detail::current_context)
    throw std::logic_error("wish: object_inspector method called outside RMI dispatch");
  return *detail::current_context;
}

wish::ui_element_ptr object_inspector::stamp(context& s, key_t klass, const std::string& path) {
  if (!ctx_)
    throw std::logic_error("wish: object_inspector::init() was never called");
  ui_element_ptr elem = ui_element_ptr::create("wish"_key, klass);
  key_t id = rmi::shared::generate_id();
  ctx_->put_object(id, elem);
  elem["__wish_id"_key] = id;
  built_ids_.push_back(id);
  s.ui_objects[path] = elem;
  built_paths_.push_back(path);
  return elem;
}

void object_inspector::set_target(context& s, dynamic_ptr target) {
  release(s);
  target_ = std::move(target);

  static std::atomic<uint32_t> counter{0};
  base_path_ = "__object_inspector_" + std::to_string(counter.fetch_add(1));

  auto table = stamp(s, "Table"_key, base_path_ + ".table");
  table["columns"_key] = int32_t{2};
  table["headers"_key] = false;
  auto* label_ratio_f = findField<float>("label_ratio"_key);
  const float label_ratio = label_ratio_f ? std::clamp(*label_ratio_f, 0.0f, 0.9f) : 0.0f;
  auto col_field = stamp(s, "TableColumn"_key, base_path_ + ".table.col_field");
  col_field["label"_key] = std::string{"Field"};
  auto col_value = stamp(s, "TableColumn"_key, base_path_ + ".table.col_value");
  col_value["label"_key] = std::string{"Value"};
  col_value["flags"_key] = int32_t{8}; // ImGuiTableColumnFlags_WidthStretch
  if (label_ratio > 0.0f) {
    // Both columns stretch in proportion (label_ratio : 1 - label_ratio),
    // user-resizable: ImGuiTableFlags_RowBg | Resizable | SizingStretchProp.
    table["flags"_key] = int32_t{64 | 1 | (3 << 13)};
    col_field["flags"_key] = int32_t{8};
    col_field["init_width"_key] = label_ratio;
    col_value["init_width"_key] = 1.0f - label_ratio;
  } else {
    // ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit -- a mixed
    // fixed-width/stretch column layout (Field fixed, Value stretch) needs
    // an explicit sizing policy on the table, or ImGui's TableSetupColumn()
    // asserts the moment a column supplies a non-zero init_width (see
    // imgui_tables.cpp's TableSetupColumnApply()). Matches the flag
    // combination file_dialog.cpp's file_table already uses.
    table["flags"_key] = int32_t{64 | 8192};
    col_field["flags"_key] = int32_t{16}; // ImGuiTableColumnFlags_WidthFixed
    col_field["init_width"_key] = 140.0f;
  }
  table_id_ = wish_id_of(table);

  auto table_children = dynamic_ptr{key_t{0U}, {}};
  size_t idx = 0;
  (*table_children)[idx++] = dynamic_ptr{col_field};
  (*table_children)[idx++] = dynamic_ptr{col_value};

  auto* read_only_f = findField<bool>("read_only"_key);
  bool read_only = read_only_f && *read_only_f;
  auto* show_description_f = findField<bool>("show_description_panel"_key);
  bool show_description_panel = !show_description_f || *show_description_f;

  if (target_) {
    for (const auto& rf : reflect_fields(*target_)) {
      std::string display_name;
      if (const auto* dn = rf.proto_field->findAttribute<DisplayName>())
        display_name = dn->name();
      else
        display_name = "#" + std::to_string(rf.field_key.id);

      std::string row_path = base_path_ + ".table.row" + std::to_string(row_field_order_.size());
      auto row = stamp(s, "TableRow"_key, row_path);

      auto name_label = stamp(s, "Label"_key, row_path + ".name");
      name_label["text"_key] = display_name;
      // The field's description is always a hover tooltip on its name (the
      // Godot/Unity convention), with or without the description panel.
      if (const auto* d = rf.proto_field->findAttribute<Description>(); d && !d->text().empty())
        name_label["tooltip"_key] = d->text();

      wish::ui_element_ptr value_widget;
      bool editable = true;
      bool drop_target = false;

      const field& proto_field = *rf.proto_field;
      field& live = (*target_)[rf.field_key]; // resolves + caches attrs onto the instance

      if (read_only && (proto_field.is<bool>() || proto_field.is<int32_t>() || proto_field.is<float>() ||
                        proto_field.is<std::string>() || proto_field.is<std::vector<float>>())) {
        // read_only skips every otherwise-editable widget type in favor of a
        // plain Label -- field::get_as<std::string>() already formats an
        // Enum/EnumFlags int32 by name rather than raw index, so this is
        // strictly more readable than the editable widgets it replaces, not
        // just a disabled version of them.
        editable = false;
        value_widget = stamp(s, "Label"_key, row_path + ".value");
        value_widget["text"_key] = proto_field.is<std::vector<float>>()
            ? floats_to_text(live.get_as<std::vector<float>>())
            : live.get_as<std::string>();
      } else if (proto_field.is<bool>()) {
        value_widget = stamp(s, "Checkbox"_key, row_path + ".value");
        value_widget["label"_key] = std::string{};
        value_widget["value"_key] = live.as<bool>();
      } else if (proto_field.is<int32_t>()) {
        if (const auto* e = proto_field.findAttribute<Enum>()) {
          int32_t current = live.as<int32_t>();
          std::string items;
          int32_t selected = 0;
          const auto& entries = e->entries();
          for (size_t i = 0; i < entries.size(); ++i) {
            items += entries[i].first;
            items += '\n';
            if (entries[i].second == current)
              selected = static_cast<int32_t>(i);
          }
          value_widget = stamp(s, "Combo"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["items"_key] = items;
          value_widget["value"_key] = selected;
          value_widget["width"_key] = -1.0f;
        } else if (proto_field.findAttribute<EnumFlags>()) {
          value_widget = stamp(s, "InputText"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["value"_key] = live.get_as<std::string>();
          value_widget["width"_key] = -1.0f;
        } else if (const auto* r = proto_field.findAttribute<Range>()) {
          value_widget = stamp(s, "SliderInt"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["value"_key] = live.as<int32_t>();
          value_widget["min"_key] = static_cast<int32_t>(r->min());
          value_widget["max"_key] = static_cast<int32_t>(r->max());
          value_widget["width"_key] = -1.0f;
        } else {
          value_widget = stamp(s, "InputInt"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["value"_key] = live.as<int32_t>();
          value_widget["width"_key] = -1.0f;
        }
      } else if (proto_field.is<float>()) {
        // Every float is a drag widget; the field's reflection attributes pick
        // the clamp (Range) and the drag step (Step). An Angle field is shown
        // in degrees (stored in radians).
        const bool angle = proto_field.findAttribute<Angle>() != nullptr;
        const auto* range = proto_field.findAttribute<Range>();
        const auto* step = proto_field.findAttribute<Step>();
        const float scale = angle ? kDegreesPerRadian : 1.0f;
        value_widget = stamp(s, "DragFloat"_key, row_path + ".value");
        value_widget["label"_key] = std::string{};
        value_widget["value"_key] = live.as<float>() * scale;
        if (range) {
          value_widget["min"_key] = static_cast<float>(range->min()) * scale;
          value_widget["max"_key] = static_cast<float>(range->max()) * scale;
        }
        // Default step: 0.5 degrees, else 1/200 of the range, else 0.1.
        float speed = angle ? 0.5f : 0.1f;
        if (step)
          speed = static_cast<float>(step->step()) * scale;
        else if (range && !angle && range->max() > range->min())
          speed = static_cast<float>((range->max() - range->min()) / 200.0);
        value_widget["speed"_key] = speed;
        value_widget["format"_key] = angle ? std::string{"%.1f\xC2\xB0"} : std::string{"%.3f"};
        value_widget["width"_key] = -1.0f;
        if (angle)
          angle_widgets_.insert(wish_id_of(value_widget));
      } else if (proto_field.is<std::string>()) {
        value_widget = stamp(s, "InputText"_key, row_path + ".value");
        value_widget["label"_key] = std::string{};
        value_widget["value"_key] = live.as<std::string>();
        value_widget["width"_key] = -1.0f;
        if (const auto* m = proto_field.findAttribute<Multiline>()) {
          value_widget["multiline"_key] = true;
          value_widget["height"_key] = static_cast<float>(m->lines()) * 18.0f;
        }
      } else if (proto_field.is<std::vector<float>>()) {
        const auto current = live.get_as<std::vector<float>>();
        if (proto_field.findAttribute<ColorField>()) {
          // A full-width swatch opening the picker (no numeric inputs), as
          // in Godot/Unity: ImGuiColorEditFlags_NoInputs | AlphaPreviewHalf.
          value_widget = stamp(s, "ColorEdit"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["value"_key] = current;
          value_widget["flags"_key] = int32_t{32 | (1 << 18)};
          value_widget["width"_key] = -1.0f;
        } else if (current.size() >= 2 && current.size() <= 4) {
          value_widget = stamp(s, "DragFloatN"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["value"_key] = current;
          value_widget["width"_key] = -1.0f;
        } else {
          value_widget = stamp(s, "InputText"_key, row_path + ".value");
          value_widget["label"_key] = std::string{};
          value_widget["value"_key] = floats_to_text(live.get_as<std::vector<float>>());
          value_widget["width"_key] = -1.0f;
        }
      } else if (proto_field.is<dynamic_ptr>()) {
        editable = false;
        value_widget = stamp(s, "Button"_key, row_path + ".value");
        value_widget["label"_key] = reference_label(live.as<dynamic_ptr>());
        value_widget["width"_key] = -1;
        if (const auto* dt = read_only ? nullptr : proto_field.findAttribute<DropTarget>()) {
          drop_target = true;
          value_widget["drag_type"_key] = std::string{};
          value_widget["drop_type"_key] = dt->drop_type();
        }
      } else {
        editable = false;
        std::string text;
        if (proto_field.is<key_t>())
          text = "#" + std::to_string(live.as<key_t>().id);
        else if (proto_field.is<hash_t>())
          text = "#" + std::to_string(live.as<hash_t>());
        else
          text = "(unsupported)";
        value_widget = stamp(s, "Label"_key, row_path + ".value");
        value_widget["text"_key] = text;
      }

      if (editable)
        value_widgets_[wish_id_of(value_widget)] = rf.field_key;
      if (drop_target)
        drop_widgets_[wish_id_of(value_widget)] = rf.field_key;

      auto row_children = dynamic_ptr{key_t{0U}, {}};
      (*row_children)[size_t{0}] = dynamic_ptr{name_label};
      (*row_children)[size_t{1}] = dynamic_ptr{value_widget};

      // The owning app's per-field decoration (set_field_decorator()).
      if (decorator_) {
        const field_decoration deco = decorator_(rf.field_key);
        if (!deco.name_color.empty())
          name_label["text_color"_key] = deco.name_color;
        if (!deco.tooltip_note.empty()) {
          std::string tip = name_label->get_as<std::string>("tooltip"_key, std::string{});
          name_label["tooltip"_key] = tip.empty() ? deco.tooltip_note : tip + "\n" + deco.tooltip_note;
        }
        if (!deco.menu.empty()) {
          // A TableRow renders one ContextMenu child as the row's own
          // right-click menu (render_table()).
          auto menu = stamp(s, "ContextMenu"_key, row_path + ".menu");
          auto items = dynamic_ptr{key_t{0U}, {}};
          int32_t mi = 0;
          for (const auto& [action, label] : deco.menu) {
            auto item = stamp(s, "MenuItem"_key, row_path + ".menu.i" + std::to_string(mi));
            item["label"_key] = label;
            item["order"_key] = mi;
            menu_widgets_[wish_id_of(item)] = field_menu_action{rf.field_key, action};
            (*items)[static_cast<size_t>(mi++)] = dynamic_ptr{item};
          }
          menu["children"_key] = items;
          menu->refresh_children_order();
          (*row_children)[size_t{2}] = dynamic_ptr{menu};
        }
      }
      row["children"_key] = row_children;

      (*table_children)[idx++] = dynamic_ptr{row};
      row_field_order_.push_back(rf.field_key);
    }
  }

  table["children"_key] = table_children;

  if (show_description_panel) {
    description_label_ = stamp(s, "Label"_key, base_path_ + ".description");
    description_label_["wrap"_key] = true;
    description_label_["text"_key] = std::string{"Select a field to see its description."};
  }

  auto* existing = findField<dynamic_ptr>("children"_key);
  dynamic_ptr self_children = (existing && *existing) ? *existing : dynamic_ptr{key_t{0U}, {}};
  self_children->clear();
  (*self_children)[size_t{0}] = dynamic_ptr{table};
  if (show_description_panel)
    (*self_children)[size_t{1}] = dynamic_ptr{description_label_};
  (*this)["children"_key] = self_children;
}

void object_inspector::handle_row_event(key_t widget_id, key_t event_name, const dynamic& payload) {
  if (widget_id.id != table_id_.id || !description_label_)
    return;
  if (event_name != "row_selected"_key && event_name != "row_activated"_key)
    return;
  int32_t index = payload.get_as<int32_t>("index"_key, -1);
  if (index < 0 || static_cast<size_t>(index) >= row_field_order_.size())
    return;

  std::string desc;
  if (target_) {
    key_t field_name = row_field_order_[static_cast<size_t>(index)];
    if (const auto* d = (*target_)[field_name].findAttribute<Description>())
      desc = d->text();
  }
  description_label_["text"_key] = desc.empty() ? "(no description)" : desc;
}

std::optional<object_inspector::field_edit> object_inspector::handle_changed(
    key_t widget_id, const dynamic& payload) const {
  auto it = value_widgets_.find(widget_id);
  if (it == value_widgets_.end() || !target_)
    return std::nullopt;
  key_t field_name = it->second;
  auto& current = (*target_)[field_name];

  dynamic new_value;
  if (current.is<int32_t>() && (current.findAttribute<Enum>() || current.findAttribute<EnumFlags>())) {
    // Combo (Enum) reports {value: index, text: string}; InputText
    // (EnumFlags) reports {value: string} -- both round-trip through
    // field::operator=(std::string)'s existing Enum/EnumFlags coercion.
    std::string text = payload.findField("text"_key) != nullptr ? payload.as<std::string>("text"_key)
                                                                  : payload.as<std::string>("value"_key);
    new_value[field_name] = text;
  } else if (current.is<bool>()) {
    new_value[field_name] = payload.as<bool>("value"_key);
  } else if (current.is<int32_t>()) {
    new_value[field_name] = payload.as<int32_t>("value"_key);
  } else if (current.is<float>()) {
    float v = payload.as<float>("value"_key);
    new_value[field_name] = angle_widgets_.count(widget_id) ? v / kDegreesPerRadian : v;
  } else if (current.is<std::string>()) {
    new_value[field_name] = payload.as<std::string>("value"_key);
  } else if (current.is<std::vector<float>>()) {
    const auto* v = payload.findField("value"_key);
    if (v && v->is<std::vector<float>>()) // ColorEdit, DragFloatN
      new_value[field_name] = v->as<std::vector<float>>();
    else
      new_value[field_name] =
          text_to_floats(payload.as<std::string>("value"_key), current.get_as<std::vector<float>>().size());
  } else {
    return std::nullopt;
  }
  return field_edit{field_name, std::move(new_value)};
}

std::optional<object_inspector::field_drop> object_inspector::handle_dropped(
    key_t widget_id, const dynamic& payload) const {
  auto it = drop_widgets_.find(widget_id);
  if (it == drop_widgets_.end())
    return std::nullopt;
  return field_drop{it->second, payload.as<std::string>("payload"_key)};
}

std::optional<object_inspector::field_menu_action> object_inspector::handle_menu_clicked(key_t widget_id) const {
  auto it = menu_widgets_.find(widget_id);
  if (it == menu_widgets_.end())
    return std::nullopt;
  return it->second;
}

dynamic object_inspector::do_construct(const dynamic& params) {
  dynamic_ptr target;
  if (const auto* t = params.findField("target"_key); t && t->is<dynamic_ptr>())
    target = t->as<dynamic_ptr>();
  set_target(require_dispatch_session(), std::move(target));
  return dynamic{};
}

dynamic object_inspector::do_set_target(const dynamic& params) {
  set_target(require_dispatch_session(), params.as<dynamic_ptr>("target"_key));
  return dynamic{};
}

// ── registration ──────────────────────────────────────────────────────────────

void register_object_inspector() {
  auto proto = dynamic_ptr{"ObjectInspector"_key, {}};
  proto->addField(
      "target"_key,
      field{
          dynamic_ptr{},
          attr<DisplayName>("Target"),
          attr<Description>("The object whose fields this inspector reflects over and edits. "
                            "A plain set() on this field alone does NOT rebuild the table -- "
                            "call the set_target() method (or instantiate with target in the "
                            "construct params), which does."),
          attr<Category>("Content")});

  proto->addField(
      "read_only"_key,
      field{
          false,
          attr<DisplayName>("Read Only"),
          attr<Description>("When true, every field renders as a plain read-only Label instead "
                            "of its normal type-appropriate editable widget, and dynamic_ptr "
                            "fields are never drop targets even if DropTarget-attributed."),
          attr<Category>("Behavior")});

  proto->addField(
      "label_ratio"_key,
      field{
          0.0f,
          attr<DisplayName>("Label Ratio"),
          attr<Description>("0 (default): the name column is a fixed 140 px. Between 0 and 0.9: both "
                            "columns stretch, the name column taking this fraction of the width, and "
                            "the user can resize them (Godot/Unity-style inspector). Read by "
                            "set_target()."),
          attr<Category>("Appearance")});
  proto->addField(
      "show_description_panel"_key,
      field{
          true,
          attr<DisplayName>("Show Description Panel"),
          attr<Description>("When false, the description Label below the field table -- which "
                            "normally shows the selected row's field description -- is omitted "
                            "entirely."),
          attr<Category>("Appearance")});

  proto->addMethod(
      "__construct"_key, method{[](dynamic& s, const dynamic& p) -> dynamic {
        return static_cast<object_inspector&>(s).do_construct(p);
      }});

  auto set_target_in = std::make_shared<dynamic>();
  set_target_in->addField("target"_key, field{dynamic_ptr{}, attr<DisplayName>("target")});
  proto->addMethod(
      "set_target"_key,
      method{
          [](dynamic& s, const dynamic& p) -> dynamic { return static_cast<object_inspector&>(s).do_set_target(p); },
          dynamic_ptr{set_target_in},
          nullptr,
          attr<DisplayName>("set_target")});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Object Inspector"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Unity/Visual-Studio-style property table: reflects over target's registered "
      "class (via bison attributes) to show one row per visible field with a "
      "type-appropriate editor, plus a description panel for the selected row. "
      "See object_inspector.hpp for the field -> widget dispatch table."));

  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<object_inspector>("wish"_key, "ObjectInspector"_key));
}

} // namespace bdg::wish
