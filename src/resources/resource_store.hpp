// MIT License © 2025 Binary Dice Games
/// @file resource_store.hpp
/// @brief Access to the embedded resource archive compiled into the wish
///        binary, and the routine that unpacks it per-session.
#pragma once

#include <filesystem>

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
/// @param dir  Destination directory.
/// @return true if every entry was extracted successfully.
bool extract_to(const std::filesystem::path& dir);

} // namespace bdg::wish::resource_store
