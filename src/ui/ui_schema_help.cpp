// MIT License © 2025 Binary Dice Games
/// @file ui_schema_help.cpp
/// @brief Implementation of the "wish" UI element class registry queries and
///        the JSON cursor-context scanner (see ui_schema_help.hpp).
#include <ui/ui_schema_help.hpp>

#include <cctype>
#include <set>

namespace bdg::wish {

using namespace bison;

namespace {

/// @brief Walk @p klass's own `PARENT` chain (starting from @p klass itself)
/// and report whether it reaches `"Element"_key`. Used to decide "is this a
/// real UI element type" (true) vs. "is this a `form` (Editor, MessageBox,
/// ...)" (false, `PARENT == 0` directly) -- see element.cpp's registration
/// of the shared "Element" root every real UI element class descends from.
bool reaches_element(const collection& col, key_t klass) {
  key_t cur = klass;
  // Bounded to guard against a (should-never-happen) cycle in the registry.
  for (int guard = 0; guard < 64; ++guard) {
    if (cur.id == static_cast<hash_t>("Element"_key))
      return true;
    auto it = col.find(cur);
    if (it == col.end() || !it->second)
      return false;
    key_t parent = it->second->get_as<key_t>(dynamic::PARENT, key_t{0U});
    if (parent.id == 0)
      return false;
    cur = parent;
  }
  return false;
}

/// @brief Resolve one field's metadata, or std::nullopt if its name was
/// never `_rkey`-registered (see ui_schema_help.hpp's doc comment on why a
/// missed conversion is silently omitted rather than fabricated).
std::optional<element_field_info> resolve_field(key_t key, const field& f) {
  auto name = lookup_registered_key_name(key.id);
  if (!name)
    return std::nullopt;

  element_field_info info;
  info.name = *name;
  if (const auto* dn = f.findAttribute<DisplayName>())
    info.display_name = dn->name();
  else
    info.display_name = info.name;
  if (const auto* d = f.findAttribute<Description>())
    info.description = d->text();
  if (const auto* c = f.findAttribute<Category>())
    info.category = c->name();
  info.required = f.findAttribute<Required>() != nullptr;
  if (const auto* r = f.findAttribute<Range>())
    info.range = std::make_pair(r->min(), r->max());
  if (const auto* s = f.findAttribute<Step>())
    info.step = s->step();
  if (const auto* ef = f.findAttribute<EnumFlags>()) {
    info.is_enum_flags = true;
    for (const auto& [name_str, value] : ef->entries())
      info.enum_values.push_back(name_str);
  } else if (const auto* e = f.findAttribute<Enum>()) {
    for (const auto& [name_str, value] : e->entries())
      info.enum_values.push_back(name_str);
  }
  return info;
}

/// @brief Build @p klass_key's full info: its own class-level DisplayName/
/// Description, plus every field from @p proto merged with every field
/// inherited from its PARENT chain up through (and including) Element --
/// most-derived definition wins on a name collision.
element_class_info build_class_info(const collection& col, key_t klass_key, const dynamic_ptr& proto) {
  element_class_info info;
  info.name = lookup_registered_key_name(klass_key.id).value_or(std::string{});
  if (const auto* cf = proto->findField(dynamic::CLASS)) {
    if (const auto* dn = cf->findAttribute<DisplayName>())
      info.display_name = dn->name();
    if (const auto* d = cf->findAttribute<Description>())
      info.description = d->text();
  }
  if (info.display_name.empty())
    info.display_name = info.name;

  std::set<std::string> seen;
  key_t cur = klass_key;
  const dynamic_ptr* cur_proto = &proto;
  for (int guard = 0; guard < 64; ++guard) {
    (*cur_proto)->forEach([&](key_t k, const field& f) {
      if (static_cast<hash_t>(k) == dynamic::CLASS || static_cast<hash_t>(k) == dynamic::PARENT ||
          static_cast<hash_t>(k) == dynamic::NAMESPACE)
        return;
      auto resolved = resolve_field(k, f);
      if (!resolved || !seen.insert(resolved->name).second)
        return; // unresolvable name, or a more-derived class already defined it
      info.fields.push_back(std::move(*resolved));
    });
    if (cur.id == static_cast<hash_t>("Element"_key))
      break;
    key_t parent = (*cur_proto)->get_as<key_t>(dynamic::PARENT, key_t{0U});
    if (parent.id == 0)
      break;
    auto it = col.find(parent);
    if (it == col.end() || !it->second)
      break;
    cur = parent;
    cur_proto = &it->second;
  }
  return info;
}

const collection* wish_collection(const namespace_map& ns_map) {
  auto it = ns_map.find("wish"_key);
  return it == ns_map.end() ? nullptr : &it->second;
}

} // namespace

std::vector<element_class_info> enumerate_ui_element_classes() {
  std::vector<element_class_info> result;
  auto lp = dynamic::getRegistry().rlock();
  const auto* col = wish_collection(*lp);
  if (!col)
    return result;

  for (const auto& [klass_key, proto] : *col) {
    if (!proto || klass_key.id == static_cast<hash_t>("Element"_key))
      continue;
    if (!reaches_element(*col, proto->get_as<key_t>(dynamic::PARENT, key_t{0U})))
      continue;
    if (!lookup_registered_key_name(klass_key.id))
      continue;
    result.push_back(build_class_info(*col, klass_key, proto));
  }
  return result;
}

std::optional<element_class_info> find_ui_element_class(const std::string& type_name) {
  key_t klass_key{type_name};
  auto lp = dynamic::getRegistry().rlock();
  const auto* col = wish_collection(*lp);
  if (!col)
    return std::nullopt;

  auto it = col->find(klass_key);
  if (it == col->end() || !it->second || klass_key.id == static_cast<hash_t>("Element"_key))
    return std::nullopt;
  if (!reaches_element(*col, it->second->get_as<key_t>(dynamic::PARENT, key_t{0U})))
    return std::nullopt;

  return build_class_info(*col, klass_key, it->second);
}

// ── JSON cursor-context scanner ─────────────────────────────────────────────

namespace {

/// @brief Convert a (line, column) position into a flat byte offset into
/// @p source. Clamps to the nearest valid offset if @p cursor names a
/// position past the end of its line or of @p source itself.
size_t to_offset(std::string_view source, text_pos cursor) {
  size_t line = 0, col = 0;
  for (size_t i = 0; i < source.size(); ++i) {
    if (line == cursor.line && col == cursor.column)
      return i;
    if (source[i] == '\n') {
      if (line == cursor.line)
        return i; // cursor.column was past this (shorter) line's end
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
  return source.size();
}

/// One `{`/`[` nesting level. Only `{` levels populate the object-specific
/// fields below; `[` levels exist purely for depth bookkeeping.
struct frame {
  bool is_object = false;
  std::string type_value; // this object's own "type" value, once seen
  std::vector<std::string> field_names; // keys seen so far in this object
  std::string pending_key; // most recently closed key awaiting its value
};

char last_non_ws_before(std::string_view source, size_t pos) {
  while (pos > 0) {
    --pos;
    char c = source[pos];
    if (!std::isspace(static_cast<unsigned char>(c)))
      return c;
  }
  return '\0';
}

} // namespace

cursor_context scan_cursor_context(std::string_view source, text_pos cursor) {
  size_t offset = to_offset(source, cursor);

  std::vector<frame> stack;
  bool in_string = false;
  bool escape = false;
  size_t string_start = 0;
  enum class str_role { none, key, value } role = str_role::none;

  for (size_t i = 0; i < offset; ++i) {
    char c = source[i];

    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        std::string content{source.substr(string_start + 1, i - string_start - 1)};
        in_string = false;
        if (!stack.empty() && stack.back().is_object) {
          frame& top = stack.back();
          if (role == str_role::key) {
            top.pending_key = content;
            top.field_names.push_back(content);
          } else if (role == str_role::value) {
            if (top.pending_key == "type")
              top.type_value = content;
            top.pending_key.clear();
          }
        }
        role = str_role::none;
      }
      continue;
    }

    switch (c) {
      case '"': {
        in_string = true;
        string_start = i;
        char prev = last_non_ws_before(source, i);
        role = (prev == '{' || prev == ',') ? str_role::key : (prev == ':') ? str_role::value : str_role::none;
        break;
      }
      case '{':
        stack.push_back(frame{.is_object = true});
        break;
      case '[':
        stack.push_back(frame{.is_object = false});
        break;
      case '}':
      case ']':
        if (!stack.empty())
          stack.pop_back();
        break;
      case ',':
        if (!stack.empty())
          stack.back().pending_key.clear();
        break;
      default:
        break;
    }
  }

  cursor_context result;

  if (in_string) {
    result.partial_text = std::string(source.substr(string_start + 1, offset - string_start - 1));
    if (!stack.empty() && stack.back().is_object) {
      const frame& top = stack.back();
      result.enclosing_type = top.type_value;
      if (role == str_role::key) {
        result.kind = cursor_context_kind::field_key;
        result.existing_field_names = top.field_names;
      } else if (role == str_role::value) {
        if (top.pending_key == "type") {
          result.kind = cursor_context_kind::type_value;
        } else if (!top.pending_key.empty()) {
          result.kind = cursor_context_kind::field_value;
          result.field_name = top.pending_key;
        }
      }
    }
    return result;
  }

  if (!stack.empty() && stack.back().is_object) {
    const frame& top = stack.back();
    result.enclosing_type = top.type_value;
    char prev = last_non_ws_before(source, offset);
    if (prev == '{' || prev == ',') {
      result.kind = cursor_context_kind::field_key;
      result.existing_field_names = top.field_names;
    } else if (prev == ':') {
      result.kind = cursor_context_kind::field_value;
      result.field_name = top.pending_key;
    }
  }
  return result;
}

} // namespace bdg::wish
