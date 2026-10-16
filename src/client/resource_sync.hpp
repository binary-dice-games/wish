// MIT License © 2026 Binary Dice Games
/// @file resource_sync.hpp
/// @brief Client-side resource package sync against the server's
///        `__WishFileSystem` service, shared by `wish::client` and
///        `wish::standalone`.
#pragma once

#include "src/bison/bison_object.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish {

/// @brief Outcome of one `sync_resource_package()` call.
struct resource_sync_result {
  std::string name;      ///< Package name passed to the call.
  bool uploaded = false; ///< `true` if the zip was uploaded and unpacked; `false` if already current.
  std::string error;     ///< Empty on success; the failure message otherwise.
};

/// @brief Completion callback of `sync_resource_package()`: invoked exactly
///        once per call, on the sync's worker thread, on success or failure.
using resource_sync_callback = std::function<void(const resource_sync_result&)>;

namespace detail {

/// @brief Chunk size of a resource package upload.
inline constexpr std::size_t kResourceSyncChunkSize = 1u << 20; // 1 MiB

/// @brief Opaque version token of a local zip: `"<mtime ticks>:<size>"`.
///        The server only compares it for equality.
inline std::string resource_version_token(const std::filesystem::path& zip) {
  auto size = std::filesystem::file_size(zip);
  auto ticks = std::filesystem::last_write_time(zip).time_since_epoch().count();
  return std::to_string(ticks) + ":" + std::to_string(size);
}

/**
 * @brief Synchronous resource package sync.
 *
 * Asks the server whether package @p name is current. If not, streams
 * @p zip to `private/packages/<name>.zip` and has the server unpack it into
 * @p dest and record the new version.
 *
 * @param fs           Proxy of the session's `__WishFileSystem` service.
 * @param name         Package name (validated by the server).
 * @param zip          Local zip archive.
 * @param dest         Sandbox-relative destination; empty for the root.
 * @param on_progress  Called after each chunk with bytes sent and the zip
 *                     size; called once with `(size, size)` when current.
 * @param chunk_size   Bytes per `upload_chunk` call.
 * @return `true` if the zip was uploaded, `false` if already current.
 * @throws std::runtime_error if @p zip cannot be read, @p name is invalid,
 *         or the server fails to unpack the archive.
 */
inline bool sync_resource_package_blocking(
    bison::rmi::proxy::dynamic& fs,
    const std::string& name,
    const std::filesystem::path& zip,
    const std::string& dest,
    const std::function<void(std::uint64_t, std::uint64_t)>& on_progress,
    std::size_t chunk_size = kResourceSyncChunkSize) {
  using namespace bison;
  std::ifstream in(zip, std::ios::binary);
  if (!in)
    throw std::runtime_error("wish: cannot open resource package: " + zip.string());
  std::error_code ec;
  auto total = static_cast<std::uint64_t>(std::filesystem::file_size(zip, ec));
  if (ec)
    throw std::runtime_error("wish: cannot read resource package: " + zip.string() + ": " + ec.message());
  auto version = resource_version_token(zip);

  // Checked first: the server validates the name before anything is written.
  {
    dynamic args;
    args["name"_key] = name;
    args["version"_key] = version;
    auto result = fs.call("package_current"_key, std::move(args)).get();
    if (result.get_as<bool>("result"_key, false)) {
      if (on_progress)
        on_progress(total, total);
      return false;
    }
  }

  std::string staging_name = "private/packages/" + name + ".zip";
  std::vector<char> buf(chunk_size);
  std::uint64_t sent = 0;
  bool first = true;
  for (;;) {
    in.read(buf.data(), static_cast<std::streamsize>(chunk_size));
    auto n = static_cast<std::size_t>(in.gcount());
    bool eof = in.eof() || in.peek() == std::char_traits<char>::eof();

    dynamic args;
    args["name"_key] = staging_name;
    args["data"_key] = std::string(buf.data(), n);
    args["first"_key] = first;
    args["eof"_key] = eof;
    fs.call("upload_chunk"_key, std::move(args)).get();

    sent += n;
    if (on_progress)
      on_progress(sent, total);
    first = false;
    if (eof)
      break;
  }

  dynamic args;
  args["name"_key] = name;
  args["version"_key] = version;
  args["dest"_key] = dest;
  fs.call("commit_package"_key, std::move(args)).get();
  return true;
}

/// @brief sync_resource_package_blocking() plus the @p on_done contract:
///        @p on_done runs exactly once, after the sync succeeded or failed;
///        a failure is then rethrown.
inline bool run_resource_sync(
    bison::rmi::proxy::dynamic& fs,
    const std::string& name,
    const std::filesystem::path& zip,
    const std::string& dest,
    const std::function<void(std::uint64_t, std::uint64_t)>& on_progress,
    const resource_sync_callback& on_done) {
  resource_sync_result result{name, false, {}};
  try {
    result.uploaded = sync_resource_package_blocking(fs, name, zip, dest, on_progress);
  } catch (const std::exception& e) {
    result.error = e.what();
    if (on_done)
      on_done(result);
    throw;
  }
  if (on_done)
    on_done(result);
  return result.uploaded;
}

} // namespace detail
} // namespace bdg::wish
