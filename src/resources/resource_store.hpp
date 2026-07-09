// MIT License © 2025 Binary Dice Games
/// @file resource_store.hpp
/// @brief Access to the embedded resource archive compiled into the wish
///        binary, and the routine that unpacks it per-session.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace bdg::wish::resource_store {

/// @brief Unpack the embedded resource archive into @p dir.
///
/// Creates @p dir if it does not already exist. Every regular-file entry in
/// the archive becomes a file under @p dir (creating subdirectories as
/// needed), then has its permissions set to read-only (owner/group/other
/// read, no write), so `file_service::upload()` cannot silently overwrite a
/// built-in asset.
///
/// Never throws. Any miniz or filesystem failure is logged to stderr and
/// that entry is skipped; extraction continues with the remaining entries.
/// Callers must treat a `false` return as non-fatal — see the rationale in
/// `context::context()`, which calls this on a thread with no surrounding
/// try/catch.
///
/// @param dir        Destination directory.
/// @param out_crc32  When non-null, populated with one entry per
///                    successfully-extracted file: the archive-relative
///                    path (same string as the extracted file's path under
///                    @p dir) mapped to that file's zip CRC-32 (already
///                    computed by miniz for its own integrity checks, and
///                    surfaced here so callers can use it as a stable
///                    content-version number without recomputing it).
/// @return true if every entry was extracted successfully.
bool extract_to(const std::filesystem::path& dir, std::unordered_map<std::string, uint32_t>* out_crc32 = nullptr);

} // namespace bdg::wish::resource_store
