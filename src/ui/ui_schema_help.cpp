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

  // Dot-path tracking (matches ui_importer.cpp's build_ui_node/build_ui_children
  // convention exactly -- see scan_cursor_context's doc comment on why a
  // "children" value is an extra, otherwise-unmarked nesting level between a
  // parent element's frame and each of its named children's frames).
  bool is_children_wrapper = false; // this object/array is the value of a "children" key
  std::string owner_path; // children-wrapper only: the owning element's own path
  std::optional<std::string> own_path; // element frames only: this element's dot-path
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

// Determine the new frame's path-tracking fields from the stack as it stood
// just before this `{`/`[` was pushed (i.e. `stack.back()` is still the
// *parent* frame here). See the `frame` struct's own comment for the
// "children" wrapper concept this distinguishes.
frame make_frame(const std::vector<frame>& stack, bool is_object) {
  frame f;
  f.is_object = is_object;

  if (stack.empty()) {
    f.own_path = std::string{}; // document root
    return f;
  }

  const frame& parent = stack.back();
  if (parent.pending_key == "children") {
    // This new frame is the "children" wrapper itself -- not an addressable
    // element, but its own children need the owning element's path as a prefix.
    f.is_children_wrapper = true;
    f.owner_path = parent.own_path.value_or(std::string{});
    return f;
  }
  if (parent.is_children_wrapper) {
    // A direct entry of a "children" map/array: only a *named* entry (the
    // wrapper is itself an object, and a key just closed for it) gets a path,
    // matching import_json()'s own exclusion of unnamed/array-indexed children.
    if (parent.is_object && !parent.pending_key.empty()) {
      f.own_path =
          parent.owner_path.empty() ? parent.pending_key : (parent.owner_path + "." + parent.pending_key);
    }
    return f;
  }
  // A `{`/`[` value for some field other than "children" -- not a wish
  // schema-element position; own_path stays nullopt.
  return f;
}

// The dot-path of the element @p top represents for highlighting purposes:
// a children-wrapper frame (cursor positioned in the gap before any child is
// named yet) falls back to the *owning* element, so there's still a sensible
// target rather than nothing.
std::optional<std::string> element_path_of(const frame& top) {
  if (top.is_children_wrapper)
    return top.owner_path;
  return top.own_path;
}

// Small, bounded FORWARD lookahead for the frame's own "type" value, used
// only when the backward scan hasn't found one yet: the cursor is
// positioned before "type" was written in this object (right after its
// opening "{", on a blank line before any field, or before "type" if some
// other field happens to precede it). Scans only source[offset, end),
// skipping over sibling values via depth tracking, and stops the instant
// it would leave the current object/array ('}'/']' at depth 0) before
// finding "type" -- so content elsewhere in the document (possibly
// transiently invalid mid-edit -- the reason the main scan stays
// backward-only) can never leak in: this lookahead is self-limiting to
// whatever remains of the CURRENT frame's own, already-typed body.
std::string lookahead_type_value(std::string_view source, size_t offset) {
  bool in_str = false, escape = false;
  size_t string_start = 0;
  std::string pending_key;
  int depth = 0;

  for (size_t i = offset; i < source.size(); ++i) {
    char c = source[i];
    if (in_str) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_str = false;
        if (depth == 0) {
          std::string content{source.substr(string_start + 1, i - string_start - 1)};
          char prev = last_non_ws_before(source, string_start);
          if (prev == ':') {
            if (pending_key == "type")
              return content;
            pending_key.clear();
          } else {
            pending_key = content;
          }
        }
      }
      continue;
    }
    switch (c) {
      case '"':
        in_str = true;
        string_start = i;
        escape = false;
        break;
      case '{':
      case '[':
        ++depth;
        break;
      case '}':
      case ']':
        if (depth == 0)
          return {};
        --depth;
        break;
      default:
        break;
    }
  }
  return {};
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
        stack.push_back(make_frame(stack, /*is_object=*/true));
        break;
      case '[':
        stack.push_back(make_frame(stack, /*is_object=*/false));
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
      result.element_path = element_path_of(top);
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
    if (result.enclosing_type.empty())
      result.enclosing_type = lookahead_type_value(source, offset);
    result.element_path = element_path_of(top);
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

// ── YAML cursor-context scanner ────────────────────────────────────────────

namespace {

/// One open YAML block mapping. `indent` is the column of the `key:` line
/// that opened it (the document root uses -1). The path bookkeeping mirrors
/// the JSON `frame` exactly -- see `make_frame()` above.
struct yframe {
  int indent = -1;
  std::string type_value;
  std::vector<std::string> field_names;
  bool is_children_wrapper = false;
  std::string owner_path;              // children-wrapper: owning element's path
  std::optional<std::string> own_path; // element frames: this element's dot-path
};

std::optional<std::string> element_path_of(const yframe& f) {
  if (f.is_children_wrapper)
    return f.owner_path;
  return f.own_path;
}

int leading_indent(std::string_view line) {
  int n = 0;
  for (char c : line) {
    if (c == ' ' || c == '\t')
      ++n;
    else
      break;
  }
  return n;
}

std::string trim_ws(std::string_view s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string_view::npos)
    return {};
  size_t b = s.find_last_not_of(" \t\r");
  return std::string{s.substr(a, b - a + 1)};
}

// Drop a trailing `# ...` comment (a `#` at line start or preceded by
// whitespace, and not inside a quoted scalar).
std::string strip_inline_comment(std::string_view s) {
  bool in_single = false, in_double = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\'' && !in_double)
      in_single = !in_single;
    else if (c == '"' && !in_single)
      in_double = !in_double;
    else if (c == '#' && !in_single && !in_double && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
      return std::string{s.substr(0, i)};
  }
  return std::string{s};
}

std::string unquote_scalar(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
    return s.substr(1, s.size() - 2);
  return s;
}

struct yaml_kv {
  std::string key;
  bool has_colon = false;
  bool has_value = false;
  std::string value;
};

// Split a trimmed line body into `key: value`. A `:` only separates when
// followed by whitespace or end-of-line and not inside a quoted scalar --
// matching YAML's own rule, so a `:` inside a value (`format: %H:%M`) or a
// quoted key is not mistaken for the separator.
yaml_kv split_yaml_kv(const std::string& body) {
  yaml_kv r;
  bool in_single = false, in_double = false;
  for (size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    if (c == '\'' && !in_double)
      in_single = !in_single;
    else if (c == '"' && !in_single)
      in_double = !in_double;
    else if (c == ':' && !in_single && !in_double &&
             (i + 1 == body.size() || body[i + 1] == ' ' || body[i + 1] == '\t')) {
      r.has_colon = true;
      r.key = unquote_scalar(trim_ws(std::string_view{body}.substr(0, i)));
      std::string v = trim_ws(std::string_view{body}.substr(i + 1));
      if (!v.empty()) {
        r.has_value = true;
        r.value = v;
      }
      return r;
    }
  }
  r.key = unquote_scalar(trim_ws(body));
  return r;
}

// Strip leading `- ` sequence markers, bumping the effective content indent
// by 2 per marker. Returns the remaining body; @p indent and @p seq_entry
// are updated in place.
std::string strip_seq_markers(std::string body, int& indent, bool& seq_entry) {
  while (body == "-" || body.rfind("- ", 0) == 0) {
    seq_entry = true;
    indent += 2;
    body = body == "-" ? std::string{} : trim_ws(body.substr(2));
  }
  return body;
}

} // namespace

cursor_context scan_cursor_context_yaml(std::string_view source, text_pos cursor) {
  size_t offset = to_offset(source, cursor);
  std::string_view prefix = source.substr(0, offset);

  // Complete lines before the cursor, plus the (possibly partial) cursor line.
  std::vector<std::string_view> lines;
  size_t line_start = 0;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i] == '\n') {
      lines.push_back(prefix.substr(line_start, i - line_start));
      line_start = i + 1;
    }
  }
  std::string_view cursor_line = prefix.substr(line_start);

  std::vector<yframe> stack;
  stack.push_back(yframe{});
  stack.back().own_path = std::string{}; // document root

  auto pop_to = [&](int indent) {
    while (stack.size() > 1 && stack.back().indent >= indent)
      stack.pop_back();
  };

  for (std::string_view raw : lines) {
    std::string content = strip_inline_comment(raw);
    std::string body = trim_ws(content);
    if (body.empty() || body == "---" || body == "...")
      continue;
    int indent = leading_indent(content);
    bool seq_entry = false;
    body = strip_seq_markers(std::move(body), indent, seq_entry);
    if (body.empty())
      continue;

    pop_to(indent);
    yframe& parent = stack.back();

    yaml_kv kv = split_yaml_kv(body);
    if (kv.key.empty() && !kv.has_colon)
      continue;

    if (!seq_entry && !kv.key.empty())
      parent.field_names.push_back(kv.key);

    if (kv.has_colon && kv.has_value) {
      if (kv.key == "type")
        parent.type_value = unquote_scalar(kv.value);
      continue; // scalar entry -- opens no block
    }
    if (kv.has_colon && !kv.has_value) {
      yframe nf;
      nf.indent = indent;
      if (kv.key == "children") {
        nf.is_children_wrapper = true;
        nf.owner_path = parent.own_path.value_or(std::string{});
      } else if (parent.is_children_wrapper && !seq_entry) {
        nf.own_path = parent.owner_path.empty() ? kv.key : (parent.owner_path + "." + kv.key);
      }
      stack.push_back(std::move(nf));
    }
  }

  // ── Classify the cursor line ──
  int cur_indent = leading_indent(cursor_line);
  std::string typed =
      cursor_line.size() > static_cast<size_t>(cur_indent) ? std::string{cursor_line.substr(cur_indent)} : std::string{};
  {
    bool seq_entry = false;
    typed = strip_seq_markers(std::move(typed), cur_indent, seq_entry);
  }

  pop_to(cur_indent);
  const yframe& top = stack.back();

  cursor_context result;
  result.enclosing_type = top.type_value;
  result.element_path = element_path_of(top);

  // Bounded forward peek for a `type:` sibling of the cursor line not yet
  // reached: scan whole lines after the cursor, staying within the current
  // block (indent >= cur_indent), and take a `type:` at exactly cur_indent.
  if (result.enclosing_type.empty()) {
    size_t p = offset;
    while (p < source.size() && source[p] != '\n')
      ++p;
    while (p < source.size()) {
      ++p; // step past '\n'
      size_t ls = p;
      while (p < source.size() && source[p] != '\n')
        ++p;
      std::string lc = strip_inline_comment(source.substr(ls, p - ls));
      std::string lb = trim_ws(lc);
      if (lb.empty() || lb == "---" || lb == "...")
        continue;
      int li = leading_indent(lc);
      if (li < cur_indent)
        break;
      if (li == cur_indent) {
        yaml_kv q = split_yaml_kv(lb);
        if (q.key == "type" && q.has_value) {
          result.enclosing_type = unquote_scalar(q.value);
          break;
        }
      }
    }
  }

  yaml_kv cp = split_yaml_kv(typed);
  if (!cp.has_colon) {
    result.kind = cursor_context_kind::field_key;
    result.partial_text = trim_ws(typed);
    result.existing_field_names = top.field_names;
  } else {
    size_t cpos = typed.find(':');
    std::string after = cpos == std::string::npos ? std::string{} : trim_ws(typed.substr(cpos + 1));
    // Strip an opening quote (and a matching close, if already typed) so the
    // prefix filter sees the bare value text.
    if (!after.empty() && (after.front() == '"' || after.front() == '\'')) {
      char q = after.front();
      after.erase(after.begin());
      if (!after.empty() && after.back() == q)
        after.pop_back();
    }
    if (cp.key == "type") {
      result.kind = cursor_context_kind::type_value;
      result.partial_text = after;
    } else {
      result.kind = cursor_context_kind::field_value;
      result.field_name = cp.key;
      result.partial_text = after;
    }
  }
  return result;
}

} // namespace bdg::wish
