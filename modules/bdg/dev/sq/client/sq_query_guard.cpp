// MIT License © 2026 Binary Dice Games
/// @file sq_query_guard.cpp
/// @brief Implementation of check_read_only_sql().
#include "sq_query_guard.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace bdg::wish::sq {

namespace {

// Blanks out comments, string literals, quoted identifiers and dollar-quoted
// bodies so only real SQL tokens remain. Backslash is deliberately NOT an
// escape: for MySQL that only errs toward seeing (and rejecting) more code.
std::string strip_literals(const std::string& sql) {
  std::string out;
  out.reserve(sql.size());
  size_t i = 0;
  while (i < sql.size()) {
    const char c = sql[i];
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n')
        ++i;
      out += ' ';
    } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      const size_t end = sql.find("*/", i + 2);
      i = end == std::string::npos ? sql.size() : end + 2;
      out += ' ';
    } else if (c == '\'' || c == '"' || c == '`' || c == '[') {
      const char close = c == '[' ? ']' : c;
      ++i;
      while (i < sql.size()) {
        if (sql[i] == close) {
          if (close != ']' && i + 1 < sql.size() && sql[i + 1] == close) {
            i += 2; // doubled quote is an escaped quote
            continue;
          }
          break;
        }
        ++i;
      }
      ++i;
      out += ' ';
    } else if (c == '$') {
      // PostgreSQL $tag$ ... $tag$
      size_t j = i + 1;
      while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_'))
        ++j;
      if (j < sql.size() && sql[j] == '$') {
        const std::string tag = sql.substr(i, j - i + 1);
        const size_t end = sql.find(tag, j + 1);
        i = end == std::string::npos ? sql.size() : end + tag.size();
        out += ' ';
      } else {
        out += c;
        ++i;
      }
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

struct token {
  std::string word; // lower-cased
  bool call{false}; // next non-space character is '('
};

std::vector<token> tokenize(const std::string& s) {
  std::vector<token> tokens;
  size_t i = 0;
  while (i < s.size()) {
    if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') {
      size_t j = i;
      while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
        ++j;
      token t;
      t.word = s.substr(i, j - i);
      std::transform(t.word.begin(), t.word.end(), t.word.begin(), [](unsigned char ch) { return std::tolower(ch); });
      size_t k = j;
      while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k])))
        ++k;
      t.call = k < s.size() && s[k] == '(';
      tokens.push_back(std::move(t));
      i = j;
    } else {
      ++i;
    }
  }
  return tokens;
}

} // namespace

std::optional<std::string> check_read_only_sql(const std::string& sql) {
  std::string clean = strip_literals(sql);

  // Drop trailing statement terminators / whitespace; any remaining ';'
  // means more than one statement.
  while (!clean.empty() && (clean.back() == ';' || std::isspace(static_cast<unsigned char>(clean.back()))))
    clean.pop_back();
  if (clean.find_first_not_of(" \t\r\n") == std::string::npos)
    return std::string{"The query is empty."};
  if (clean.find(';') != std::string::npos)
    return std::string{"Only a single statement can be run."};

  const auto tokens = tokenize(clean);
  if (tokens.empty())
    return std::string{"The query is empty."};

  static const std::set<std::string> kAllowedFirst = {"select", "with", "values", "show", "describe", "desc", "explain"};
  if (!kAllowedFirst.count(tokens.front().word))
    return "Only read-only queries are allowed (a statement starting with '" + tokens.front().word + "' was rejected).";

  // Words that are also common scalar functions (REPLACE(), INSERT(),
  // TRUNCATE()) are only rejected when not used as a call.
  static const std::set<std::string> kFunctionLike = {"replace", "insert", "truncate"};
  static const std::set<std::string> kForbidden = {
      "insert", "update", "delete", "merge",  "drop",   "create", "alter",  "truncate", "grant",
      "revoke", "replace", "attach", "detach", "vacuum", "pragma", "call",   "exec",     "execute",
      "into",   "copy",    "load",   "upsert", "lock",   "reindex", "analyze"};
  for (const auto& t : tokens) {
    if (!kForbidden.count(t.word))
      continue;
    if (t.call && kFunctionLike.count(t.word))
      continue;
    return "Only read-only queries are allowed ('" + t.word + "' was rejected).";
  }
  return std::nullopt;
}

} // namespace bdg::wish::sq
