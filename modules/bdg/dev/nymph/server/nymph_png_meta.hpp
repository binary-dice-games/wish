// MIT License © 2026 Binary Dice Games
/// @file nymph_png_meta.hpp
/// @brief PNG encoding with the nymph source embedded as metadata, and
///        reading that source back without decoding any pixels.
///
/// The source is an `iTXt` chunk with keyword `nymph`, placed directly
/// after `IHDR`: stored as-is up to `kInlineSourceBytes`, zlib-compressed
/// above. The description is a second `iTXt` chunk with the standard
/// keyword `Description`. See DESIGN.md "4. Key Abstractions".
#pragma once

#include "nymph_figure_renderer.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace bdg::wish::nymph {

/// @brief Sources up to this size are embedded uncompressed, so plain text
///        tools can read them straight out of the PNG.
inline constexpr size_t kInlineSourceBytes = 64u * 1024u;

/// @brief True if @p bytes starts with the 8-byte PNG signature.
bool is_png(std::string_view bytes);

/// @brief Encode @p img as a PNG carrying @p source and @p description.
/// @return The PNG file bytes.
/// @throws std::runtime_error if @p img is empty or encoding fails.
std::string encode_png(const image& img, std::string_view source, std::string_view description);

/// @brief Read the nymph source embedded in a PNG.
/// @return The source text; `std::nullopt` if @p bytes is not a PNG or has
///         no `nymph` chunk.
/// @throws std::runtime_error if the PNG is truncated, a chunk read on the
///         way fails its CRC, or the embedded text cannot be decompressed.
std::optional<std::string> read_source(std::string_view bytes);

} // namespace bdg::wish::nymph
