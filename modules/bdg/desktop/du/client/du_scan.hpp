// MIT License © 2026 Binary Dice Games
/// @file du_scan.hpp
/// @brief Directory scanning and treemap flattening for the du embedded app.
///
/// Plain C++ with no bison/RMI dependency, so it is unit tested on its own
/// (tests/test_du_scan.cpp); client/du.cpp turns its results into the
/// arguments of the Du form's `show_directory()` method.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bdg::wish::du_scan {

/// @brief One scanned file or directory.
struct entry {
  std::string name;
  /// A file's own size in bytes; for a directory, the total of everything
  /// below it.
  std::uint64_t size{0};
  std::uint64_t files{0}; ///< Files below a directory (0 for a file).
  std::uint64_t dirs{0};  ///< Directories below a directory (0 for a file).
  bool is_dir{false};
  /// A directory's contents, largest first (ties by name). Empty for a file.
  std::vector<entry> children;
};

/// @brief Running totals handed to a scan's progress callback.
struct progress {
  std::uint64_t items{0};  ///< Files and directories seen so far.
  std::uint64_t bytes{0};  ///< Sum of the file sizes seen so far.
  std::uint64_t errors{0}; ///< Directories that could not be read.
  std::string current;     ///< The directory being read.
};

/// @brief Called once per directory, before it is read. Returning false
/// cancels the scan.
using progress_fn = std::function<bool(const progress&)>;

/// @brief Recursively measures @p root.
///
/// Sizes are apparent file sizes (not allocated blocks). Symbolic links are
/// counted as zero-size files and never followed. On POSIX the scan stays on
/// @p root's filesystem, like `du -x`: a mount point below it is listed as
/// an empty directory, which keeps `/proc`, network mounts and other disks
/// out of the totals. Unreadable directories are counted in
/// `progress::errors` and treated as empty. A file with several hard links
/// is counted once per link.
///
/// @param root         Directory to scan.
/// @param out          Receives the tree; `out.name` is @p root's full path.
/// @param on_progress  Optional; see progress_fn.
/// @param totals       Optional; receives the final totals.
/// @return false if @p on_progress cancelled the scan (@p out is then
///         unspecified), true otherwise.
/// @throws std::runtime_error if @p root is not a readable directory.
bool scan(const std::filesystem::path& root, entry& out, const progress_fn& on_progress = {}, progress* totals = nullptr);

/// @brief Resolves a `/`-separated path relative to @p root ("" is @p root
/// itself). Returns nullptr if any component is missing.
const entry* find(const entry& root, std::string_view rel_path);

/// @brief What a treemap node stands for.
enum class node_kind : std::int32_t {
  file = 0,
  dir = 1,
  rest = 2, ///< Several small siblings merged into one node.
};

/// @brief A directory subtree flattened into the parallel arrays the
/// `Treemap` element takes (node 0 is the directory itself; a parent
/// precedes its children).
struct treemap {
  std::vector<std::int32_t> parents;
  std::vector<float> sizes;
  std::vector<std::int32_t> colors; ///< Packed 0xRRGGBBAA; see color_for().
  std::vector<std::int32_t> kinds;  ///< node_kind values.
  std::vector<std::string> labels;
  std::vector<std::uint64_t> bytes;
};

/// @brief Flattens @p dir for display, dropping detail too small to see.
///
/// A child smaller than @p min_fraction of @p dir, and every smaller sibling
/// after it, is merged into one `node_kind::rest` node; so is whatever is
/// left of a directory once @p max_nodes is reached.
treemap build_treemap(const entry& dir, std::size_t max_nodes = 6000, double min_fraction = 1.0 / 2500.0);

/// @brief Packed 0xRRGGBBAA treemap color for a file, chosen by its
/// extension so files of one type share a color (a fixed color for the
/// common media/archive/code/document families, a hashed one otherwise).
std::int32_t color_for(std::string_view file_name);

/// @brief `"1.5 GB"`-style size, in binary units.
std::string format_bytes(std::uint64_t bytes);

/// @brief `1234567` -> `"1,234,567"`.
std::string format_count(std::uint64_t n);

} // namespace bdg::wish::du_scan
