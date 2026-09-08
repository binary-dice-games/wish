// MIT License © 2026 Binary Dice Games
/// @file mi_parser.cpp
/// @brief Implementation of the GDB/LLDB MI output-record parser. See the
///        header for the grammar this follows.
#include "mi_parser.hpp"

#include <cctype>

namespace bdg::wish::dbg {

namespace {

/// @brief Recursive-descent cursor over one MI line.
class cursor {
 public:
  explicit cursor(std::string_view s) : s_(s) {}

  bool eof() const {
    return pos_ >= s_.size();
  }
  char peek() const {
    return pos_ < s_.size() ? s_[pos_] : '\0';
  }
  char next() {
    return pos_ < s_.size() ? s_[pos_++] : '\0';
  }
  bool consume(char c) {
    if (peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  /// @brief Reads a MI variable name: everything up to `=`, `,`, or a
  ///        closing bracket. MI names are unquoted identifiers
  ///        (`name-with-dashes`).
  std::string read_name() {
    size_t start = pos_;
    while (!eof()) {
      char c = peek();
      if (c == '=' || c == ',' || c == '}' || c == ']' || c == '{' || c == '[')
        break;
      ++pos_;
    }
    return std::string(s_.substr(start, pos_ - start));
  }

  /// @brief Reads a `"..."` C-string and returns its unescaped body.
  ///        Assumes `peek() == '"'`.
  std::string read_cstring() {
    std::string out;
    if (!consume('"'))
      return out;
    while (!eof()) {
      char c = next();
      if (c == '"')
        break;
      if (c == '\\' && !eof()) {
        char e = next();
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'a': out += '\a'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'v': out += '\v'; break;
          case '0': out += '\0'; break;
          case '\\': out += '\\'; break;
          case '"': out += '"'; break;
          default: out += e; break; // Unknown escape: keep the char verbatim.
        }
        continue;
      }
      out += c;
    }
    return out;
  }

  mi_value read_value() {
    mi_value v;
    char c = peek();
    if (c == '"') {
      v.type = mi_value::kind::string;
      v.str = read_cstring();
    } else if (c == '{') {
      v.type = mi_value::kind::tuple;
      ++pos_;
      read_members(v, '}');
      consume('}');
    } else if (c == '[') {
      v.type = mi_value::kind::list;
      ++pos_;
      read_list(v);
      consume(']');
    } else {
      // Bare token (rare: some emitters print `value=addr` unquoted).
      v.type = mi_value::kind::string;
      size_t start = pos_;
      while (!eof() && peek() != ',' && peek() != '}' && peek() != ']')
        ++pos_;
      v.str = std::string(s_.substr(start, pos_ - start));
    }
    return v;
  }

  /// @brief Reads `name=value` members until @p close, filling `out.items`.
  void read_members(mi_value& out, char close) {
    while (!eof() && peek() != close) {
      std::string name = read_name();
      if (!consume('='))
        break;
      out.items.emplace_back(std::move(name), read_value());
      if (!consume(','))
        break;
    }
  }

  /// @brief Reads a `[...]` body: either bare values or `name=value`
  ///        results (MI permits both; GDB uses result-style for e.g.
  ///        `stack=[frame={...},frame={...}]`).
  void read_list(mi_value& out) {
    while (!eof() && peek() != ']') {
      size_t save = pos_;
      std::string name = read_name();
      if (!name.empty() && peek() == '=') {
        ++pos_;
        out.items.emplace_back(std::move(name), read_value());
      } else {
        pos_ = save;
        out.values.push_back(read_value());
      }
      if (!consume(','))
        break;
    }
  }

  /// @brief Reads top-level `,name=value` results into @p results.
  void read_result_list(std::vector<std::pair<std::string, mi_value>>& results) {
    while (consume(',')) {
      std::string name = read_name();
      if (!consume('='))
        break;
      results.emplace_back(std::move(name), read_value());
    }
  }

 private:
  std::string_view s_;
  size_t pos_{0};
};

} // namespace

std::string mi_unescape(std::string_view quoted_body) {
  // Wrap in quotes so read_cstring() sees a well-formed literal.
  std::string wrapped = "\"";
  wrapped += std::string(quoted_body);
  wrapped += "\"";
  cursor c(wrapped);
  return c.read_cstring();
}

const mi_value* mi_value::find(std::string_view key) const {
  for (const auto& [name, val] : items) {
    if (name == key)
      return &val;
  }
  return nullptr;
}

std::string mi_value::get(std::string_view key) const {
  const mi_value* v = find(key);
  return v && v->type == kind::string ? v->str : std::string();
}

const mi_value* mi_record::find(std::string_view key) const {
  for (const auto& [name, val] : results) {
    if (name == key)
      return &val;
  }
  return nullptr;
}

std::string mi_record::get(std::string_view key) const {
  const mi_value* v = find(key);
  return v && v->type == mi_value::kind::string ? v->str : std::string();
}

std::optional<mi_record> parse_mi_line(std::string_view line) {
  // Strip a trailing CR/LF pair if the caller passed the raw line.
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.remove_suffix(1);
  if (line.empty())
    return std::nullopt;

  mi_record rec;

  // Optional leading token (digits) precedes the record-kind character.
  size_t i = 0;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
    ++i;
  rec.token = std::string(line.substr(0, i));
  line.remove_prefix(i);
  if (line.empty()) {
    // A bare token with nothing after it isn't a valid record.
    return std::nullopt;
  }

  char lead = line.front();
  line.remove_prefix(1);
  cursor c(line);

  switch (lead) {
    case '^':
      rec.type = mi_record::kind::result;
      rec.klass = c.read_name();
      c.read_result_list(rec.results);
      return rec;
    case '*':
      rec.type = mi_record::kind::exec_async;
      rec.klass = c.read_name();
      c.read_result_list(rec.results);
      return rec;
    case '+':
      rec.type = mi_record::kind::status_async;
      rec.klass = c.read_name();
      c.read_result_list(rec.results);
      return rec;
    case '=':
      rec.type = mi_record::kind::notify_async;
      rec.klass = c.read_name();
      c.read_result_list(rec.results);
      return rec;
    case '~':
      rec.type = mi_record::kind::console_stream;
      rec.klass = c.read_cstring();
      return rec;
    case '@':
      rec.type = mi_record::kind::target_stream;
      rec.klass = c.read_cstring();
      return rec;
    case '&':
      rec.type = mi_record::kind::log_stream;
      rec.klass = c.read_cstring();
      return rec;
    case '(': {
      // "(gdb) " / "(lldb) " idle prompt (usually with a trailing space).
      std::string rest(line);
      while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t'))
        rest.pop_back();
      if (!rest.empty() && rest.back() == ')') {
        rec.type = mi_record::kind::prompt;
        rec.klass = rest.substr(0, rest.size() - 1);
        return rec;
      }
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

} // namespace bdg::wish::dbg
