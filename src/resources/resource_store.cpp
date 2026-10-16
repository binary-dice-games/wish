// MIT License © 2025 Binary Dice Games
/// @file resource_store.cpp
/// @brief Implementation of the per-session embedded resource extraction.
#include <resource_store.hpp>

#include <miniz.h>
#include <miniz_zip.h>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace bdg::wish {

// Defined in the generated embedded_resources.cpp translation unit.
extern const unsigned char g_resource_archive_data[];
extern const std::size_t g_resource_archive_size;

namespace resource_store {

namespace {

// True if @p path is a regular file of @p size bytes whose CRC-32 is @p crc:
// an earlier extraction into a persistent sandbox that is still current.
bool matches(const std::filesystem::path& path, mz_uint64 size, mz_uint32 crc) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) != size || ec)
    return false;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return bytes.size() == size && mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()) == crc;
}

// Replaces @p out_path with archive entry @p index. The file is extracted
// beside it and renamed over it, because an earlier extraction left the old
// copy read-only (miniz cannot open it for writing) and a concurrent session
// sharing the same persistent sandbox may be reading it.
bool extract_entry(mz_zip_archive& zip, mz_uint index, const std::filesystem::path& out_path) {
  std::error_code ec;
  if (!std::filesystem::exists(out_path, ec))
    return mz_zip_reader_extract_to_file(&zip, index, out_path.string().c_str(), 0);
  auto tmp = out_path;
  tmp += ".extracting";
  std::filesystem::remove(tmp, ec);
  if (!mz_zip_reader_extract_to_file(&zip, index, tmp.string().c_str(), 0))
    return false;
  std::filesystem::rename(tmp, out_path, ec);
  if (ec) {
    // Native Windows will not rename over a read-only file.
    std::filesystem::permissions(out_path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
    std::filesystem::remove(out_path, ec);
    std::filesystem::rename(tmp, out_path, ec);
  }
  if (ec)
    std::filesystem::remove(tmp, ec);
  return !ec;
}

} // namespace

bool extract_to(const std::filesystem::path& dir, std::unordered_map<std::string, uint32_t>* out_crc32) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "wish::resource_store: cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
    return false;
  }

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, g_resource_archive_data, g_resource_archive_size, 0)) {
    std::fprintf(stderr, "wish::resource_store: failed to open embedded archive\n");
    return false;
  }

  bool ok = true;
  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    if (mz_zip_reader_is_file_a_directory(&zip, i))
      continue;

    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      ok = false;
      continue;
    }

    auto out_path = dir / std::filesystem::path{st.m_filename};
    if (matches(out_path, st.m_uncomp_size, st.m_crc32)) {
      if (out_crc32)
        (*out_crc32)[st.m_filename] = static_cast<uint32_t>(st.m_crc32);
      continue;
    }
    std::filesystem::create_directories(out_path.parent_path(), ec);
    if (ec || !extract_entry(zip, i, out_path)) {
      std::fprintf(stderr, "wish::resource_store: failed to extract %s\n", st.m_filename);
      ok = false;
      continue;
    }

    std::filesystem::permissions(
        out_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, ec);
    ok = ok && !ec;

    if (out_crc32)
      (*out_crc32)[st.m_filename] = static_cast<uint32_t>(st.m_crc32);
  }

  mz_zip_reader_end(&zip);
  return ok;
}

} // namespace resource_store

} // namespace bdg::wish
