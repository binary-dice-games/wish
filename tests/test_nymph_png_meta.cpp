// MIT License © 2026 Binary Dice Games
/// @file test_nymph_png_meta.cpp
/// @brief nymph's PNG metadata: the source text embedded in, and read back
///        from, the image file.
#include <gtest/gtest.h>

#include "modules/bdg/dev/nymph/server/nymph_png_meta.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace nymph = bdg::wish::nymph;

namespace {

nymph::image gradient(int w, int h) {
  nymph::image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = img.rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
      p[0] = static_cast<uint8_t>(x * 7);
      p[1] = static_cast<uint8_t>(y * 11);
      p[2] = static_cast<uint8_t>(x + y);
      p[3] = 255;
    }
  return img;
}

std::string repeated(const std::string& unit, size_t bytes) {
  std::string out;
  while (out.size() < bytes)
    out += unit;
  return out;
}

/// Type of the chunk starting at @p pos, and the offset of the next one.
std::string chunk_type(const std::string& png, size_t pos, size_t* next) {
  uint32_t length = 0;
  for (int i = 0; i < 4; ++i)
    length = length << 8 | static_cast<uint8_t>(png[pos + i]);
  *next = pos + 12 + length;
  return png.substr(pos + 4, 4);
}

} // namespace

TEST(NymphPngMeta, SmallSourceRoundTripsAndStaysReadable) {
  const std::string source = "Ünïcode ✓ description\n--\nroot: {type: Plot}\n--\na,b\n1,2\n";
  std::string png = nymph::encode_png(gradient(20, 10), source, "alt text");
  ASSERT_TRUE(nymph::is_png(png));
  auto back = nymph::read_source(png);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, source);
  // Stored as-is, so plain text tools can find it in the file.
  EXPECT_NE(png.find(source), std::string::npos);
}

TEST(NymphPngMeta, LargeSourceRoundTripsCompressed) {
  const std::string source = "d\n--\nroot: {}\n--\nx,y\n" + repeated("12345,67890\n", 1024 * 1024);
  ASSERT_GT(source.size(), nymph::kInlineSourceBytes);
  std::string png = nymph::encode_png(gradient(8, 8), source, "");
  EXPECT_LT(png.size(), source.size() / 4); // repetitive text compresses well
  auto back = nymph::read_source(png);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, source);
}

TEST(NymphPngMeta, EmptySourceRoundTrips) {
  auto back = nymph::read_source(nymph::encode_png(gradient(4, 4), "", ""));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, "");
}

TEST(NymphPngMeta, PixelsSurviveForOrdinaryPngReaders) {
  auto img = gradient(33, 17);
  std::string png = nymph::encode_png(img, "source", "description");
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(png.data()), static_cast<int>(png.size()), &w, &h, &channels, 4);
  ASSERT_NE(pixels, nullptr) << stbi_failure_reason();
  EXPECT_EQ(w, 33);
  EXPECT_EQ(h, 17);
  EXPECT_EQ(std::memcmp(pixels, img.rgba.data(), img.rgba.size()), 0);
  stbi_image_free(pixels);
}

TEST(NymphPngMeta, SourceChunkComesRightAfterIhdr) {
  std::string png = nymph::encode_png(gradient(4, 4), "the source", "the description");
  size_t pos = 8, next = 0;
  EXPECT_EQ(chunk_type(png, pos, &next), "IHDR");
  pos = next;
  EXPECT_EQ(chunk_type(png, pos, &next), "iTXt");
  EXPECT_EQ(png.compare(pos + 8, 6, std::string("nymph\0", 6)), 0);
  pos = next;
  EXPECT_EQ(chunk_type(png, pos, &next), "iTXt");
  EXPECT_EQ(png.compare(pos + 8, 12, std::string("Description\0", 12)), 0);
  EXPECT_NE(png.find("the description"), std::string::npos);
}

TEST(NymphPngMeta, NoDescriptionChunkWhenDescriptionIsEmpty) {
  std::string png = nymph::encode_png(gradient(4, 4), "s", "");
  EXPECT_EQ(png.find("Description"), std::string::npos);
}

TEST(NymphPngMeta, NotANymphImage) {
  EXPECT_FALSE(nymph::read_source("plain text, not a PNG").has_value());
  EXPECT_FALSE(nymph::read_source("").has_value());
  // A real PNG that simply has no nymph chunk: cut ours back out.
  std::string png = nymph::encode_png(gradient(4, 4), "s", "");
  size_t next = 0;
  chunk_type(png, 33, &next);
  png.erase(33, next - 33);
  EXPECT_FALSE(nymph::read_source(png).has_value());
}

TEST(NymphPngMeta, CorruptOrTruncatedPngThrows) {
  std::string png = nymph::encode_png(gradient(4, 4), "the source", "");
  std::string flipped = png;
  flipped[png.find("the source") + 2] ^= 0x20; // payload no longer matches its CRC
  EXPECT_THROW(nymph::read_source(flipped), std::runtime_error);
  EXPECT_THROW(nymph::read_source(png.substr(0, 40)), std::runtime_error);
}

TEST(NymphPngMeta, RejectsEmptyImage) {
  EXPECT_THROW(nymph::encode_png(nymph::image{}, "s", ""), std::runtime_error);
}
