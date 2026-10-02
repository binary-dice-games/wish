// MIT License © 2026 Binary Dice Games
/// @file pip_parsers.cpp
/// @brief Implementation of the pip output / argument helpers.
#include "pip_parsers.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace bdg::wish::pip {

namespace {

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  auto b = s.find_first_not_of(ws);
  if (b == std::string::npos)
    return {};
  auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

void skip_ws(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
    ++i;
}

void append_utf8(std::string& out, unsigned long cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Reads the JSON string starting at s[i] == '"'; leaves i past the closing
// quote. @return false on an unterminated string.
bool read_string(const std::string& s, size_t& i, std::string& out) {
  out.clear();
  for (++i; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '"') {
      ++i;
      return true;
    }
    if (ch != '\\') {
      out += ch;
      continue;
    }
    if (++i >= s.size())
      return false;
    switch (s[i]) {
    case 'n':
      out += '\n';
      break;
    case 't':
      out += '\t';
      break;
    case 'r':
      out += '\r';
      break;
    case 'b':
      out += '\b';
      break;
    case 'f':
      out += '\f';
      break;
    case 'u': {
      if (i + 4 >= s.size())
        return false;
      // Basic Multilingual Plane only; a surrogate pair comes out as two
      // (invalid) 3-byte sequences, which is fine for a display string.
      append_utf8(out, std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
      i += 4;
      break;
    }
    default:
      out += s[i]; // \" \\ \/
    }
  }
  return false;
}

// Skips the nested object / array starting at s[i]; leaves i past its end.
bool skip_nested(const std::string& s, size_t& i) {
  int depth = 0;
  std::string ignored;
  while (i < s.size()) {
    char ch = s[i];
    if (ch == '"') {
      if (!read_string(s, i, ignored))
        return false;
      continue;
    }
    if (ch == '{' || ch == '[')
      ++depth;
    else if (ch == '}' || ch == ']')
      --depth;
    ++i;
    if (depth == 0)
      return true;
  }
  return false;
}

// Reads one `{ "key": value, ... }` starting at s[i] == '{'.
bool read_object(const std::string& s, size_t& i, json_object& obj) {
  ++i;
  while (true) {
    skip_ws(s, i);
    if (i >= s.size())
      return false;
    if (s[i] == '}') {
      ++i;
      return true;
    }
    if (s[i] == ',') {
      ++i;
      continue;
    }
    std::string key;
    if (s[i] != '"' || !read_string(s, i, key))
      return false;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != ':')
      return false;
    ++i;
    skip_ws(s, i);
    if (i >= s.size())
      return false;
    if (s[i] == '"') {
      std::string value;
      if (!read_string(s, i, value))
        return false;
      obj[key] = std::move(value);
    } else if (s[i] == '{' || s[i] == '[') {
      if (!skip_nested(s, i))
        return false;
    } else {
      size_t start = i;
      while (i < s.size() && s[i] != ',' && s[i] != '}' && !std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
      obj[key] = s.substr(start, i - start);
    }
  }
}

// "  LABEL:   value" -> "value" when @p line starts with @p label.
bool labelled(const std::string& line, const char* label, std::string& out) {
  const std::string t = trim(line);
  if (t.rfind(label, 0) != 0)
    return false;
  out = trim(t.substr(std::string{label}.size()));
  return true;
}

} // namespace

std::vector<json_object> parse_json_objects(const std::string& text) {
  std::vector<json_object> out;
  size_t i = 0;
  skip_ws(text, i);
  if (i >= text.size() || text[i] != '[')
    return out;
  ++i;
  while (true) {
    skip_ws(text, i);
    if (i >= text.size() || text[i] == ']')
      return out;
    if (text[i] == ',') {
      ++i;
      continue;
    }
    json_object obj;
    if (text[i] != '{' || !read_object(text, i, obj))
      return out;
    out.push_back(std::move(obj));
  }
}

index_versions parse_index_versions(const std::string& text) {
  index_versions out;
  std::istringstream iss(text);
  std::string line;
  std::string list;
  while (std::getline(iss, line)) {
    if (labelled(line, "Available versions:", list)) {
      std::istringstream parts(list);
      std::string v;
      while (std::getline(parts, v, ',')) {
        v = trim(v);
        if (!v.empty())
          out.versions.push_back(v);
      }
    } else if (!labelled(line, "INSTALLED:", out.installed)) {
      labelled(line, "LATEST:", out.latest);
    }
  }
  return out;
}

bool is_safe_arg(const std::string& value) {
  return !value.empty() && value[0] != '-';
}

bool is_valid_package_name(const std::string& name) {
  auto alnum = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0; };
  if (name.empty() || !alnum(name.front()) || !alnum(name.back()))
    return false;
  for (char ch : name) {
    if (!alnum(ch) && ch != '.' && ch != '_' && ch != '-')
      return false;
  }
  return true;
}

std::vector<std::string> split_requirements(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream iss(text);
  std::string word;
  while (iss >> word)
    out.push_back(word);
  return out;
}

std::string error_summary(const std::string& stderr_text) {
  // PEP 668: pip's own message is a long boxed paragraph about apt / pipx
  // that says nothing about how to proceed from here.
  if (stderr_text.find("externally-managed-environment") != std::string::npos)
    return "this Python is managed by the operating system (externally-managed-environment), so pip will not "
           "change it. Create a virtualenv (python3 -m venv ~/.venvs/work) and restart with it: "
           "wish client --run=pip -- ~/.venvs/work";
  std::string text = stderr_text;
  size_t pos = text.rfind("ERROR:", 0) == 0 ? 0 : text.find("\nERROR:");
  if (pos != std::string::npos && pos != 0)
    text = text.substr(pos + 1);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
}

} // namespace bdg::wish::pip
