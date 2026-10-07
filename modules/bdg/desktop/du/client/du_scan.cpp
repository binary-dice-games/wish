// MIT License © 2026 Binary Dice Games
/// @file du_scan.cpp
/// @brief Directory scanning and treemap flattening for the du embedded app.
#include "modules/bdg/desktop/du/client/du_scan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace bdg::wish::du_scan {

namespace {

namespace fs = std::filesystem;

/// @brief Identifies the filesystem @p path lives on, so the scan can stop
/// at mount points. std::filesystem has no portable equivalent; on Windows
/// (where a volume is a drive letter, already outside any scanned folder
/// short of a junction) every path reports the same id.
std::uint64_t device_of(const fs::path& path) {
#if defined(_WIN32)
  (void)path;
  return 0;
#else
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
#endif
}

struct scanner {
  const progress_fn& on_progress;
  std::uint64_t device;
  progress totals;

  /// @brief Fills @p dir (whose name is already set) from @p path. Returns
  /// false once cancelled.
  bool read(const fs::path& path, entry& dir) {
    totals.current = path.string();
    if (on_progress && !on_progress(totals))
      return false;

    std::error_code ec;
    fs::directory_iterator it{path, fs::directory_options::skip_permission_denied, ec};
    if (ec) {
      ++totals.errors;
      return true;
    }
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) {
        ++totals.errors;
        break;
      }
      entry child;
      child.name = it->path().filename().string();
      ++totals.items;

      std::error_code type_ec;
      const fs::file_status status = it->symlink_status(type_ec);
      if (!type_ec && fs::is_directory(status)) {
        child.is_dir = true;
        if (device_of(it->path()) == device && !read(it->path(), child))
          return false;
        dir.dirs += child.dirs + 1;
      } else {
        if (!type_ec && fs::is_regular_file(status)) {
          std::error_code size_ec;
          const std::uintmax_t size = it->file_size(size_ec);
          if (!size_ec)
            child.size = size;
        }
        totals.bytes += child.size;
        dir.files += 1;
      }
      dir.files += child.files;
      dir.size += child.size;
      dir.children.push_back(std::move(child));
    }

    std::sort(dir.children.begin(), dir.children.end(), [](const entry& a, const entry& b) {
      return a.size != b.size ? a.size > b.size : a.name < b.name;
    });
    dir.children.shrink_to_fit();
    return true;
  }
};

std::int32_t pack(int r, int g, int b) {
  return static_cast<std::int32_t>(
      (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
      (static_cast<std::uint32_t>(b) << 8) | 0xFFu);
}

constexpr std::int32_t kDirColor = 0x5A6270FF;  // a directory shown without its contents
constexpr std::int32_t kRestColor = 0x6E6E6EFF; // merged small items

struct family {
  std::int32_t color;
  const char* extensions; ///< Space-separated, with a leading and trailing space.
};

/// Colors of the well-known file families; anything else is hashed.
constexpr family kFamilies[] = {
    {0x4C9BE0FF, " jpg jpeg png gif bmp webp tif tiff svg heic raw psd ico "},
    {static_cast<std::int32_t>(0xB46CD8FF), " mp4 mkv avi mov wmv webm flv m4v mpg mpeg "},
    {0x3FBF9AFF, " mp3 flac wav ogg m4a aac opus wma "},
    {static_cast<std::int32_t>(0xE0A63CFF), " zip gz tgz xz bz2 7z rar tar zst deb rpm jar iso img "},
    {static_cast<std::int32_t>(0xD9605AFF), " exe dll so dylib a lib o obj bin pdb sys msi appimage "},
    {0x6CC25BFF, " c cc cpp cxx h hpp py js ts rs go java cs kt rb sh php swift lua html css cmake "},
    {static_cast<std::int32_t>(0xD8C24AFF), " pdf doc docx xls xlsx ppt pptx odt ods md txt rtf epub "},
    {0x58B8C8FF, " json xml yaml yml csv sql db sqlite log dat parquet toml ini "},
};

struct flattener {
  treemap& out;
  std::size_t max_nodes;
  double threshold; // bytes

  std::int32_t add(std::int32_t parent, std::string label, std::uint64_t size, node_kind kind, std::int32_t color) {
    // A newline would split the Treemap element's newline-separated labels.
    std::replace(label.begin(), label.end(), '\n', ' ');
    out.parents.push_back(parent);
    out.sizes.push_back(static_cast<float>(size));
    out.colors.push_back(color);
    out.kinds.push_back(static_cast<std::int32_t>(kind));
    out.labels.push_back(std::move(label));
    out.bytes.push_back(size);
    return static_cast<std::int32_t>(out.parents.size() - 1);
  }

  void add_children(const entry& dir, std::int32_t index) {
    for (std::size_t i = 0; i < dir.children.size(); ++i) {
      const entry& child = dir.children[i];
      if (child.size == 0)
        break; // sorted largest first: nothing drawable is left.
      if (static_cast<double>(child.size) < threshold || out.parents.size() + 1 >= max_nodes) {
        std::uint64_t rest = 0;
        std::size_t count = 0;
        for (std::size_t k = i; k < dir.children.size() && dir.children[k].size > 0; ++k, ++count)
          rest += dir.children[k].size;
        if (count == 1 && !child.is_dir)
          add(index, child.name, child.size, node_kind::file, color_for(child.name));
        else
          add(index, "(" + format_count(count) + " smaller items)", rest, node_kind::rest, kRestColor);
        return;
      }
      if (child.is_dir) {
        const std::int32_t node = add(index, child.name, child.size, node_kind::dir, kDirColor);
        add_children(child, node);
      } else {
        add(index, child.name, child.size, node_kind::file, color_for(child.name));
      }
    }
  }
};

} // namespace

bool scan(const fs::path& root, entry& out, const progress_fn& on_progress, progress* totals) {
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    throw std::runtime_error("not a directory: " + root.string());
  fs::directory_iterator probe{root, ec};
  if (ec)
    throw std::runtime_error("cannot read " + root.string() + ": " + ec.message());

  out = entry{};
  out.name = root.string();
  out.is_dir = true;
  scanner s{on_progress, device_of(root), {}};
  const bool completed = s.read(root, out);
  if (totals)
    *totals = s.totals;
  return completed;
}

const entry* find(const entry& root, std::string_view rel_path) {
  const entry* cur = &root;
  while (!rel_path.empty()) {
    const std::size_t slash = rel_path.find('/');
    const std::string_view name = rel_path.substr(0, slash);
    rel_path = slash == std::string_view::npos ? std::string_view{} : rel_path.substr(slash + 1);
    if (name.empty())
      continue;
    const auto it = std::find_if(cur->children.begin(), cur->children.end(), [&](const entry& e) {
      return e.name == name;
    });
    if (it == cur->children.end())
      return nullptr;
    cur = &*it;
  }
  return cur;
}

treemap build_treemap(const entry& dir, std::size_t max_nodes, double min_fraction) {
  treemap out;
  flattener f{out, std::max<std::size_t>(max_nodes, 2), static_cast<double>(dir.size) * min_fraction};
  const std::size_t slash = dir.name.find_last_of("/\\");
  const std::string label = slash == std::string::npos || slash + 1 >= dir.name.size() ? dir.name
                                                                                      : dir.name.substr(slash + 1);
  f.add(-1, label, dir.size, node_kind::dir, kDirColor);
  f.add_children(dir, 0);
  return out;
}

std::int32_t color_for(std::string_view file_name) {
  const std::size_t dot = file_name.find_last_of('.');
  std::string ext;
  if (dot != std::string_view::npos && dot > 0)
    ext = std::string{file_name.substr(dot + 1)};
  for (char& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext.empty())
    return static_cast<std::int32_t>(0x9AA3B0FF);

  const std::string needle = " " + ext + " ";
  for (const family& f : kFamilies)
    if (std::string_view{f.extensions}.find(needle) != std::string_view::npos)
      return f.color;

  // FNV-1a over the extension picks a stable mid-brightness color.
  std::uint32_t h = 2166136261u;
  for (unsigned char c : ext)
    h = (h ^ c) * 16777619u;
  return pack(90 + static_cast<int>(h & 0x7F), 90 + static_cast<int>((h >> 8) & 0x7F),
              90 + static_cast<int>((h >> 16) & 0x7F));
}

std::string format_bytes(std::uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
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
    digits.insert(static_cast<std::size_t>(i), 1, ',');
  return digits;
}

} // namespace bdg::wish::du_scan
