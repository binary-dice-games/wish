// MIT License © 2025 Binary Dice Games
/// @file zip.cpp
/// @brief Client-side runner for the zip embedded app.
///
/// The Zip form (server-side) owns and renders the browser, but has no
/// access to the client's local machine at all. This runner is the bridge:
/// it reacts to the form's `on_navigate` event by enumerating a local
/// directory and reporting it back via `update_listing()`, and to
/// `on_compress_requested`/`on_extract_requested`/
/// `on_view_contents_requested` by doing the actual zip I/O against the
/// local filesystem with miniz -- the same library wish_server itself uses
/// to unpack embedded resources (see src/context/file_service.cpp), here
/// linked into the client instead.
#include "modules/bdg/desktop/zip/client/zip.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <miniz.h>
#include <miniz_zip.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

std::string format_bytes(uintmax_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << kUnits[unit];
  return oss.str();
}

std::string format_modified(const fs::file_time_type& ftime) {
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M");
  return oss.str();
}

// Enumerate `dir` and push the listing to the server via update_listing(),
// the shape Zip::do_update_listing() expects:
// {path, files: [{name, type, size, modified}, ...]}.
void report_listing(const std::shared_ptr<rmi::proxy::dynamic>& tool, const std::shared_ptr<fs::path>& cur_dir) {
  dynamic files;
  size_t i = 0;
  if (cur_dir->has_parent_path() && *cur_dir != cur_dir->root_path()) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = std::string{".."};
    (*e)["type"_key] = std::string{"dir"};
    (*e)["size"_key] = std::string{};
    (*e)["modified"_key] = std::string{};
    files[i++] = dynamic_ptr{e};
  }
  std::error_code ec;
  for (auto& dirent : fs::directory_iterator{*cur_dir, ec}) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = dirent.path().filename().string();
    bool is_dir = dirent.is_directory(ec);
    (*e)["type"_key] = is_dir ? std::string{"dir"} : std::string{"file"};
    (*e)["size"_key] = is_dir ? std::string{} : format_bytes(dirent.file_size(ec));
    auto ftime = dirent.last_write_time(ec);
    (*e)["modified"_key] = ec ? std::string{} : format_modified(ftime);
    files[i++] = dynamic_ptr{e};
  }

  dynamic args;
  args["path"_key] = cur_dir->string();
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};
  tool->call("update_listing"_key, std::move(args)).get();
}

void report_status(const std::shared_ptr<rmi::proxy::dynamic>& tool, const std::string& message) {
  dynamic patch;
  patch["status"_key] = message;
  tool->set(std::move(patch)).get();
}

// Recursively adds `source` (a file or directory) into an already-open
// zip writer under `entry_name` -- for a directory, every nested entry is
// named "<entry_name>/...", i.e. the top folder itself is included, matching
// the common "right-click > Compress" convention (macOS Archive Utility,
// Windows "Send to > Compressed folder") rather than flattening its
// contents to the archive root.
void add_to_zip(mz_zip_archive& zip, const fs::path& source, const std::string& entry_name) {
  std::error_code ec;
  if (fs::is_directory(source, ec)) {
    bool any = false;
    for (auto& dirent : fs::recursive_directory_iterator{source, ec}) {
      any = true;
      std::string rel = (fs::path(entry_name) / fs::relative(dirent.path(), source, ec)).generic_string();
      if (dirent.is_directory(ec)) {
        if (!mz_zip_writer_add_mem(&zip, (rel + "/").c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION))
          throw std::runtime_error("failed to add folder: " + rel);
      } else if (!mz_zip_writer_add_file(
                     &zip, rel.c_str(), dirent.path().string().c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
        throw std::runtime_error("failed to add: " + rel);
      }
    }
    // Preserve an empty source directory as a single directory entry, so
    // the archive isn't left completely empty.
    if (!any && !mz_zip_writer_add_mem(&zip, (entry_name + "/").c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION))
      throw std::runtime_error("failed to add folder: " + entry_name);
  } else if (!mz_zip_writer_add_file(
                 &zip, entry_name.c_str(), source.string().c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION)) {
    throw std::runtime_error("failed to add: " + entry_name);
  }
}

// Creates `dir/archive_name` containing `dir/source_name` (recursively, if
// it is a directory). Throws std::runtime_error on failure.
void compress(const fs::path& dir, const std::string& source_name, const std::string& archive_name) {
  fs::path source_full = dir / source_name;
  fs::path archive_full = dir / archive_name;

  // The server already rejects an archive_name equal to source_name before
  // ever emitting the request (see zip.cpp (server)'s
  // on_prompt_confirmed()), but source_full/archive_full are always two
  // sibling paths built from the same `dir` here, so a plain path compare
  // is enough of a defense-in-depth check without touching the filesystem.
  if (source_full == archive_full)
    throw std::runtime_error("archive name must differ from the source");

  std::error_code ec;
  if (!fs::exists(source_full, ec))
    throw std::runtime_error("source not found: " + source_name);

  mz_zip_archive zip{};
  if (!mz_zip_writer_init_file(&zip, archive_full.string().c_str(), 0))
    throw std::runtime_error("cannot create archive: " + archive_name);

  try {
    add_to_zip(zip, source_full, source_name);
  } catch (...) {
    mz_zip_writer_end(&zip);
    throw;
  }

  if (!mz_zip_writer_finalize_archive(&zip)) {
    mz_zip_writer_end(&zip);
    throw std::runtime_error("failed to finalize archive: " + archive_name);
  }
  mz_zip_writer_end(&zip);
}

// Extracts `dir/zip_name` into `dir/dest_name`, creating the destination if
// needed and merging into it (overwriting individual files) otherwise.
// Throws std::runtime_error on failure.
void extract(const fs::path& dir, const std::string& zip_name, const std::string& dest_name) {
  fs::path zip_full = dir / zip_name;
  fs::path dest_full = dir / dest_name;

  std::error_code ec;
  if (fs::exists(dest_full, ec) && !fs::is_directory(dest_full, ec))
    throw std::runtime_error("destination exists and is not a folder: " + dest_name);

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, zip_full.string().c_str(), 0))
    throw std::runtime_error("cannot open archive: " + zip_name);

  fs::create_directories(dest_full, ec);

  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      mz_zip_reader_end(&zip);
      throw std::runtime_error("corrupt archive entry in: " + zip_name);
    }

    // The local disk isn't a wish session sandbox, but a zip-slip guard is
    // still worth applying here: this archive may have come from anywhere
    // (downloaded, emailed, ...), and its entries are exactly as untrusted
    // as any other zip a desktop unarchiver might be pointed at. Mirrors
    // file_service::unpack()'s own lexical escape check, rooted at
    // dest_full instead of a session resource_dir.
    fs::path target = (dest_full / st.m_filename).lexically_normal();
    auto rel = target.lexically_relative(dest_full);
    if (rel.empty() || *rel.begin() == fs::path("..")) {
      mz_zip_reader_end(&zip);
      throw std::runtime_error("archive entry escapes destination: " + std::string{st.m_filename});
    }

    if (mz_zip_reader_is_file_a_directory(&zip, i)) {
      fs::create_directories(target, ec);
      continue;
    }

    fs::create_directories(target.parent_path(), ec);
    if (!mz_zip_reader_extract_to_file(&zip, i, target.string().c_str(), 0)) {
      mz_zip_reader_end(&zip);
      throw std::runtime_error("failed to extract: " + std::string{st.m_filename});
    }
  }

  mz_zip_reader_end(&zip);
  // Deliberately keeps zip_full on disk: a zip the user is browsing and
  // chose to extract is theirs to keep, unlike the server-side
  // file_service::unpack() flow (built for "upload a package, extract it,
  // discard the staging archive").
}

// Reads `zip_full`'s central directory without extracting anything, for the
// View Contents dialog. Throws std::runtime_error if the archive can't be
// opened.
dynamic list_contents(const fs::path& zip_full) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, zip_full.string().c_str(), 0))
    throw std::runtime_error(zip_full.filename().string());

  dynamic entries;
  size_t idx = 0;
  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st))
      continue;
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = std::string{st.m_filename};
    (*e)["type"_key] = mz_zip_reader_is_file_a_directory(&zip, i) ? std::string{"dir"} : std::string{"file"};
    // Cast down to int32_t: bison::dynamic's field variant has no int64_t
    // alternative, only int32_t among integer types (see
    // file_service.cpp's download_chunk() RMI handler, which narrows its
    // own uint64_t total the same way for the same reason).
    (*e)["uncompressed_size"_key] = static_cast<int32_t>(st.m_uncomp_size);
    (*e)["compressed_size"_key] = static_cast<int32_t>(st.m_comp_size);
    entries[idx++] = dynamic_ptr{e};
  }
  mz_zip_reader_end(&zip);
  return entries;
}

} // namespace

void run_zip(wish_app_host& s) {
  auto tool = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Zip"_key).get());
  auto cur_dir = std::make_shared<fs::path>(fs::current_path());

  // Server asks to browse a different directory (row activated, or the path
  // bar's value was changed) -- mirrors mc's on_local_navigate.
  tool->onEvent("on_navigate"_key, [tool, cur_dir](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);
    auto type = payload.as<std::string>("type"_key);
    fs::path target = type == "path" ? fs::path(name) : (name == ".." ? cur_dir->parent_path() : (*cur_dir / name));
    std::error_code ec;
    if (!fs::is_directory(target, ec))
      return;
    *cur_dir = target;
    report_listing(tool, cur_dir);
  });

  // "Compress" confirmed server-side: create the archive, then report the
  // outcome and refresh the listing so the new file appears.
  //
  // Runs on a detached background thread rather than inline in this
  // handler: in standalone mode the handler runs on the RMI dispatch thread
  // while holding the session's write lock (see
  // bdg::bison::rmi::standalone's "do not block the worker from within an
  // event handler" contract), and a large folder could take a while to
  // compress. Blocking here would freeze the entire UI.
  tool->onEvent("on_compress_requested"_key, [tool, cur_dir](dynamic payload) {
    auto path = payload.as<std::string>("path"_key);
    auto source_name = payload.as<std::string>("source_name"_key);
    auto archive_name = payload.as<std::string>("archive_name"_key);

    std::thread([tool, cur_dir, path, source_name, archive_name]() {
      std::string message;
      try {
        compress(fs::path(path), source_name, archive_name);
        message = "Created \"" + archive_name + "\".";
      } catch (const std::exception& e) {
        message = std::string{"Compress failed: "} + e.what();
      }
      // Refresh regardless of outcome -- even a failed attempt may have
      // left a partial archive file behind. update_listing()'s handler
      // unconditionally resets the status label to "Ready.", so the
      // outcome message must be reported *after* the refresh, not before,
      // or it would be immediately overwritten and never seen.
      if (*cur_dir == fs::path(path))
        report_listing(tool, cur_dir);
      report_status(tool, message);
    }).detach();
  });

  // "Extract" confirmed server-side: same threading rationale as compress.
  tool->onEvent("on_extract_requested"_key, [tool, cur_dir](dynamic payload) {
    auto path = payload.as<std::string>("path"_key);
    auto zip_name = payload.as<std::string>("zip_name"_key);
    auto dest_name = payload.as<std::string>("dest_name"_key);

    std::thread([tool, cur_dir, path, zip_name, dest_name]() {
      std::string message;
      try {
        extract(fs::path(path), zip_name, dest_name);
        message = "Extracted to \"" + dest_name + "/\".";
      } catch (const std::exception& e) {
        message = std::string{"Extract failed: "} + e.what();
      }
      // See the compress handler's comment above: report the outcome after
      // the refresh, not before, so update_listing()'s "Ready." reset
      // doesn't clobber it.
      if (*cur_dir == fs::path(path))
        report_listing(tool, cur_dir);
      report_status(tool, message);
    }).detach();
  });

  // "View Contents" clicked (button, or double-clicking a .zip row): read
  // the archive's central directory without extracting, then hand the
  // listing to the server to render.
  tool->onEvent("on_view_contents_requested"_key, [tool](dynamic payload) {
    auto path = payload.as<std::string>("path"_key);
    auto name = payload.as<std::string>("name"_key);

    std::thread([tool, path, name]() {
      try {
        dynamic entries = list_contents(fs::path(path) / name);
        dynamic args;
        args["name"_key] = name;
        args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(entries))};
        tool->call("show_contents"_key, std::move(args)).get();
      } catch (const std::exception& e) {
        report_status(tool, std::string{"Could not open archive: "} + e.what());
      }
    }).detach();
  });

  tool->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  // Show the client's current working directory at startup, mirroring
  // mc's own root-on-open behavior for its local panel.
  report_listing(tool, cur_dir);

  // on_session() blocks until signal_done() is called.
}

namespace {
struct zip_app_registrar {
  zip_app_registrar() {
    register_app({
        .name = "zip",
        .organization = WISH_MODULE_BDG_DESKTOP_ZIP_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_ZIP_COLLECTION,
        .description = "Zip/unzip tool: browse the local filesystem, compress/extract/view zip contents",
        .params = {},
        .run = run_zip,
    });
  }
};
const zip_app_registrar zip_app_registrar_instance;
} // namespace

} // namespace bdg::wish
