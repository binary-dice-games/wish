// MIT License © 2026 Binary Dice Games
/// @file media_decoder.cpp
/// @brief Implementation of wish::media::media_decoder on FFmpeg.
#include <media/media_decoder.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <fstream>
#include <stdexcept>
#include <string>

namespace bdg::wish::media {

namespace {

constexpr int kIoBufferSize = 64 * 1024;

std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

[[noreturn]] void fail(const std::string& what, int err) {
  throw std::runtime_error("wish media: " + what + ": " + av_error(err));
}

// ── Custom I/O over one already-opened file ──────────────────────────────────

int read_packet(void* opaque, uint8_t* buf, int size) {
  auto& in = *static_cast<std::ifstream*>(opaque);
  in.read(reinterpret_cast<char*>(buf), size);
  const auto got = static_cast<int>(in.gcount());
  if (got <= 0)
    return AVERROR_EOF;
  return got;
}

int64_t seek_stream(void* opaque, int64_t offset, int whence) {
  auto& in = *static_cast<std::ifstream*>(opaque);
  in.clear();
  if (whence & AVSEEK_SIZE) {
    const auto here = in.tellg();
    in.seekg(0, std::ios::end);
    const auto size = static_cast<int64_t>(in.tellg());
    in.seekg(here);
    return size;
  }
  std::ios::seekdir dir = std::ios::beg;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_CUR:
      dir = std::ios::cur;
      break;
    case SEEK_END:
      dir = std::ios::end;
      break;
    default:
      break;
  }
  in.seekg(offset, dir);
  if (!in)
    return -1;
  return static_cast<int64_t>(in.tellg());
}

// Demuxers that reference other resources (HLS, DASH, concat, image
// sequences, ...) open them through this hook: refuse every one, so only
// the validated file is ever read.
int deny_io_open(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
  return AVERROR(EPERM);
}

double to_seconds(int64_t ts, AVRational tb) {
  return ts == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(ts) * av_q2d(tb);
}

} // namespace

// ── impl ─────────────────────────────────────────────────────────────────────

struct media_decoder::impl {
  std::ifstream file;
  AVIOContext* avio = nullptr;
  AVFormatContext* fmt = nullptr;
  int video_index = -1;
  int audio_index = -1;
  AVCodecContext* video_ctx = nullptr;
  AVCodecContext* audio_ctx = nullptr;
  SwsContext* sws = nullptr;
  SwrContext* swr = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  double start_time = 0.0; // container start, subtracted from every pts
  bool demux_eof = false;
  bool video_drained = false;
  bool audio_drained = false;
  media_info info;

  ~impl() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    sws_freeContext(sws);
    avcodec_free_context(&video_ctx);
    avcodec_free_context(&audio_ctx);
    avformat_close_input(&fmt); // also frees an unopened fmt
    if (avio) {
      av_freep(&avio->buffer);
      avio_context_free(&avio);
    }
  }

  AVCodecContext* open_codec(int index) {
    AVStream* st = fmt->streams[index];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
      throw std::runtime_error("wish media: no decoder for this file's codec");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
      throw std::runtime_error("wish media: out of memory");
    int err = avcodec_parameters_to_context(ctx, st->codecpar);
    if (err >= 0) {
      ctx->pkt_timebase = st->time_base;
      av_opt_set(ctx, "threads", "auto", 0);
      err = avcodec_open2(ctx, codec, nullptr);
    }
    if (err < 0) {
      avcodec_free_context(&ctx);
      fail("cannot open decoder", err);
    }
    return ctx;
  }

  void make_resampler() {
    swr_free(&swr);
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int err = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, kAudioRate, &audio_ctx->ch_layout,
        audio_ctx->sample_fmt, audio_ctx->sample_rate, 0, nullptr);
    if (err >= 0)
      err = swr_init(swr);
    if (err < 0)
      fail("cannot convert this audio", err);
  }

  void convert_video(video_frame& out) {
    const int w = frame->width;
    const int h = frame->height;
    sws = sws_getCachedContext(sws, w, h, static_cast<AVPixelFormat>(frame->format), w, h, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
      throw std::runtime_error("wish media: cannot convert this video's pixel format");
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
    int dst_pitch[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dst_pitch);
    const auto tb = fmt->streams[video_index]->time_base;
    out.pts = to_seconds(frame->best_effort_timestamp, tb) - start_time;
  }

  void convert_audio(audio_chunk& out) {
    // A stream may change layout or rate mid-file (e.g. some broadcasts).
    if (!swr || frame->sample_rate != audio_ctx->sample_rate ||
        av_channel_layout_compare(&frame->ch_layout, &audio_ctx->ch_layout) != 0) {
      audio_ctx->sample_rate = frame->sample_rate;
      av_channel_layout_uninit(&audio_ctx->ch_layout);
      av_channel_layout_copy(&audio_ctx->ch_layout, &frame->ch_layout);
      audio_ctx->sample_fmt = static_cast<AVSampleFormat>(frame->format);
      make_resampler();
    }
    const int capacity = swr_get_out_samples(swr, frame->nb_samples);
    out.samples.resize(static_cast<size_t>(capacity) * kAudioChannels);
    uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(out.samples.data())};
    const int got = swr_convert(swr, dst, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (got < 0)
      fail("audio conversion failed", got);
    out.samples.resize(static_cast<size_t>(got) * kAudioChannels);
    const auto tb = fmt->streams[audio_index]->time_base;
    out.pts = to_seconds(frame->best_effort_timestamp, tb) - start_time;
  }

  // Tries to pull one frame from @p ctx; true when one was produced.
  bool receive(AVCodecContext* ctx, bool& drained) {
    if (!ctx || drained)
      return false;
    const int err = avcodec_receive_frame(ctx, frame);
    if (err == AVERROR(EAGAIN))
      return false;
    if (err == AVERROR_EOF) {
      drained = true;
      return false;
    }
    if (err < 0)
      fail("decoding failed", err);
    return true;
  }
};

// ── media_decoder ────────────────────────────────────────────────────────────

media_decoder::media_decoder(const std::filesystem::path& file) : impl_(std::make_unique<impl>()) {
  auto& d = *impl_;
  d.file.open(file, std::ios::binary);
  if (!d.file)
    throw std::runtime_error("wish media: cannot open " + file.filename().string());

  auto* buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
  if (!buffer)
    throw std::runtime_error("wish media: out of memory");
  d.avio = avio_alloc_context(buffer, kIoBufferSize, 0, &d.file, read_packet, nullptr, seek_stream);
  if (!d.avio) {
    av_free(buffer);
    throw std::runtime_error("wish media: out of memory");
  }
  d.fmt = avformat_alloc_context();
  if (!d.fmt)
    throw std::runtime_error("wish media: out of memory");
  d.fmt->pb = d.avio;
  d.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  d.fmt->io_open = deny_io_open;

  int err = avformat_open_input(&d.fmt, "", nullptr, nullptr);
  if (err < 0) {
    d.fmt = nullptr; // avformat_open_input frees it on failure
    fail("not a playable media file", err);
  }
  err = avformat_find_stream_info(d.fmt, nullptr);
  if (err < 0)
    fail("not a playable media file", err);

  d.video_index = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (d.video_index >= 0 && (d.fmt->streams[d.video_index]->disposition & AV_DISPOSITION_ATTACHED_PIC))
    d.video_index = -1; // cover art, not a video
  d.audio_index = av_find_best_stream(d.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (d.video_index < 0 && d.audio_index < 0)
    throw std::runtime_error("wish media: the file has no audio or video");

  if (d.video_index >= 0) {
    const auto* par = d.fmt->streams[d.video_index]->codecpar;
    if (par->width <= 0 || par->height <= 0 || par->width > kMaxVideoSide || par->height > kMaxVideoSide)
      throw std::runtime_error("wish media: unsupported video size");
    d.video_ctx = d.open_codec(d.video_index);
    d.info.has_video = true;
    d.info.width = par->width;
    d.info.height = par->height;
  }
  if (d.audio_index >= 0) {
    d.audio_ctx = d.open_codec(d.audio_index);
    d.make_resampler();
    d.info.has_audio = true;
  }
  // Every other stream (subtitles, data, a second audio track) is skipped
  // by the demuxer instead of being read and dropped.
  for (unsigned i = 0; i < d.fmt->nb_streams; ++i)
    if (static_cast<int>(i) != d.video_index && static_cast<int>(i) != d.audio_index)
      d.fmt->streams[i]->discard = AVDISCARD_ALL;

  if (d.fmt->start_time != AV_NOPTS_VALUE)
    d.start_time = static_cast<double>(d.fmt->start_time) / AV_TIME_BASE;
  if (d.fmt->duration != AV_NOPTS_VALUE && d.fmt->duration > 0)
    d.info.duration = static_cast<double>(d.fmt->duration) / AV_TIME_BASE;

  d.packet = av_packet_alloc();
  d.frame = av_frame_alloc();
  if (!d.packet || !d.frame)
    throw std::runtime_error("wish media: out of memory");
}

media_decoder::~media_decoder() = default;

const media_info& media_decoder::info() const {
  return impl_->info;
}

media_decoder::result media_decoder::decode(video_frame& video, audio_chunk& audio) {
  auto& d = *impl_;
  for (;;) {
    if (d.receive(d.video_ctx, d.video_drained)) {
      d.convert_video(video);
      av_frame_unref(d.frame);
      return result::video;
    }
    if (d.receive(d.audio_ctx, d.audio_drained)) {
      d.convert_audio(audio);
      av_frame_unref(d.frame);
      return result::audio;
    }
    const bool video_done = !d.video_ctx || d.video_drained;
    const bool audio_done = !d.audio_ctx || d.audio_drained;
    if (d.demux_eof) {
      if (video_done && audio_done)
        return result::eof;
      continue; // decoders are flushing; keep receiving
    }

    const int err = av_read_frame(d.fmt, d.packet);
    if (err == AVERROR_EOF || (err < 0 && d.fmt->pb && avio_feof(d.fmt->pb))) {
      d.demux_eof = true;
      if (d.video_ctx)
        avcodec_send_packet(d.video_ctx, nullptr); // start flushing
      if (d.audio_ctx)
        avcodec_send_packet(d.audio_ctx, nullptr);
      continue;
    }
    if (err < 0)
      fail("reading the file failed", err);

    AVCodecContext* ctx = nullptr;
    if (d.packet->stream_index == d.video_index)
      ctx = d.video_ctx;
    else if (d.packet->stream_index == d.audio_index)
      ctx = d.audio_ctx;
    if (ctx) {
      const int sent = avcodec_send_packet(ctx, d.packet);
      // EAGAIN can't happen here (every frame is received before reading
      // on); an invalid packet is skipped like players usually do.
      (void)sent;
    }
    av_packet_unref(d.packet);
  }
}

void media_decoder::seek(double seconds) {
  auto& d = *impl_;
  if (seconds < 0)
    seconds = 0;
  const auto target = static_cast<int64_t>((seconds + d.start_time) * AV_TIME_BASE);
  const int err = av_seek_frame(d.fmt, -1, target, AVSEEK_FLAG_BACKWARD);
  if (err < 0) {
    // Some files can't seek by time (no index): rewind to the start.
    if (avio_seek(d.fmt->pb, 0, SEEK_SET) < 0)
      fail("seeking failed", err);
    avformat_flush(d.fmt);
  }
  if (d.video_ctx)
    avcodec_flush_buffers(d.video_ctx);
  if (d.audio_ctx) {
    avcodec_flush_buffers(d.audio_ctx);
    d.make_resampler(); // drop samples buffered for resampling
  }
  d.demux_eof = false;
  d.video_drained = false;
  d.audio_drained = false;
}

} // namespace bdg::wish::media
