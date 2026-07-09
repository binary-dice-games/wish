// MIT License © 2025 Binary Dice Games
/// @file resource_store.cpp
/// @brief Implementation of the per-session embedded resource extraction.
#include <resource_store.hpp>

#include <miniz.h>
#include <miniz_zip.h>

#include <cstddef>
#include <cstdio>
#include <system_error>

namespace bdg::wish {

// Defined in the generated embedded_resources.cpp translation unit.
extern const unsigned char g_resource_archive_data[];
extern const std::size_t g_resource_archive_size;

namespace resource_store {

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
    std::filesystem::create_directories(out_path.parent_path(), ec);
    if (ec || !mz_zip_reader_extract_to_file(&zip, i, out_path.string().c_str(), 0)) {
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
