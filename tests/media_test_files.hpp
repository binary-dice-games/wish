// MIT License © 2026 Binary Dice Games
/// @file media_test_files.hpp
/// @brief Writes small, known media files for the media playback tests: a
///        WAV tone (plain C++) and a red-then-green video (libavcodec).
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wish_test {

/// @brief Writes a 16-bit stereo 48 kHz WAV sine tone of @p seconds.
inline void write_wav(const std::filesystem::path& path, double seconds, double freq = 440.0) {
  const uint32_t rate = 48000;
  const uint16_t channels = 2;
  const auto frames = static_cast<uint32_t>(seconds * rate);
  const uint32_t data_bytes = frames * channels * 2;
  std::ofstream out(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  out.write("RIFF", 4);
  u32(36 + data_bytes);
  out.write("WAVEfmt ", 8);
  u32(16);
  u16(1); // PCM
  u16(channels);
  u32(rate);
  u32(rate * channels * 2);
  u16(channels * 2);
  u16(16);
  out.write("data", 4);
  u32(data_bytes);
  for (uint32_t i = 0; i < frames; ++i) {
    const auto s = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * freq * i / rate) * 8000.0);
    u16(static_cast<uint16_t>(s));
    u16(static_cast<uint16_t>(s));
  }
}

/**
 * @brief Writes a Matroska/MPEG-4 video: @p red_frames solid red frames,
 *        then @p green_frames solid green ones, at @p fps, with a keyframe
 *        every second (so a seek lands exactly where the color changes).
 */
inline void write_red_green_video(
    const std::filesystem::path& path, int width, int height, int fps, int red_frames, int green_frames) {
  auto check = [](int err, const char* what) {
    if (err < 0)
      throw std::runtime_error(std::string("write_red_green_video: ") + what);
  };
  AVFormatContext* fmt = nullptr;
  check(avformat_alloc_output_context2(&fmt, nullptr, "matroska", path.string().c_str()), "output");
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  if (!codec)
    throw std::runtime_error("write_red_green_video: no MPEG-4 encoder");
  AVStream* st = avformat_new_stream(fmt, nullptr);
  AVCodecContext* enc = avcodec_alloc_context3(codec);
  enc->width = width;
  enc->height = height;
  enc->pix_fmt = AV_PIX_FMT_YUV420P;
  enc->time_base = AVRational{1, fps};
  enc->framerate = AVRational{fps, 1};
  enc->gop_size = fps;
  enc->max_b_frames = 0;
  enc->bit_rate = 400000;
  if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  check(avcodec_open2(enc, codec, nullptr), "open encoder");
  check(avcodec_parameters_from_context(st->codecpar, enc), "parameters");
  st->time_base = enc->time_base;
  check(avio_open(&fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE), "open file");
  check(avformat_write_header(fmt, nullptr), "header");

  AVFrame* frame = av_frame_alloc();
  frame->format = enc->pix_fmt;
  frame->width = width;
  frame->height = height;
  check(av_frame_get_buffer(frame, 0), "frame");
  AVPacket* pkt = av_packet_alloc();
  auto drain = [&] {
    while (avcodec_receive_packet(enc, pkt) == 0) {
      av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
      pkt->stream_index = st->index;
      check(av_interleaved_write_frame(fmt, pkt), "write");
    }
  };
  for (int i = 0; i < red_frames + green_frames; ++i) {
    check(av_frame_make_writable(frame), "writable");
    const bool red = i < red_frames;
    // BT.601 limited range: red (81, 90, 240), green (145, 54, 34).
    const uint8_t y = red ? 81 : 145, u = red ? 90 : 54, v = red ? 240 : 34;
    for (int row = 0; row < height; ++row)
      std::memset(frame->data[0] + row * frame->linesize[0], y, width);
    for (int row = 0; row < height / 2; ++row) {
      std::memset(frame->data[1] + row * frame->linesize[1], u, width / 2);
      std::memset(frame->data[2] + row * frame->linesize[2], v, width / 2);
    }
    frame->pts = i;
    check(avcodec_send_frame(enc, frame), "encode");
    drain();
  }
  avcodec_send_frame(enc, nullptr);
  drain();
  av_write_trailer(fmt);
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&enc);
  avio_closep(&fmt->pb);
  avformat_free_context(fmt);
}

} // namespace wish_test
