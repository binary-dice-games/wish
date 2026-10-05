// MIT License © 2026 Binary Dice Games
/// @file nymph_png_meta.cpp
/// @brief Implementation of nymph's PNG metadata reader/writer.
#include "nymph_png_meta.hpp"

#include "nymph_document.hpp"

#include <miniz.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace bdg::wish::nymph {

namespace {

constexpr std::string_view kSignature{"\x89PNG\r\n\x1a\n", 8};
constexpr std::string_view kSourceKeyword{"nymph"};
constexpr std::string_view kDescriptionKeyword{"Description"};
constexpr size_t kIhdrEnd = 8 + 4 + 4 + 13 + 4; // signature + the IHDR chunk

void put_u32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v >> 24));
  out.push_back(static_cast<char>(v >> 16));
  out.push_back(static_cast<char>(v >> 8));
  out.push_back(static_cast<char>(v));
}

uint32_t get_u32(std::string_view bytes, size_t at) {
  auto b = [&](size_t i) { return static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + i])); };
  return b(0) << 24 | b(1) << 16 | b(2) << 8 | b(3);
}

uint32_t crc32(std::string_view bytes) {
  return static_cast<uint32_t>(
      mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

/// A complete iTXt chunk (length, type, data, CRC) holding @p text.
std::string text_chunk(std::string_view keyword, std::string_view text, bool compress) {
  std::string body{"iTXt"};
  body += keyword;
  body.push_back('\0');
  body.push_back(compress ? '\1' : '\0'); // compression flag
  body.push_back('\0');                   // compression method: zlib
  body.push_back('\0');                   // empty language tag
  body.push_back('\0');                   // empty translated keyword
  if (compress) {
    mz_ulong packed_size = mz_compressBound(static_cast<mz_ulong>(text.size()));
    std::string packed(packed_size, '\0');
    if (mz_compress(reinterpret_cast<unsigned char*>(packed.data()), &packed_size,
            reinterpret_cast<const unsigned char*>(text.data()), static_cast<mz_ulong>(text.size())) != MZ_OK)
      throw std::runtime_error("nymph: could not compress the embedded source");
    body.append(packed.data(), packed_size);
  } else {
    body += text;
  }
  std::string chunk;
  put_u32(chunk, static_cast<uint32_t>(body.size() - 4));
  chunk += body;
  put_u32(chunk, crc32(body));
  return chunk;
}

void append_bytes(void* context, void* data, int size) {
  static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

/// The text of an iTXt chunk's data if its keyword is @p keyword.
std::optional<std::string> text_of(std::string_view data, std::string_view keyword) {
  if (data.size() < keyword.size() + 3 || data.substr(0, keyword.size()) != keyword || data[keyword.size()] != '\0')
    return std::nullopt;
  size_t pos = keyword.size() + 1;
  const bool compressed = data[pos] != '\0';
  pos += 2; // compression flag + method
  for (int field = 0; field < 2; ++field) { // language tag, translated keyword
    size_t end = data.find('\0', pos);
    if (end == std::string_view::npos)
      throw std::runtime_error("nymph: malformed text chunk in PNG");
    pos = end + 1;
  }
  std::string_view payload = data.substr(pos);
  if (!compressed)
    return std::string(payload);
  // Decompressed into a bounded buffer, so a crafted chunk cannot make this
  // allocate without limit.
  std::string text(kMaxSourceBytes, '\0');
  size_t out_size = tinfl_decompress_mem_to_mem(
      text.data(), text.size(), payload.data(), payload.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
  if (out_size == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
    throw std::runtime_error("nymph: could not decompress the embedded source (corrupt, or larger than 16 MiB)");
  text.resize(out_size);
  return text;
}

} // namespace

bool is_png(std::string_view bytes) {
  return bytes.substr(0, kSignature.size()) == kSignature;
}

std::string encode_png(const image& img, std::string_view source, std::string_view description) {
  if (img.width <= 0 || img.height <= 0 ||
      img.rgba.size() != static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4)
    throw std::runtime_error("nymph: cannot encode an empty image");

  std::string png;
  if (!stbi_write_png_to_func(append_bytes, &png, img.width, img.height, 4, img.rgba.data(), img.width * 4) ||
      png.size() < kIhdrEnd || !is_png(png))
    throw std::runtime_error("nymph: PNG encoding failed");

  std::string chunks = text_chunk(kSourceKeyword, source, source.size() > kInlineSourceBytes);
  if (!description.empty())
    chunks += text_chunk(kDescriptionKeyword, description, false);
  png.insert(kIhdrEnd, chunks);
  return png;
}

std::optional<std::string> read_source(std::string_view bytes) {
  if (!is_png(bytes))
    return std::nullopt;
  size_t pos = kSignature.size();
  while (pos < bytes.size()) {
    if (bytes.size() - pos < 12)
      throw std::runtime_error("nymph: truncated PNG");
    const size_t length = get_u32(bytes, pos);
    if (length > bytes.size() - pos - 12)
      throw std::runtime_error("nymph: truncated PNG");
    std::string_view type_and_data = bytes.substr(pos + 4, 4 + length);
    if (crc32(type_and_data) != get_u32(bytes, pos + 8 + length))
      throw std::runtime_error("nymph: corrupt PNG (bad chunk checksum)");
    std::string_view type = type_and_data.substr(0, 4);
    if (type == "iTXt") {
      if (auto text = text_of(type_and_data.substr(4), kSourceKeyword))
        return text;
    } else if (type == "IEND") {
      break;
    }
    pos += 12 + length;
  }
  return std::nullopt;
}

} // namespace bdg::wish::nymph
