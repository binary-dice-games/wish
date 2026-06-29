// MIT License © 2025 Binary Dice Games
/// @file resource_store.hpp
/// @brief Read-only store of binary resources compiled into the wish binary.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace bdg::wish {

/// @brief Immutable view over a single embedded resource.
///
/// Backed by static storage; the span is valid for the lifetime of the process.
/// Pass `data.data()` / `data.size()` to APIs (e.g. ImGui font loader) that
/// require a raw pointer and byte count.
struct resource_view {
  std::span<const unsigned char> data; ///< View into static storage.
};

/// @brief Read-only store of binary resources compiled into the wish binary.
///
/// All functions are free functions (no instantiation needed). The backing
/// table is generated at build time from the asset tree in WISH_RESOURCE_DIR.
///
/// Paths use forward slashes and are relative to the asset root, e.g.
/// "icons/folder.png".  The "res://" URI scheme prefix is stripped before
/// lookup; callers may pass either form to find().
namespace resource_store {

/// @brief Return true if @p path starts with the "res://" scheme.
bool is_resource_path(std::string_view path);

/// @brief Strip the "res://" prefix from @p path.
/// @return The bare path (e.g. "icons/folder.png"), or @p path unchanged
///         if it does not start with "res://".
std::string_view strip_scheme(std::string_view path);

/// @brief Look up an embedded resource by its bare path (no "res://" prefix).
/// @param path  Case-sensitive path relative to the asset root.
/// @return A populated resource_view, or std::nullopt if not found.
std::optional<resource_view> find(std::string_view path);

} // namespace resource_store

} // namespace bdg::wish
