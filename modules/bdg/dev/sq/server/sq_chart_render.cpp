// MIT License © 2026 Binary Dice Games
/// @file sq_chart_render.cpp
/// @brief Implementation of the sq chart PNG rasterizer.
#include "sq_chart_render.hpp"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#define STB_EASY_FONT_STATIC
#include <stb_easy_font.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace bdg::wish::sq_chart {

namespace {

struct rgb {
  uint8_t r, g, b;
};

// Okabe-Ito colour-blind-safe palette.
constexpr rgb kPalette[] = {{0, 114, 178},  {230, 159, 0}, {0, 158, 115}, {213, 94, 0},
                            {204, 121, 167}, {86, 180, 233}, {240, 228, 66}, {100, 100, 100}};
constexpr rgb kBg{255, 255, 255};
constexpr rgb kInk{40, 40, 40};
constexpr rgb kGrid{225, 225, 225};
constexpr int kTextScale = 2;

rgb series_color(size_t i) {
  return kPalette[i % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

// Supersampled canvas: every drawing call takes *logical* (output) pixel
// coordinates and is rasterized at kSuper times the resolution; pixels()
// box-filters the result down, which anti-aliases lines, discs and text.
constexpr int kSuper = 4;

class canvas {
 public:
  /// @param font_ttf TrueType bytes (must outlive the canvas) or empty for the built-in font.
  canvas(int w, int h, const std::string& font_ttf, float font_size)
      : w_(w), h_(h), pw_(w * kSuper), ph_(h * kSuper), px_(static_cast<size_t>(pw_) * ph_ * 3), size_(font_size) {
    if (!font_ttf.empty() && font_size > 0) {
      const auto* data = reinterpret_cast<const unsigned char*>(font_ttf.data());
      const int offset = font_ttf.size() > 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
      have_font_ = offset >= 0 && stbtt_InitFont(&font_, data, offset) != 0;
      if (have_font_) {
        scale_ = stbtt_ScaleForPixelHeight(&font_, size_ * kSuper);
        stbtt_GetFontVMetrics(&font_, &ascent_, &descent_, &line_gap_);
      }
    }
    for (size_t i = 0; i < px_.size(); i += 3) {
      px_[i] = kBg.r;
      px_[i + 1] = kBg.g;
      px_[i + 2] = kBg.b;
    }
    clear_clip();
  }
  void set_clip(int x0, int y0, int x1, int y1) {
    cx0_ = x0 * kSuper; cy0_ = y0 * kSuper; cx1_ = x1 * kSuper; cy1_ = y1 * kSuper;
  }
  void clear_clip() { set_clip(0, 0, w_, h_); }

  /// Blend at a physical (supersampled) pixel.
  void blend_phys(int x, int y, rgb c, float a = 1.0f) {
    if (x < std::max(cx0_, 0) || y < std::max(cy0_, 0) || x >= std::min(cx1_, pw_) || y >= std::min(cy1_, ph_))
      return;
    uint8_t* p = &px_[(static_cast<size_t>(y) * pw_ + x) * 3];
    p[0] = static_cast<uint8_t>(p[0] + (c.r - p[0]) * a);
    p[1] = static_cast<uint8_t>(p[1] + (c.g - p[1]) * a);
    p[2] = static_cast<uint8_t>(p[2] + (c.b - p[2]) * a);
  }
  /// Fills the logical rectangle; edges may be fractional.
  void fill_rect(float x0, float y0, float x1, float y1, rgb c, float a = 1.0f) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    fill_phys(x0 * kSuper, y0 * kSuper, x1 * kSuper, y1 * kSuper, c, a);
  }
  /// Line of @p thick logical pixels (round-ish caps via square stamps).
  void line(float x0, float y0, float x1, float y1, rgb c, float thick = 2.0f) {
    x0 *= kSuper; y0 *= kSuper; x1 *= kSuper; y1 *= kSuper;
    const float t = thick * kSuper;
    const float dx = x1 - x0, dy = y1 - y0;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
    for (int i = 0; i <= steps; ++i) {
      const float f = static_cast<float>(i) / steps;
      const float x = x0 + dx * f, y = y0 + dy * f;
      fill_phys(x - t / 2, y - t / 2, x + t / 2, y + t / 2, c, 1.0f);
    }
  }
  int text_width(const std::string& s) const {
    if (!have_font_)
      return stb_easy_font_width(const_cast<char*>(s.c_str())) * kTextScale;
    float pen = 0;
    int prev = 0;
    for (int cp : codepoints(s)) {
      pen += advance(cp, prev);
      prev = cp;
    }
    return static_cast<int>(std::ceil(pen / kSuper));
  }
  int text_height() const {
    return have_font_ ? static_cast<int>(std::ceil((ascent_ - descent_) * scale_ / kSuper)) : 12 * kTextScale;
  }
  /// Draws @p s with its line box's top-left corner at logical (x, y).
  void text(int x, int y, const std::string& s, rgb c = kInk) {
    if (!have_font_) {
      easy_text(x, y, s, c);
      return;
    }
    float pen = static_cast<float>(x) * kSuper;
    const int baseline = static_cast<int>(std::lround(y * kSuper + ascent_ * scale_));
    int prev = 0;
    std::vector<unsigned char> bmp;
    for (int cp : codepoints(s)) {
      int x0, y0, x1, y1;
      stbtt_GetCodepointBitmapBox(&font_, cp, scale_, scale_, &x0, &y0, &x1, &y1);
      const int gw = x1 - x0, gh = y1 - y0;
      if (gw > 0 && gh > 0) {
        bmp.assign(static_cast<size_t>(gw) * gh, 0);
        stbtt_MakeCodepointBitmap(&font_, bmp.data(), gw, gh, gw, scale_, scale_, cp);
        const int ox = static_cast<int>(std::lround(pen)) + x0, oy = baseline + y0;
        for (int gy = 0; gy < gh; ++gy)
          for (int gx = 0; gx < gw; ++gx)
            if (const unsigned char cov = bmp[static_cast<size_t>(gy) * gw + gx])
              blend_phys(ox + gx, oy + gy, c, cov / 255.0f);
      }
      pen += advance(cp, prev);
      prev = cp;
    }
  }
  int width() const { return w_; }
  int height() const { return h_; }
  int phys_scale() const { return kSuper; }

  /// The image box-filtered down to w x h RGB.
  std::vector<uint8_t> pixels() const {
    std::vector<uint8_t> out(static_cast<size_t>(w_) * h_ * 3);
    for (int y = 0; y < h_; ++y)
      for (int x = 0; x < w_; ++x)
        for (int ch = 0; ch < 3; ++ch) {
          int sum = 0;
          for (int sy = 0; sy < kSuper; ++sy)
            for (int sx = 0; sx < kSuper; ++sx)
              sum += px_[(static_cast<size_t>(y * kSuper + sy) * pw_ + x * kSuper + sx) * 3 + ch];
          out[(static_cast<size_t>(y) * w_ + x) * 3 + ch] = static_cast<uint8_t>(sum / (kSuper * kSuper));
        }
    return out;
  }

 private:
  // Advance (physical px) of @p cp, including kerning against @p prev.
  float advance(int cp, int prev) const {
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&font_, cp, &adv, &lsb);
    float a = adv * scale_;
    if (prev)
      a += stbtt_GetCodepointKernAdvance(&font_, prev, cp) * scale_;
    return a;
  }
  static std::vector<int> codepoints(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
      const unsigned char b = static_cast<unsigned char>(s[i]);
      const int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
      int cp = len == 1 ? b : b & (0xFF >> (len + 1));
      for (int k = 1; k < len && i + k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
      out.push_back(cp);
      i += len;
    }
    return out;
  }
  void easy_text(int x, int y, const std::string& s, rgb c) {
    static char buf[64 * 1024];
    const int quads = stb_easy_font_print(0, 0, const_cast<char*>(s.c_str()), nullptr, buf, sizeof(buf));
    struct vert { float x, y, z; uint8_t col[4]; };
    const vert* v = reinterpret_cast<const vert*>(buf);
    for (int q = 0; q < quads; ++q, v += 4)
      fill_rect(x + v[0].x * kTextScale, y + v[0].y * kTextScale, x + v[2].x * kTextScale,
                y + v[2].y * kTextScale, c);
  }

  // Fills a physical-space rectangle, weighting partially covered edge pixels.
  void fill_phys(float x0, float y0, float x1, float y1, rgb c, float a) {
    const int ix0 = static_cast<int>(std::floor(x0)), ix1 = static_cast<int>(std::ceil(x1));
    const int iy0 = static_cast<int>(std::floor(y0)), iy1 = static_cast<int>(std::ceil(y1));
    for (int y = iy0; y < iy1; ++y) {
      const float cy = std::min<float>(y + 1, y1) - std::max<float>(y, y0);
      for (int x = ix0; x < ix1; ++x) {
        const float cx = std::min<float>(x + 1, x1) - std::max<float>(x, x0);
        blend_phys(x, y, c, a * cx * cy);
      }
    }
  }

  int w_, h_, pw_, ph_;
  int cx0_{0}, cy0_{0}, cx1_{0}, cy1_{0};
  std::vector<uint8_t> px_;
  float size_{0};
  bool have_font_{false};
  mutable stbtt_fontinfo font_{};
  float scale_{0};
  int ascent_{0}, descent_{0}, line_gap_{0};
};

std::string fmt(double v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.6g", v);
  return b;
}

/// Tick positions at a "nice" step covering [lo, hi] with about @p target ticks.
std::vector<double> nice_ticks(double lo, double hi, int target) {
  std::vector<double> t;
  if (!(hi > lo))
    return {lo};
  const double raw = (hi - lo) / std::max(1, target);
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double norm = raw / mag;
  const double step = (norm < 1.5 ? 1 : norm < 3.5 ? 2 : norm < 7.5 ? 5 : 10) * mag;
  for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-6; v += step)
    t.push_back(std::fabs(v) < step * 1e-9 ? 0.0 : v);
  return t;
}

struct range {
  double lo{0}, hi{1};
  void include(double v) {
    if (!seen) { lo = hi = v; seen = true; }
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  void pad(double frac) {
    if (!seen) { lo = 0; hi = 1; return; }
    if (hi == lo) { lo -= 0.5; hi += 0.5; return; }
    const double d = (hi - lo) * frac;
    lo -= d;
    hi += d;
  }
  bool seen{false};
};

struct hist_bins {
  double lo{0}, width{1};
  std::vector<int> counts;
};

hist_bins bin(const std::vector<float>& v, int rule) {
  hist_bins h;
  if (v.empty())
    return h;
  const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
  double mean = 0;
  for (float x : v) mean += x;
  mean /= v.size();
  double var = 0;
  for (float x : v) var += (x - mean) * (x - mean);
  const float sd = static_cast<float>(std::sqrt(var / v.size()));
  const int n = histogram_bin_count(rule, v.size(), *mx - *mn, sd);
  h.lo = *mn;
  h.width = *mx > *mn ? (static_cast<double>(*mx) - *mn) / n : 1.0;
  h.counts.assign(n, 0);
  for (float x : v)
    ++h.counts[std::min(n - 1, static_cast<int>((x - h.lo) / h.width))];
  return h;
}

// Legend swatches + labels at (x, y), top-down. Returns nothing; clips to @p max_y.
void draw_legend(canvas& c, int x, int y, int max_y, const std::vector<std::string>& labels) {
  const int row = c.text_height() + 6;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (y + row > max_y) {
      c.text(x, y, "...");
      return;
    }
    const int sw = c.text_height() - 6;
    c.fill_rect(x, y + 3, x + sw, y + 3 + sw, series_color(i));
    c.text(x + sw + 8, y, labels[i]);
    y += row;
  }
}

void draw_pie(canvas& c, const chart_spec& spec, int px0, int py0, int px1, int py1) {
  if (spec.data.empty())
    return;
  const auto& vals = spec.data[0].ys;
  double total = 0;
  for (float v : vals) total += std::max(0.0f, v);
  if (total <= 0)
    return;
  const double cx = (px0 + px1) / 2.0, cy = (py0 + py1) / 2.0;
  const double r = std::min(px1 - px0, py1 - py0) * 0.45;
  // Clockwise from 12 o'clock.
  std::vector<double> ends;
  double acc = 0;
  for (float v : vals) { acc += std::max(0.0f, v) / total; ends.push_back(acc); }
  constexpr double kTwoPi = 6.283185307179586;
  const int k = c.phys_scale();
  const double pcx = cx * k, pcy = cy * k, pr = r * k;
  for (int y = static_cast<int>(pcy - pr) - 1; y <= pcy + pr + 1; ++y)
    for (int x = static_cast<int>(pcx - pr) - 1; x <= pcx + pr + 1; ++x) {
      const double dx = x + 0.5 - pcx, dy = y + 0.5 - pcy, d = std::sqrt(dx * dx + dy * dy);
      if (d > pr)
        continue;
      double a = std::atan2(dx, -dy); // 0 at top, clockwise positive
      if (a < 0) a += kTwoPi;
      const double f = a / kTwoPi;
      size_t s = std::lower_bound(ends.begin(), ends.end(), f) - ends.begin();
      if (s >= ends.size()) s = ends.size() - 1;
      c.blend_phys(x, y, series_color(s));
    }
  std::vector<std::string> labels;
  for (size_t i = 0; i < vals.size(); ++i) {
    char pct[16];
    std::snprintf(pct, sizeof(pct), " (%.1f%%)", std::max(0.0f, vals[i]) / total * 100.0);
    labels.push_back((i < spec.pie_labels.size() ? spec.pie_labels[i] : std::to_string(i + 1)) + pct);
  }
  draw_legend(c, px1 + 20, py0, py1, labels);
}

} // namespace

int histogram_bin_count(int rule, size_t n, float range, float sd) {
  if (n == 0)
    return 1;
  if (rule > 0)
    return rule;
  const double dn = static_cast<double>(n);
  double bins = 1;
  switch (rule) {
    case -2: // Scott
      bins = (sd > 0 && range > 0) ? range / (3.49 * sd * std::pow(dn, -1.0 / 3.0)) : 1;
      break;
    case -3: bins = 2.0 * std::cbrt(dn); break;   // Rice
    case -4: bins = std::sqrt(dn); break;         // Square root
    default: bins = std::log2(dn) + 1; break;     // Sturges
  }
  return std::clamp(static_cast<int>(std::ceil(bins)), 1, 1000);
}

std::string render_png(const chart_spec& spec, int width, int height) {
  if (width <= 0 || height <= 0)
    return {};
  bool any = false;
  for (const auto& s : spec.data)
    any = any || !s.ys.empty();
  if (!any)
    return {};

  canvas c(width, height, spec.font_ttf, spec.font_size);
  const int th = c.text_height();
  const bool pie = spec.type == kind::pie;

  std::vector<std::string> labels;
  for (const auto& s : spec.data) labels.push_back(s.label);
  int legend_w = 0;
  const bool legend = !pie && spec.data.size() > 1;
  if (legend) {
    legend_w = 40;
    for (const auto& l : labels) legend_w = std::max(legend_w, c.text_width(l) + 40);
  }

  const int left = pie ? 20 : c.text_width("0000000000") + 24, top = th + 24, bottom = pie ? 20 : th * 2 + 30;
  const int right = pie ? width / 3 : 20 + legend_w;
  const int px0 = left, py0 = top, px1 = std::max(px0 + 10, width - right), py1 = std::max(py0 + 10, height - bottom);

  if (pie) {
    draw_pie(c, spec, px0, py0, std::min(px1, width - right), py1);
    const auto pixels = c.pixels();
    return [&] {
      std::string out;
      stbi_write_png_to_func(
          [](void* ctx, void* d, int n) { static_cast<std::string*>(ctx)->append(static_cast<char*>(d), n); },
          &out, width, height, 3, pixels.data(), width * 3);
      return out;
    }();
  }

  // ── Data ranges (x = horizontal axis, y = vertical axis) ──
  const bool horiz = spec.type == kind::bars_h;
  const bool hist = spec.type == kind::histogram;
  const bool bars = spec.type == kind::bars || horiz;
  std::vector<hist_bins> hbins;
  range rx, ry;
  if (hist) {
    for (const auto& s : spec.data) {
      hbins.push_back(bin(s.ys, spec.bins));
      const auto& h = hbins.back();
      if (h.counts.empty()) continue;
      rx.include(h.lo);
      rx.include(h.lo + h.width * h.counts.size());
      for (int n : h.counts) ry.include(n);
    }
    ry.include(0);
  } else {
    for (const auto& s : spec.data)
      for (size_t i = 0; i < s.ys.size() && i < s.xs.size(); ++i) {
        // For bars_h, xs are lengths (horizontal) and ys positions (vertical).
        rx.include(s.xs[i]);
        ry.include(s.ys[i]);
      }
    if (bars) {
      const double half = spec.bar_size * spec.data.size() / 2.0;
      if (horiz) {
        rx.include(0);
        ry.lo -= half; ry.hi += half;
      } else {
        ry.include(0);
        rx.lo -= half; rx.hi += half;
      }
    }
    if (spec.type == kind::area || spec.type == kind::stems) ry.include(0);
  }
  rx.pad(bars && !horiz ? 0.0 : 0.05);
  ry.pad(bars && horiz ? 0.0 : 0.05);
  if (hist) ry.hi += (ry.hi - ry.lo) * 0.05;

  auto sx = [&](double v) { return px0 + (v - rx.lo) / (rx.hi - rx.lo) * (px1 - px0); };
  auto sy = [&](double v) { return py1 - (v - ry.lo) / (ry.hi - ry.lo) * (py1 - py0); };

  // ── Grid, ticks, frame ──
  for (double t : nice_ticks(rx.lo, rx.hi, std::max(2, (px1 - px0) / 110))) {
    const float x = static_cast<float>(sx(t));
    c.fill_rect(x, py0, x + 1, py1, kGrid);
    const std::string s = fmt(t);
    c.text(static_cast<int>(x) - c.text_width(s) / 2, py1 + 8, s);
  }
  for (double t : nice_ticks(ry.lo, ry.hi, std::max(2, (py1 - py0) / 60))) {
    const float y = static_cast<float>(sy(t));
    c.fill_rect(px0, y, px1, y + 1, kGrid);
    const std::string s = fmt(t);
    c.text(px0 - 8 - c.text_width(s), static_cast<int>(y) - th / 2, s);
  }
  c.fill_rect(px0, py1, px1, py1 + 1, kInk);
  c.fill_rect(px0, py0, px0 + 1, py1, kInk);
  const std::string xl = hist ? std::string{} : spec.x_label;
  if (!xl.empty())
    c.text((px0 + px1) / 2 - c.text_width(xl) / 2, py1 + th + 16, xl);
  const std::string yl = hist ? std::string{"count"} : spec.y_label;
  if (!yl.empty())
    c.text(px0, 10, yl);

  // ── Series ──
  c.set_clip(px0, py0, px1 + 1, py1 + 1);
  const double base = sy(0);
  for (size_t si = 0; si < spec.data.size(); ++si) {
    const auto& s = spec.data[si];
    const rgb col = series_color(si);
    const size_t n = std::min(s.xs.size(), s.ys.size());
    if (hist) {
      const auto& h = hbins[si];
      for (size_t b = 0; b < h.counts.size(); ++b)
        c.fill_rect(static_cast<float>(sx(h.lo + h.width * b)) + 1, static_cast<float>(sy(h.counts[b])),
                    static_cast<float>(sx(h.lo + h.width * (b + 1))), static_cast<float>(sy(0)), col, 0.75f);
      continue;
    }
    switch (spec.type) {
      case kind::line:
        for (size_t i = 1; i < n; ++i)
          c.line(sx(s.xs[i - 1]), sy(s.ys[i - 1]), sx(s.xs[i]), sy(s.ys[i]), col);
        if (n == 1) c.fill_rect(sx(s.xs[0]) - 2, sy(s.ys[0]) - 2, sx(s.xs[0]) + 2, sy(s.ys[0]) + 2, col);
        break;
      case kind::stairs:
        for (size_t i = 1; i < n; ++i) {
          c.line(sx(s.xs[i - 1]), sy(s.ys[i - 1]), sx(s.xs[i]), sy(s.ys[i - 1]), col);
          c.line(sx(s.xs[i]), sy(s.ys[i - 1]), sx(s.xs[i]), sy(s.ys[i]), col);
        }
        break;
      case kind::scatter:
        for (size_t i = 0; i < n; ++i)
          c.fill_rect(sx(s.xs[i]) - 3, sy(s.ys[i]) - 3, sx(s.xs[i]) + 3, sy(s.ys[i]) + 3, col);
        break;
      case kind::stems:
        for (size_t i = 0; i < n; ++i) {
          c.line(sx(s.xs[i]), base, sx(s.xs[i]), sy(s.ys[i]), col, 1);
          c.fill_rect(sx(s.xs[i]) - 3, sy(s.ys[i]) - 3, sx(s.xs[i]) + 3, sy(s.ys[i]) + 3, col);
        }
        break;
      case kind::area:
        for (size_t i = 1; i < n; ++i) {
          const double xa = sx(s.xs[i - 1]), xb = sx(s.xs[i]);
          for (int x = static_cast<int>(std::min(xa, xb)); x <= static_cast<int>(std::max(xa, xb)); ++x) {
            const double t = xb == xa ? 0 : (x - xa) / (xb - xa);
            const double y = sy(s.ys[i - 1]) + (sy(s.ys[i]) - sy(s.ys[i - 1])) * t;
            c.fill_rect(x, static_cast<float>(y), x + 1, static_cast<float>(base), col, 0.3f);
          }
          c.line(xa, sy(s.ys[i - 1]), xb, sy(s.ys[i]), col);
        }
        break;
      case kind::bars:
        for (size_t i = 0; i < n; ++i)
          c.fill_rect(sx(s.xs[i] - spec.bar_size / 2), sy(s.ys[i]), sx(s.xs[i] + spec.bar_size / 2), base, col);
        break;
      case kind::bars_h: // xs = lengths, ys = positions
        for (size_t i = 0; i < n; ++i)
          c.fill_rect(sx(0), sy(s.ys[i] + spec.bar_size / 2), sx(s.xs[i]), sy(s.ys[i] - spec.bar_size / 2), col);
        break;
      default:
        break;
    }
  }
  c.clear_clip();

  if (legend)
    draw_legend(c, px1 + 20, py0, py1, labels);

  const auto pixels = c.pixels();
  std::string out;
  stbi_write_png_to_func(
      [](void* ctx, void* d, int n) { static_cast<std::string*>(ctx)->append(static_cast<char*>(d), n); }, &out,
      width, height, 3, pixels.data(), width * 3);
  return out;
}

} // namespace bdg::wish::sq_chart
