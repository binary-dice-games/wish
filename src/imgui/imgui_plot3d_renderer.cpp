// MIT License © 2025 Binary Dice Games
/// @file imgui_plot3d_renderer.cpp
/// @brief ImPlot3D render functions for wish plot3d elements.
///
/// Each function maps one wish element class to the corresponding ImPlot3D call.
/// All functions share the signature:
///   void(imgui_renderer&, const ui_element&, session&)
/// matching the render_fn typedef in imgui_renderer.cpp.
#include "imgui_plot3d_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <wish/renderer.hpp>

#include <implot3d.h>

#include <algorithm>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── Inline field helpers (mirrors imgui_renderer.cpp) ─────────────────────────

static std::string str_field(
    const dynamic& obj, key_t k, const char* dflt = "") {
  const auto* f = obj.findField(k);
  return (f && f->is<std::string>()) ? f->as<std::string>() : dflt;
}

static float float_field(const dynamic& obj, key_t k, float dflt = 0.0f) {
  const auto* f = obj.findField(k);
  return (f && f->is<float>()) ? f->as<float>() : dflt;
}

static int32_t int_field(const dynamic& obj, key_t k, int32_t dflt = 0) {
  const auto* f = obj.findField(k);
  return (f && f->is<int32_t>()) ? f->as<int32_t>() : dflt;
}

static const std::vector<float>* vec_field(const dynamic& obj, key_t k) {
  const auto* f = obj.findField(k);
  return (f && f->is<std::vector<float>>()) ? &f->as<std::vector<float>>() : nullptr;
}


// ── Plot3D container ──────────────────────────────────────────────────────────

void render_plot3d(imgui_renderer& r, const ui_element& node, session& s) {
  auto    title   = str_field(node, "title"_key, "##plot3d");
  float   w       = float_field(node, "width"_key, -1.0f);
  float   h       = float_field(node, "height"_key, 400.0f);
  int32_t flags   = int_field(node, "flags"_key, 0);
  auto    x_label = str_field(node, "x_label"_key, "");
  auto    y_label = str_field(node, "y_label"_key, "");
  auto    z_label = str_field(node, "z_label"_key, "");
  int32_t xf      = int_field(node, "x_flags"_key, 0);
  int32_t yf      = int_field(node, "y_flags"_key, 0);
  int32_t zf      = int_field(node, "z_flags"_key, 0);

  if (ImPlot3D::BeginPlot(title.c_str(), ImVec2(w, h),
                          ImPlot3DFlags(flags))) {
    if (!x_label.empty() || !y_label.empty() || !z_label.empty()
        || xf != 0 || yf != 0 || zf != 0) {
      ImPlot3D::SetupAxes(
          x_label.empty() ? nullptr : x_label.c_str(),
          y_label.empty() ? nullptr : y_label.c_str(),
          z_label.empty() ? nullptr : z_label.c_str(),
          ImPlot3DAxisFlags(xf),
          ImPlot3DAxisFlags(yf),
          ImPlot3DAxisFlags(zf));
    }
    render_children(r, node, s);
    ImPlot3D::EndPlot();
  }
}

// ── Line / Scatter ────────────────────────────────────────────────────────────

void render_plot3d_line(imgui_renderer&, const ui_element& node, session&) {
  auto label      = str_field(node, "label"_key, "");
  const auto* xs  = vec_field(node, "xs"_key);
  const auto* ys  = vec_field(node, "ys"_key);
  const auto* zs  = vec_field(node, "zs"_key);
  if (!xs || !ys || !zs) return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  if (count > 0)
    ImPlot3D::PlotLine(label.c_str(), xs->data(), ys->data(), zs->data(), count);
}

void render_plot3d_scatter(imgui_renderer&, const ui_element& node, session&) {
  auto label      = str_field(node, "label"_key, "");
  const auto* xs  = vec_field(node, "xs"_key);
  const auto* ys  = vec_field(node, "ys"_key);
  const auto* zs  = vec_field(node, "zs"_key);
  if (!xs || !ys || !zs) return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  if (count > 0)
    ImPlot3D::PlotScatter(label.c_str(), xs->data(), ys->data(), zs->data(), count);
}

// ── Surface ───────────────────────────────────────────────────────────────────

void render_plot3d_surface(imgui_renderer&, const ui_element& node, session&) {
  auto    label     = str_field(node, "label"_key, "");
  int32_t x_count   = int_field(node, "x_count"_key, 2);
  int32_t y_count   = int_field(node, "y_count"_key, 2);
  float   scale_min = float_field(node, "scale_min"_key, 0.0f);
  float   scale_max = float_field(node, "scale_max"_key, 0.0f);
  const auto* xs    = vec_field(node, "xs"_key);
  const auto* ys    = vec_field(node, "ys"_key);
  const auto* zs    = vec_field(node, "zs"_key);
  if (!xs || !ys || !zs) return;
  int needed = x_count * y_count;
  if (needed <= 0) return;
  if (int(xs->size()) < needed || int(ys->size()) < needed ||
      int(zs->size()) < needed) return;

  ImPlot3D::PlotSurface(label.c_str(),
                        xs->data(), ys->data(), zs->data(),
                        x_count, y_count,
                        double(scale_min), double(scale_max));
}

// ── Triangle / Quad / Mesh ────────────────────────────────────────────────────

void render_plot3d_triangle(imgui_renderer&, const ui_element& node, session&) {
  auto label      = str_field(node, "label"_key, "");
  const auto* xs  = vec_field(node, "xs"_key);
  const auto* ys  = vec_field(node, "ys"_key);
  const auto* zs  = vec_field(node, "zs"_key);
  if (!xs || !ys || !zs) return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  // Must be a multiple of 3.
  count -= count % 3;
  if (count > 0)
    ImPlot3D::PlotTriangle(label.c_str(),
                           xs->data(), ys->data(), zs->data(), count);
}

void render_plot3d_quad(imgui_renderer&, const ui_element& node, session&) {
  auto label      = str_field(node, "label"_key, "");
  const auto* xs  = vec_field(node, "xs"_key);
  const auto* ys  = vec_field(node, "ys"_key);
  const auto* zs  = vec_field(node, "zs"_key);
  if (!xs || !ys || !zs) return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  // Must be a multiple of 4.
  count -= count % 4;
  if (count > 0)
    ImPlot3D::PlotQuad(label.c_str(),
                       xs->data(), ys->data(), zs->data(), count);
}

void render_plot3d_mesh(imgui_renderer&, const ui_element& node, session&) {
  auto label     = str_field(node, "label"_key, "");
  const auto* xs = vec_field(node, "xs"_key);
  const auto* ys = vec_field(node, "ys"_key);
  const auto* zs = vec_field(node, "zs"_key);
  // indices are stored as vector<int32_t> — access via findField directly.
  const auto* fi = node.findField("indices"_key);
  if (!xs || !ys || !zs || !fi || !fi->is<std::vector<int32_t>>()) return;
  const auto& idx_vec = fi->as<std::vector<int32_t>>();
  int vtx_count = int(std::min({xs->size(), ys->size(), zs->size()}));
  int idx_count = int(idx_vec.size());
  idx_count -= idx_count % 3;
  if (vtx_count <= 0 || idx_count <= 0) return;

  // Convert int32_t indices to unsigned int.
  std::vector<unsigned int> uids;
  uids.reserve(static_cast<size_t>(idx_count));
  for (int i = 0; i < idx_count; ++i)
    uids.push_back(static_cast<unsigned int>(idx_vec[static_cast<size_t>(i)]));

  // Store as typed float* locals so template T=float is deduced unambiguously.
  const float* pxs = xs->data();
  const float* pys = ys->data();
  const float* pzs = zs->data();
  ImPlot3D::PlotMesh(label.c_str(), pxs, pys, pzs, uids.data(), vtx_count, idx_count);
}

// ── Text annotation ───────────────────────────────────────────────────────────

void render_plot3d_text(imgui_renderer&, const ui_element& node, session&) {
  auto  text     = str_field(node, "text"_key, "");
  float x        = float_field(node, "x"_key, 0.0f);
  float y        = float_field(node, "y"_key, 0.0f);
  float z        = float_field(node, "z"_key, 0.0f);
  float angle    = float_field(node, "angle"_key, 0.0f);
  float offset_x = float_field(node, "offset_x"_key, 0.0f);
  float offset_y = float_field(node, "offset_y"_key, 0.0f);
  if (!text.empty())
    ImPlot3D::PlotText(text.c_str(),
                       double(x), double(y), double(z),
                       double(angle),
                       ImVec2(offset_x, offset_y));
}

}  // namespace bdg::wish

#endif  // WISH_IMGUI_ENABLED
