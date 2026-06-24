// MIT License © 2025 Binary Dice Games
/// @file resource_entry.hpp
/// @brief Internal struct shared between embedded_resources.cpp (generated)
///        and resource_store.cpp.  Do not include from public headers.
#pragma once

#include <cstddef>

namespace bdg::wish {

/// @brief A single row in the compile-time resource table.
struct resource_entry {
  const char*          path;  ///< Null-terminated key, e.g. "icons/folder.png".
  const unsigned char* data;  ///< Pointer to static byte array.
  std::size_t          size;  ///< Byte count.
};

} // namespace bdg::wish
