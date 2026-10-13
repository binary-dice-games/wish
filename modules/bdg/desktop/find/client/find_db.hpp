// MIT License © 2026 Binary Dice Games
/// @file find_db.hpp
/// @brief The find embedded app's file index and query engine.
///
/// Modelled on FSearch (https://github.com/cboxdoerfer/fsearch): the
/// configured folders are walked once into an in-memory database, which is
/// then searched on every keystroke without touching the disk again. The
/// database keeps every entry presorted by name and every folder ranked by
/// path, so a query only filters and, for the other columns, partially
/// sorts the matches it actually shows.
///
/// Plain C++ with no bison/RMI dependency, so it is unit tested on its own
/// (tests/test_find_db.cpp); client/find.cpp turns a search_result into the
/// arguments of the Find form's `show_results()` method.
///
/// ## Query syntax
///
/// Whitespace-separated terms must all match (AND); `OR` (or `|`, `||`)
/// between two terms makes either one enough, binding tighter than AND
/// (`a b OR c` is `a AND (b OR c)`). `"quoted text"` is one literal term.
/// `!term` or `NOT term` negates a term. A term is matched against the
/// entry's name -- or its full path with Search in Path, or when the term
/// contains a path separator (automatic path search). A term with `*` or
/// `?` is a wildcard pattern matching the whole name; any other term
/// matches anywhere in it. Case is ignored unless Match Case is on, or the
/// query has an upper-case letter (automatic match case).
///
/// Functions (`name:value`, combinable with `!`):
///   - `ext:jpg;png` -- files with one of these extensions (`ext:` alone:
///     files without an extension);
///   - `file:` / `folder:` -- only files / folders, optionally followed by a
///     term (`folder:src`);
///   - `size:>1mb`, `size:<=10k`, `size:1mb..2gb`, `size:100` -- file size
///     (b, kb/k, mb/m, gb/g, tb/t; binary units);
///   - `dm:today`, `dm:yesterday`, `dm:thisweek`, `dm:thismonth`,
///     `dm:thisyear`, `dm:2026-01-31`, `dm:>2026-01-01`,
///     `dm:2026-01-01..2026-02-01` -- date modified (local time);
///   - `path:`, `case:`, `nocase:`, `regex:`, `exact:` -- modifiers of the
///     term that follows the colon (`case:Makefile`, `exact:readme.md`).
///
/// With Regular Expression on, the whole query is one ECMAScript regular
/// expression, searched for in each name (or path).
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bdg::wish::find_db {

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

/// @brief One indexed file or folder.
struct entry {
  std::string name;
  /// Index of the containing folder; kNone for an indexed location itself.
  std::uint32_t parent{kNone};
  /// For a folder, its slot in the database's folder table; kNone for a file.
  std::uint32_t dir_slot{kNone};
  bool is_dir{false};
  /// A file's size in bytes; for a folder, the total of everything below it.
  std::uint64_t size{0};
  /// Last modification, seconds since the Unix epoch (0 when unknown).
  std::int64_t mtime{0};
};

/// @brief What to index.
struct index_options {
  /// Folders to index (absolute or relative to the current directory).
  std::vector<std::string> locations;
  /// Folder or file names (`node_modules`) or absolute paths that are left
  /// out, together with everything below them.
  std::vector<std::string> excludes;
  /// Leaves out names starting with a dot.
  bool exclude_hidden{false};
  /// On POSIX, stays on each location's filesystem (like `find -xdev`), so
  /// `/proc`, network mounts and other disks are not walked.
  bool one_filesystem{true};
};

/// @brief Running totals handed to an indexing progress callback.
struct index_progress {
  std::uint64_t files{0};
  std::uint64_t folders{0};
  std::uint64_t errors{0}; ///< Folders that could not be read.
  std::string current;     ///< The folder being read.
};

/// @brief Called once per folder, before it is read. Returning false
/// cancels the indexing.
using progress_fn = std::function<bool(const index_progress&)>;

/// @brief The in-memory file index.
class database {
 public:
  /// @brief Walks @p options.locations into a new database.
  ///
  /// Symbolic links are indexed as files and never followed. Unreadable
  /// folders are counted in `index_progress::errors` and indexed empty; a
  /// location that does not exist or is not a folder is counted there too.
  /// @param out          Receives the database (finalized).
  /// @param on_progress  Optional; see progress_fn.
  /// @param totals       Optional; receives the final totals.
  /// @return false if @p on_progress cancelled (@p out is then unspecified).
  static bool build(
      const index_options& options,
      database& out,
      const progress_fn& on_progress = {},
      index_progress* totals = nullptr);

  // ── Building by hand (build() and tests) ────────────────────────────────

  /// @brief Adds an indexed location: a folder with no parent.
  /// @param full_path  Its full path; shown as its name's folder and name.
  std::uint32_t add_root(const std::string& full_path, std::int64_t mtime = 0);
  /// @brief Adds @p name inside folder @p parent. Parents precede children.
  std::uint32_t add(std::uint32_t parent, const std::string& name, bool is_dir, std::uint64_t size, std::int64_t mtime);
  /// @brief Sums folder sizes and builds the name and path orders. Call once
  /// after the last add(); build() does.
  void finalize();

  // ── Reading ─────────────────────────────────────────────────────────────

  std::size_t size() const {
    return entries_.size();
  }
  const entry& at(std::uint32_t index) const {
    return entries_[index];
  }
  std::uint64_t file_count() const {
    return files_;
  }
  std::uint64_t folder_count() const {
    return folders_;
  }

  /// @brief The entry's full path.
  std::string full_path(std::uint32_t index) const;
  /// @brief The folder the entry is in (its "Path" column).
  std::string parent_path(std::uint32_t index) const;

  /// @brief Every entry, folders first, then by name (case-insensitive).
  const std::vector<std::uint32_t>& name_order() const {
    return by_name_;
  }
  /// @brief Position of @p index in name_order().
  std::uint32_t name_rank(std::uint32_t index) const {
    return name_rank_[index];
  }
  /// @brief Rank of the entry's containing folder in path order (0 for an
  /// indexed location, whose folder is outside the database).
  std::uint32_t parent_rank(std::uint32_t index) const;

 private:
  std::vector<entry> entries_;
  std::vector<std::string> dir_paths_; ///< By dir_slot: the folder's full path.
  std::vector<std::uint32_t> dir_rank_; ///< By dir_slot: rank in path order, from 1.
  std::vector<std::uint32_t> by_name_;
  std::vector<std::uint32_t> name_rank_;
  std::uint64_t files_{0};
  std::uint64_t folders_{0};
};

// ── Searching ─────────────────────────────────────────────────────────────

/// @brief The filter drop-down next to the search field.
enum class filter_kind : std::int32_t {
  all = 0,
  folders,
  files,
  applications,
  archives,
  audio,
  documents,
  pictures,
  videos,
};
inline constexpr std::int32_t kFilterCount = 9;

/// @brief The results table's columns, by `column_id`.
enum class sort_column : std::int32_t {
  name = 0,
  path = 1,
  type = 2,
  size = 3,
  modified = 4,
};

struct search_options {
  std::string query;
  filter_kind filter{filter_kind::all};
  bool match_case{false};
  bool search_in_path{false};
  bool regex{false};
  /// Match case when the query has an upper-case letter.
  bool auto_match_case{true};
  /// Search a term in the path when it contains a path separator.
  bool auto_search_in_path{true};
  sort_column sort{sort_column::name};
  bool ascending{true};
  /// At most this many matches are returned in `search_result::rows`.
  std::size_t max_rows{1000};
  /// Reference "now" for `dm:today` & co.; 0 uses the current time.
  std::time_t now{0};
};

struct search_result {
  std::uint64_t total{0};   ///< Matching entries.
  std::uint64_t folders{0}; ///< Of which folders.
  /// The first `max_rows` matches, in the requested order.
  std::vector<std::uint32_t> rows;
  /// Non-empty when the query is invalid (e.g. a bad regular expression);
  /// nothing matches then.
  std::string error;
  bool cancelled{false};
};

/// @brief Runs @p options against @p db.
/// @param cancelled  Optional; polled now and then, true stops the search
///                   (`search_result::cancelled`).
search_result search(
    const database& db,
    const search_options& options,
    const std::function<bool()>& cancelled = {});

// ── Formatting ────────────────────────────────────────────────────────────

/// @brief `"1.5 GB"`-style size, in binary units.
std::string format_bytes(std::uint64_t bytes);
/// @brief `1234567` -> `"1,234,567"`.
std::string format_count(std::uint64_t n);
/// @brief `"2026-10-12 14:03"`, local time; `""` for 0.
std::string format_time(std::int64_t seconds);

/// @brief The "Type" column of an entry: a `"$$FIND_TYPE_..."` translation
/// key or make_message()-shaped `"$$FIND_TYPE_<FAMILY>\x1f<EXT>"` string
/// (e.g. "PNG Image"), translated by the form.
std::string type_label(const entry& e);

/// @brief The lower-case extension of @p name without the dot, `""` if none
/// (a leading dot, as in `.bashrc`, is not an extension).
std::string extension_of(std::string_view name);

/// @brief The filter family of a file extension (`filter_kind::all` when it
/// belongs to none).
filter_kind family_of(std::string_view lower_ext);

} // namespace bdg::wish::find_db
