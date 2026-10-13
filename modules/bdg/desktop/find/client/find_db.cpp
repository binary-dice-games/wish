// MIT License © 2026 Binary Dice Games
/// @file find_db.cpp
/// @brief Implementation of the find app's index and query engine.
#include "modules/bdg/desktop/find/client/find_db.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace bdg::wish::find_db {

namespace fs = std::filesystem;

namespace {

// ── Text helpers ─────────────────────────────────────────────────────────────

char lower_char(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    c = lower_char(c);
  return out;
}

bool is_separator(char c) {
  return c == '/' || c == '\\';
}

/// Case-insensitive (ASCII) three-way comparison.
int compare_ci(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const char ca = lower_char(a[i]), cb = lower_char(b[i]);
    if (ca != cb)
      return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
  }
  if (a.size() != b.size())
    return a.size() < b.size() ? -1 : 1;
  return a.compare(b) < 0 ? -1 : (a.compare(b) > 0 ? 1 : 0);
}

/// @p needle (already lower-cased when !match_case) anywhere in @p hay.
bool contains(std::string_view hay, std::string_view needle, bool match_case) {
  if (needle.empty())
    return true;
  if (match_case)
    return hay.find(needle) != std::string_view::npos;
  if (needle.size() > hay.size())
    return false;
  const char first = needle[0];
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (lower_char(hay[i]) != first)
      continue;
    size_t k = 1;
    while (k < needle.size() && lower_char(hay[i + k]) == needle[k])
      ++k;
    if (k == needle.size())
      return true;
  }
  return false;
}

bool equals(std::string_view a, std::string_view pattern, bool match_case) {
  if (a.size() != pattern.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if ((match_case ? a[i] : lower_char(a[i])) != pattern[i])
      return false;
  return true;
}

/// `*` / `?` wildcard match of the whole of @p s (iterative, backtracking on
/// the last `*` only -- linear for typical patterns).
bool wildcard_match(std::string_view s, std::string_view pattern, bool match_case) {
  size_t si = 0, pi = 0, star = std::string_view::npos, mark = 0;
  while (si < s.size()) {
    const char sc = match_case ? s[si] : lower_char(s[si]);
    if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == sc)) {
      ++si;
      ++pi;
    } else if (pi < pattern.size() && pattern[pi] == '*') {
      star = pi++;
      mark = si;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (pi < pattern.size() && pattern[pi] == '*')
    ++pi;
  return pi == pattern.size();
}

std::string join_path(const std::string& base, std::string_view name) {
  std::string out = base;
  if (!out.empty() && !is_separator(out.back()))
    out += static_cast<char>(fs::path::preferred_separator);
  out += name;
  return out;
}

// ── File type families ───────────────────────────────────────────────────────

// Extension lists after FSearch's default filters.
constexpr std::string_view kApplications[] = {"app", "apk", "appimage", "bat", "cmd", "desktop", "exe", "msi"};
constexpr std::string_view kArchives[] = {"7z",  "ar",   "bz2", "cab", "gz",  "iso", "lz",  "lz4", "lzma",
                                          "rar", "tar",  "tbz", "tbz2", "tgz", "txz", "xz",  "z",   "zip",
                                          "zst", "jar",  "deb", "rpm"};
constexpr std::string_view kAudio[] = {"aac", "ac3", "aif", "aiff", "ape", "au",  "flac", "m4a",
                                       "mid", "midi", "mka", "mp3", "mpa", "oga", "ogg",  "opus",
                                       "ra",  "wav", "wma", "wv"};
constexpr std::string_view kDocuments[] = {"c",    "chm",  "cpp",  "csv",  "cxx",  "doc",  "docm", "docx", "dot",
                                           "dotx", "epub", "h",    "hpp",  "htm",  "html", "hxx",  "ini",  "java",
                                           "json", "lua",  "md",   "mht",  "odp",  "ods",  "odt",  "pdf",  "pps",
                                           "ppsx", "ppt",  "pptm", "pptx", "py",   "rst",  "rtf",  "tex",  "txt",
                                           "wpd",  "wps",  "xls",  "xlsb", "xlsm", "xlsx", "xml",  "yaml", "yml"};
constexpr std::string_view kPictures[] = {"ani", "avif", "bmp", "gif",  "heic", "ico", "jpe", "jpeg", "jpg",
                                          "pcx", "png",  "psd", "svg",  "tga",  "tif", "tiff", "webp", "wmf",
                                          "xcf", "raw",  "cr2", "nef"};
constexpr std::string_view kVideos[] = {"3g2", "3gp",  "asf", "avi", "divx", "f4v", "flv", "m2ts", "m4v",
                                        "mkv", "mov",  "mp4", "mpe", "mpeg", "mpg", "mts", "ogm",  "ogv",
                                        "qt",  "rm",   "rmvb", "vob",  "webm", "wmv"};

template <size_t N>
bool in_list(const std::string_view (&list)[N], std::string_view ext) {
  return std::find(std::begin(list), std::end(list), ext) != std::end(list);
}

/// A family's sort rank in the Type column (folders 0, plain files last).
int family_rank(filter_kind k) {
  switch (k) {
    case filter_kind::applications:
      return 1;
    case filter_kind::archives:
      return 2;
    case filter_kind::audio:
      return 3;
    case filter_kind::documents:
      return 4;
    case filter_kind::pictures:
      return 5;
    case filter_kind::videos:
      return 6;
    default:
      return 7;
  }
}

// ── Dates ────────────────────────────────────────────────────────────────────

std::tm local_tm(std::time_t t) {
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  return tm_buf;
}

/// Midnight (local) of the day @p days after year-month-day.
std::time_t local_midnight(int year, int month, int day) {
  std::tm tm_buf{};
  tm_buf.tm_year = year - 1900;
  tm_buf.tm_mon = month - 1;
  tm_buf.tm_mday = day;
  tm_buf.tm_isdst = -1;
  return std::mktime(&tm_buf);
}

/// [begin, end) of a `dm:` date word or `YYYY-MM-DD` / `YYYY-MM` / `YYYY`.
std::optional<std::pair<std::int64_t, std::int64_t>> parse_day_range(std::string_view text, std::time_t now) {
  const std::tm today = local_tm(now);
  const std::string word = lower(text);
  if (word == "today") {
    const std::time_t b = local_midnight(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday);
    return std::pair{b, local_midnight(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday + 1)};
  }
  if (word == "yesterday") {
    const std::time_t b = local_midnight(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday - 1);
    return std::pair{b, local_midnight(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday)};
  }
  const std::int64_t tomorrow = local_midnight(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday + 1);
  if (word == "thisweek") // the last seven days, today included
    return std::pair{
        static_cast<std::int64_t>(local_midnight(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday - 6)),
        tomorrow};
  if (word == "thismonth")
    return std::pair{static_cast<std::int64_t>(local_midnight(today.tm_year + 1900, today.tm_mon + 1, 1)), tomorrow};
  if (word == "thisyear")
    return std::pair{static_cast<std::int64_t>(local_midnight(today.tm_year + 1900, 1, 1)), tomorrow};

  int y = 0, m = 0, d = 0;
  char tail = 0;
  const std::string s(text);
  if (std::sscanf(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &tail) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
    return std::pair{
        static_cast<std::int64_t>(local_midnight(y, m, d)), static_cast<std::int64_t>(local_midnight(y, m, d + 1))};
  if (std::sscanf(s.c_str(), "%4d-%2d%c", &y, &m, &tail) == 2 && m >= 1 && m <= 12)
    return std::pair{
        static_cast<std::int64_t>(local_midnight(y, m, 1)), static_cast<std::int64_t>(local_midnight(y, m + 1, 1))};
  if (s.size() == 4 && std::sscanf(s.c_str(), "%4d%c", &y, &tail) == 1)
    return std::pair{
        static_cast<std::int64_t>(local_midnight(y, 1, 1)), static_cast<std::int64_t>(local_midnight(y + 1, 1, 1))};
  return std::nullopt;
}

// ── Sizes ────────────────────────────────────────────────────────────────────

std::optional<std::uint64_t> parse_size(std::string_view text) {
  size_t i = 0;
  while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.'))
    ++i;
  if (i == 0)
    return std::nullopt;
  const double value = std::strtod(std::string(text.substr(0, i)).c_str(), nullptr);
  const std::string unit = lower(text.substr(i));
  double mult = 1;
  if (unit.empty() || unit == "b")
    mult = 1;
  else if (unit == "k" || unit == "kb")
    mult = 1024.0;
  else if (unit == "m" || unit == "mb")
    mult = 1024.0 * 1024.0;
  else if (unit == "g" || unit == "gb")
    mult = 1024.0 * 1024.0 * 1024.0;
  else if (unit == "t" || unit == "tb")
    mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
  else
    return std::nullopt;
  return static_cast<std::uint64_t>(value * mult);
}

/// A `>x`, `>=x`, `<x`, `<=x`, `=x`, `x`, `a..b` comparison over a value
/// type whose single values parse into [begin, end) ranges.
template <typename ParseRange>
std::optional<std::pair<std::int64_t, std::int64_t>> parse_comparison(std::string_view text, ParseRange parse) {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if (const size_t dots = text.find(".."); dots != std::string_view::npos) {
    auto lo = parse(text.substr(0, dots));
    auto hi = parse(text.substr(dots + 2));
    if (!lo || !hi)
      return std::nullopt;
    return std::pair{lo->first, hi->second};
  }
  auto strip = [&](std::string_view op) {
    if (text.substr(0, op.size()) != op)
      return false;
    text.remove_prefix(op.size());
    return true;
  };
  if (strip(">=")) {
    auto r = parse(text);
    return r ? std::optional{std::pair{r->first, kMax}} : std::nullopt;
  }
  if (strip("<=")) {
    auto r = parse(text);
    return r ? std::optional{std::pair{kMin, r->second}} : std::nullopt;
  }
  if (strip(">")) {
    auto r = parse(text);
    return r ? std::optional{std::pair{r->second, kMax}} : std::nullopt;
  }
  if (strip("<")) {
    auto r = parse(text);
    return r ? std::optional{std::pair{kMin, r->first}} : std::nullopt;
  }
  strip("=");
  return parse(text);
}

// ── Compiled query ───────────────────────────────────────────────────────────

struct term {
  enum class kind { text, ext, size, modified };
  kind k{kind::text};
  bool negate{false};
  int type_req{0}; ///< 0 any, 1 files only, 2 folders only.

  // kind::text
  std::string pattern; ///< Lower-cased unless match_case.
  bool match_case{false};
  int case_forced{0}; ///< `case:` 1, `nocase:` -1, automatic 0.
  bool in_path{false};
  bool wildcard{false};
  bool exact{false};
  std::shared_ptr<std::regex> re;

  // kind::ext
  std::vector<std::string> exts;

  // kind::size / kind::modified: [lo, hi)
  std::int64_t lo{0};
  std::int64_t hi{0};
};

/// The query in conjunctive normal form: every clause must have one
/// matching term.
struct compiled_query {
  std::vector<std::vector<term>> clauses;
  int type_req{0};
  std::vector<std::string> filter_exts;
  std::string error;
};

struct token {
  std::string text;
  bool quoted{false};
};

std::vector<token> tokenize(std::string_view q) {
  std::vector<token> out;
  token cur;
  bool in_quotes = false, has = false;
  for (char c : q) {
    if (c == '"') {
      in_quotes = !in_quotes;
      cur.quoted = true;
      has = true;
      continue;
    }
    if (!in_quotes && std::isspace(static_cast<unsigned char>(c))) {
      if (has)
        out.push_back(std::move(cur));
      cur = {};
      has = false;
      continue;
    }
    cur.text += c;
    has = true;
  }
  if (has)
    out.push_back(std::move(cur));
  return out;
}

bool has_upper(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
}

std::vector<std::string> split_exts(std::string_view list) {
  std::vector<std::string> out;
  size_t begin = 0;
  while (begin <= list.size()) {
    size_t end = list.find_first_of(";,", begin);
    if (end == std::string_view::npos)
      end = list.size();
    std::string_view one = list.substr(begin, end - begin);
    if (!one.empty() && one.front() == '.')
      one.remove_prefix(1);
    out.push_back(lower(one));
    begin = end + 1;
  }
  return out;
}

void set_text(term& t, std::string text, const search_options& o, bool query_upper) {
  t.k = term::kind::text;
  t.match_case = t.case_forced == 1 || (t.case_forced == 0 && (o.match_case || (o.auto_match_case && query_upper)));
  t.in_path = t.in_path || o.search_in_path ||
      (o.auto_search_in_path && std::any_of(text.begin(), text.end(), is_separator));
  if (t.re) {
    // regex: modifier -- compiled by the caller.
  } else {
    t.wildcard = text.find_first_of("*?") != std::string::npos;
  }
  t.pattern = t.match_case ? std::move(text) : lower(text);
}

/// Parses one token into @p t. Returns false (with @p error set) on an
/// invalid function argument.
bool parse_term(const token& tok, const search_options& o, bool query_upper, term& t, std::string& error) {
  std::string text = tok.text;
  if (tok.quoted) {
    set_text(t, std::move(text), o, query_upper);
    return true;
  }
  // Modifiers and functions, possibly chained (`case:path:Foo`).
  bool regex_mod = false;
  while (true) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos)
      break;
    const std::string fn = lower(std::string_view(text).substr(0, colon));
    std::string arg = text.substr(colon + 1);
    if (fn == "case") {
      t.case_forced = 1;
    } else if (fn == "nocase") {
      t.case_forced = -1;
    } else if (fn == "path") {
      t.in_path = true;
    } else if (fn == "exact") {
      t.exact = true;
    } else if (fn == "regex") {
      regex_mod = true;
    } else if (fn == "file" || fn == "files") {
      t.type_req = 1;
    } else if (fn == "folder" || fn == "folders" || fn == "dir") {
      t.type_req = 2;
    } else if (fn == "ext") {
      t.k = term::kind::ext;
      t.type_req = 1;
      t.exts = split_exts(arg);
      return true;
    } else if (fn == "size") {
      auto r = parse_comparison(arg, [](std::string_view v) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
        auto n = parse_size(v);
        if (!n)
          return std::nullopt;
        return std::pair{static_cast<std::int64_t>(*n), static_cast<std::int64_t>(*n) + 1};
      });
      if (!r) {
        error = "size:" + arg;
        return false;
      }
      t.k = term::kind::size;
      t.type_req = 1;
      t.lo = r->first;
      t.hi = r->second;
      return true;
    } else if (fn == "dm" || fn == "datemodified") {
      const std::time_t now = o.now ? o.now : std::time(nullptr);
      auto r = parse_comparison(arg, [&](std::string_view v) { return parse_day_range(v, now); });
      if (!r) {
        error = "dm:" + arg;
        return false;
      }
      t.k = term::kind::modified;
      t.lo = r->first;
      t.hi = r->second;
      return true;
    } else {
      break; // Not a function: "C:" or "a:b" is plain text.
    }
    text = std::move(arg);
  }

  if (regex_mod) {
    try {
      auto flags = std::regex::ECMAScript | std::regex::optimize;
      if (t.case_forced == -1 || (t.case_forced == 0 && !o.match_case))
        flags |= std::regex::icase;
      t.re = std::make_shared<std::regex>(text, flags);
    } catch (const std::regex_error& e) {
      error = e.what();
      return false;
    }
    t.k = term::kind::text;
    t.match_case = true; // the regex handles case itself
    t.in_path = t.in_path || o.search_in_path;
    t.pattern = text;
    return true;
  }
  if (text.empty() && t.type_req != 0) {
    t.k = term::kind::text; // `file:` / `folder:` alone: the type is the test
    return true;
  }
  set_text(t, std::move(text), o, query_upper);
  return true;
}

compiled_query compile(const search_options& o) {
  compiled_query q;
  switch (o.filter) {
    case filter_kind::folders:
      q.type_req = 2;
      break;
    case filter_kind::files:
      q.type_req = 1;
      break;
    case filter_kind::all:
      break;
    default:
      q.type_req = 1;
      break;
  }

  if (o.regex) {
    if (o.query.empty())
      return q;
    term t;
    try {
      auto flags = std::regex::ECMAScript | std::regex::optimize;
      if (!(o.match_case || (o.auto_match_case && has_upper(o.query))))
        flags |= std::regex::icase;
      t.re = std::make_shared<std::regex>(o.query, flags);
    } catch (const std::regex_error& e) {
      q.error = e.what();
      return q;
    }
    t.match_case = true;
    t.in_path = o.search_in_path;
    q.clauses.push_back({std::move(t)});
    return q;
  }

  // Smart case looks at the terms only: the OR / AND / NOT keywords are
  // upper-case by definition.
  const std::vector<token> tokens = tokenize(o.query);
  const bool query_upper = std::any_of(tokens.begin(), tokens.end(), [](const token& t) {
    return (t.quoted || (t.text != "OR" && t.text != "AND" && t.text != "NOT")) && has_upper(t.text);
  });
  bool pending_or = false, pending_not = false;
  for (const token& tok : tokens) {
    if (!tok.quoted && (tok.text == "OR" || tok.text == "|" || tok.text == "||")) {
      pending_or = !q.clauses.empty();
      continue;
    }
    if (!tok.quoted && (tok.text == "AND" || tok.text == "&&"))
      continue;
    if (!tok.quoted && tok.text == "NOT") {
      pending_not = !pending_not;
      continue;
    }
    token body = tok;
    term t;
    t.negate = pending_not;
    pending_not = false;
    while (!body.quoted && !body.text.empty() && body.text.front() == '!') {
      t.negate = !t.negate;
      body.text.erase(0, 1);
    }
    if (body.text.empty() && !body.quoted)
      continue;
    std::string error;
    if (!parse_term(body, o, query_upper, t, error)) {
      q.error = error;
      q.clauses.clear();
      return q;
    }
    if (pending_or)
      q.clauses.back().push_back(std::move(t));
    else
      q.clauses.push_back({std::move(t)});
    pending_or = false;
  }
  return q;
}

std::vector<std::string_view> filter_list(filter_kind k) {
  auto view = [](const auto& list) { return std::vector<std::string_view>(std::begin(list), std::end(list)); };
  switch (k) {
    case filter_kind::applications:
      return view(kApplications);
    case filter_kind::archives:
      return view(kArchives);
    case filter_kind::audio:
      return view(kAudio);
    case filter_kind::documents:
      return view(kDocuments);
    case filter_kind::pictures:
      return view(kPictures);
    case filter_kind::videos:
      return view(kVideos);
    default:
      return {};
  }
}

// ── Matching ─────────────────────────────────────────────────────────────────

/// Per-entry scratch: the full path, built on demand once per entry.
struct match_context {
  const database& db;
  std::uint32_t index{0};
  std::string path;
  bool path_built{false};

  const std::string& full_path() {
    if (!path_built) {
      path = db.full_path(index);
      path_built = true;
    }
    return path;
  }
};

bool term_matches(const term& t, const entry& e, match_context& ctx) {
  if (t.type_req == 1 && e.is_dir)
    return false;
  if (t.type_req == 2 && !e.is_dir)
    return false;
  switch (t.k) {
    case term::kind::ext: {
      const std::string ext = extension_of(e.name);
      return std::find(t.exts.begin(), t.exts.end(), ext) != t.exts.end();
    }
    case term::kind::size: {
      const auto size = static_cast<std::int64_t>(e.size);
      return size >= t.lo && size < t.hi;
    }
    case term::kind::modified:
      return e.mtime != 0 && e.mtime >= t.lo && e.mtime < t.hi;
    case term::kind::text:
      break;
  }
  const std::string_view subject = t.in_path ? std::string_view(ctx.full_path()) : std::string_view(e.name);
  if (t.re)
    return std::regex_search(subject.begin(), subject.end(), *t.re);
  if (t.exact)
    return t.wildcard ? wildcard_match(subject, t.pattern, t.match_case) : equals(subject, t.pattern, t.match_case);
  if (t.wildcard) {
    // A path search with a wildcard matches the whole path, so `*/src/*`
    // works; a name search matches the whole name, like FSearch.
    return wildcard_match(subject, t.pattern, t.match_case);
  }
  return contains(subject, t.pattern, t.match_case);
}

bool query_matches(const compiled_query& q, const entry& e, match_context& ctx) {
  if (q.type_req == 1 && e.is_dir)
    return false;
  if (q.type_req == 2 && !e.is_dir)
    return false;
  if (!q.filter_exts.empty()) {
    const std::string ext = extension_of(e.name);
    if (std::find(q.filter_exts.begin(), q.filter_exts.end(), ext) == q.filter_exts.end())
      return false;
  }
  for (const auto& clause : q.clauses) {
    bool any = false;
    for (const term& t : clause)
      if (term_matches(t, e, ctx) != t.negate) {
        any = true;
        break;
      }
    if (!any)
      return false;
  }
  return true;
}

// ── Indexing ─────────────────────────────────────────────────────────────────

std::int64_t to_unix_seconds(const fs::file_time_type& ftime) {
  const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

/// The device a path lives on, for one_filesystem (0 when unknown).
std::uint64_t device_of(const fs::path& path) {
#if defined(_WIN32)
  (void)path;
  return 0;
#else
  struct stat st{};
  return ::lstat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
#endif
}

bool excluded(const index_options& o, const std::string& name, const fs::path& full) {
  if (o.exclude_hidden && !name.empty() && name.front() == '.')
    return true;
  for (const auto& ex : o.excludes) {
    if (ex.empty())
      continue;
    if (ex == name)
      return true;
    if (fs::path(ex).is_absolute() && fs::path(ex).lexically_normal() == full)
      return true;
  }
  return false;
}

fs::path normalize_location(const std::string& location) {
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(location), ec);
  if (ec)
    p = fs::path(location);
  p = p.lexically_normal();
  // "/tmp/" normalizes with an empty last component; keep "/" intact.
  if (!p.has_filename() && p.has_parent_path() && p != p.root_path())
    p = p.parent_path();
  return p;
}

} // namespace

// ── database ─────────────────────────────────────────────────────────────────

std::uint32_t database::add_root(const std::string& full_path, std::int64_t mtime) {
  entry e;
  const fs::path p(full_path);
  e.name = p.has_filename() ? p.filename().string() : full_path;
  e.is_dir = true;
  e.mtime = mtime;
  e.dir_slot = static_cast<std::uint32_t>(dir_paths_.size());
  dir_paths_.push_back(full_path);
  entries_.push_back(std::move(e));
  return static_cast<std::uint32_t>(entries_.size() - 1);
}

std::uint32_t database::add(
    std::uint32_t parent, const std::string& name, bool is_dir, std::uint64_t size, std::int64_t mtime) {
  entry e;
  e.name = name;
  e.parent = parent;
  e.is_dir = is_dir;
  e.size = is_dir ? 0 : size;
  e.mtime = mtime;
  if (is_dir) {
    e.dir_slot = static_cast<std::uint32_t>(dir_paths_.size());
    dir_paths_.push_back(join_path(dir_paths_[entries_[parent].dir_slot], name));
  }
  entries_.push_back(std::move(e));
  return static_cast<std::uint32_t>(entries_.size() - 1);
}

void database::finalize() {
  files_ = folders_ = 0;
  // Parents precede their children, so one backward pass sums every
  // folder's subtree.
  for (size_t i = entries_.size(); i-- > 0;) {
    const entry& e = entries_[i];
    (e.is_dir ? folders_ : files_)++;
    if (e.parent != kNone)
      entries_[e.parent].size += e.size;
  }

  by_name_.resize(entries_.size());
  for (size_t i = 0; i < by_name_.size(); ++i)
    by_name_[i] = static_cast<std::uint32_t>(i);

  // Folders ranked by full path first: the name order breaks ties by it.
  std::vector<std::uint32_t> slots(dir_paths_.size());
  for (size_t i = 0; i < slots.size(); ++i)
    slots[i] = static_cast<std::uint32_t>(i);
  std::sort(slots.begin(), slots.end(), [&](std::uint32_t a, std::uint32_t b) {
    const int c = compare_ci(dir_paths_[a], dir_paths_[b]);
    return c != 0 ? c < 0 : a < b;
  });
  dir_rank_.assign(dir_paths_.size(), 0);
  for (size_t r = 0; r < slots.size(); ++r)
    dir_rank_[slots[r]] = static_cast<std::uint32_t>(r + 1);

  std::sort(by_name_.begin(), by_name_.end(), [&](std::uint32_t a, std::uint32_t b) {
    const entry& ea = entries_[a];
    const entry& eb = entries_[b];
    if (ea.is_dir != eb.is_dir)
      return ea.is_dir;
    const int c = compare_ci(ea.name, eb.name);
    if (c != 0)
      return c < 0;
    const std::uint32_t pa = parent_rank(a), pb = parent_rank(b);
    return pa != pb ? pa < pb : a < b;
  });
  name_rank_.assign(entries_.size(), 0);
  for (size_t r = 0; r < by_name_.size(); ++r)
    name_rank_[by_name_[r]] = static_cast<std::uint32_t>(r);
}

std::uint32_t database::parent_rank(std::uint32_t index) const {
  const entry& e = entries_[index];
  if (e.parent == kNone)
    return 0;
  return dir_rank_.empty() ? 0 : dir_rank_[entries_[e.parent].dir_slot];
}

std::string database::full_path(std::uint32_t index) const {
  const entry& e = entries_[index];
  if (e.is_dir)
    return dir_paths_[e.dir_slot];
  return join_path(dir_paths_[entries_[e.parent].dir_slot], e.name);
}

std::string database::parent_path(std::uint32_t index) const {
  const entry& e = entries_[index];
  if (e.parent != kNone)
    return dir_paths_[entries_[e.parent].dir_slot];
  const fs::path p(dir_paths_[e.dir_slot]);
  return p.has_parent_path() && p.parent_path() != p ? p.parent_path().string() : std::string{};
}

bool database::build(
    const index_options& options, database& out, const progress_fn& on_progress, index_progress* totals) {
  out = database{};
  index_progress progress;
  std::error_code ec;

  for (const auto& location : options.locations) {
    if (location.empty())
      continue;
    const fs::path root = normalize_location(location);
    if (!fs::is_directory(root, ec)) {
      ++progress.errors;
      continue;
    }
    const auto root_mtime = fs::last_write_time(root, ec);
    const std::uint32_t root_index = out.add_root(root.string(), ec ? 0 : to_unix_seconds(root_mtime));
    ++progress.folders;
    const std::uint64_t root_dev = options.one_filesystem ? device_of(root) : 0;

    // Depth-first, with an explicit stack: deep trees must not overflow the
    // call stack.
    std::vector<std::pair<std::uint32_t, fs::path>> pending{{root_index, root}};
    while (!pending.empty()) {
      auto [dir_index, dir_path] = std::move(pending.back());
      pending.pop_back();
      progress.current = dir_path.string();
      if (on_progress && !on_progress(progress))
        return false;

      fs::directory_iterator it(dir_path, fs::directory_options::skip_permission_denied, ec);
      if (ec) {
        ++progress.errors;
        continue;
      }
      for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec)
          break;
        const fs::directory_entry& de = *it;
        const fs::path child = de.path();
        const std::string name = child.filename().string();
        if (excluded(options, name, child))
          continue;
        std::error_code sec;
        const auto status = de.symlink_status(sec);
        const bool is_dir = !sec && fs::is_directory(status);
        std::uint64_t size = 0;
        if (!is_dir && fs::is_regular_file(status)) {
          size = de.file_size(sec);
          if (sec)
            size = 0;
        }
        const auto ftime = de.last_write_time(sec);
        const std::int64_t mtime = sec ? 0 : to_unix_seconds(ftime);
        const std::uint32_t index = out.add(dir_index, name, is_dir, size, mtime);
        if (is_dir) {
          ++progress.folders;
          // A mount point is listed but not walked.
          if (!options.one_filesystem || root_dev == 0 || device_of(child) == root_dev)
            pending.emplace_back(index, child);
        } else {
          ++progress.files;
        }
      }
      if (ec) {
        ++progress.errors;
        ec.clear();
      }
    }
  }

  out.finalize();
  progress.current.clear();
  if (totals)
    *totals = progress;
  return true;
}

// ── search ───────────────────────────────────────────────────────────────────

search_result search(const database& db, const search_options& options, const std::function<bool()>& cancelled) {
  search_result result;
  compiled_query q = compile(options);
  if (!q.error.empty()) {
    result.error = q.error;
    return result;
  }
  for (auto ext : filter_list(options.filter))
    q.filter_exts.emplace_back(ext);

  // Walk in name order, so a name sort needs no sorting at all.
  std::vector<std::uint32_t> matches;
  const auto& order = db.name_order();
  size_t counter = 0;
  for (std::uint32_t index : order) {
    if (cancelled && (++counter & 0xFFFF) == 0 && cancelled()) {
      result.cancelled = true;
      return result;
    }
    const entry& e = db.at(index);
    match_context ctx{db, index, {}, false};
    if (!query_matches(q, e, ctx))
      continue;
    matches.push_back(index);
    if (e.is_dir)
      ++result.folders;
  }
  result.total = matches.size();

  const size_t k = std::min(options.max_rows, matches.size());
  auto less = [&](std::uint32_t a, std::uint32_t b) -> bool {
    const entry& ea = db.at(a);
    const entry& eb = db.at(b);
    switch (options.sort) {
      case sort_column::path: {
        const std::uint32_t pa = db.parent_rank(a), pb = db.parent_rank(b);
        if (pa != pb)
          return pa < pb;
        break;
      }
      case sort_column::type: {
        if (ea.is_dir != eb.is_dir)
          return ea.is_dir;
        if (!ea.is_dir) {
          const std::string xa = extension_of(ea.name), xb = extension_of(eb.name);
          const int ra = family_rank(family_of(xa)), rb = family_rank(family_of(xb));
          if (ra != rb)
            return ra < rb;
          if (xa != xb)
            return xa < xb;
        }
        break;
      }
      case sort_column::size:
        if (ea.size != eb.size)
          return ea.size < eb.size;
        break;
      case sort_column::modified:
        if (ea.mtime != eb.mtime)
          return ea.mtime < eb.mtime;
        break;
      case sort_column::name:
        break;
    }
    return db.name_rank(a) < db.name_rank(b);
  };

  if (options.sort == sort_column::name) {
    // Already in ascending name order.
    if (options.ascending)
      result.rows.assign(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(k));
    else
      result.rows.assign(matches.rbegin(), matches.rbegin() + static_cast<std::ptrdiff_t>(k));
    return result;
  }
  auto cmp = [&](std::uint32_t a, std::uint32_t b) { return options.ascending ? less(a, b) : less(b, a); };
  std::partial_sort(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(k), matches.end(), cmp);
  result.rows.assign(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(k));
  return result;
}

// ── Formatting ───────────────────────────────────────────────────────────────

std::string format_bytes(std::uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
  return buf;
}

std::string format_count(std::uint64_t n) {
  std::string digits = std::to_string(n);
  for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast<size_t>(i), 1, ',');
  return digits;
}

std::string format_time(std::int64_t seconds) {
  if (seconds == 0)
    return {};
  const std::tm tm_buf = local_tm(static_cast<std::time_t>(seconds));
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
  return buf;
}

std::string extension_of(std::string_view name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size())
    return {};
  return lower(name.substr(dot + 1));
}

filter_kind family_of(std::string_view ext) {
  if (ext.empty())
    return filter_kind::all;
  if (in_list(kPictures, ext))
    return filter_kind::pictures;
  if (in_list(kVideos, ext))
    return filter_kind::videos;
  if (in_list(kAudio, ext))
    return filter_kind::audio;
  if (in_list(kArchives, ext))
    return filter_kind::archives;
  if (in_list(kDocuments, ext))
    return filter_kind::documents;
  if (in_list(kApplications, ext))
    return filter_kind::applications;
  return filter_kind::all;
}

std::string type_label(const entry& e) {
  if (e.is_dir)
    return "$$FIND_TYPE_FOLDER";
  const std::string ext = extension_of(e.name);
  if (ext.empty())
    return "$$FIND_TYPE_FILE";
  const char* key = "$$FIND_TYPE_EXT_FILE";
  switch (family_of(ext)) {
    case filter_kind::pictures:
      key = "$$FIND_TYPE_IMAGE";
      break;
    case filter_kind::videos:
      key = "$$FIND_TYPE_VIDEO";
      break;
    case filter_kind::audio:
      key = "$$FIND_TYPE_AUDIO";
      break;
    case filter_kind::archives:
      key = "$$FIND_TYPE_ARCHIVE";
      break;
    case filter_kind::documents:
      key = "$$FIND_TYPE_DOCUMENT";
      break;
    case filter_kind::applications:
      key = "$$FIND_TYPE_APPLICATION";
      break;
    default:
      break;
  }
  std::string upper_ext = ext;
  for (char& c : upper_ext)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  // make_message()'s encoding (src/i18n/translations.hpp), spelled out to
  // keep this file free of wish dependencies.
  return std::string{key} + '\x1f' + upper_ext;
}

} // namespace bdg::wish::find_db
