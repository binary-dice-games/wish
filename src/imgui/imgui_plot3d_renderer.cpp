// MIT License © 2025 Binary Dice Games
/// @file imgui_plot3d_renderer.cpp
/// @brief ImPlot3D render functions for wish plot3d elements.
///
/// Each function maps one wish element class to the corresponding ImPlot3D call.
/// All functions share the signature:
///   void(imgui_renderer&, const ui_element&, const context&)
/// matching the render_fn typedef in imgui_renderer.cpp.
#include "imgui_plot3d_renderer.hpp"

#ifdef WISH_IMGUI_ENABLED

#include <imgui/imgui_plot_style.hpp>
#include <server/renderer.hpp>

#include <implot3d.h>

#include <algorithm>
#include <string>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// No with_id()/label-suffix helper needed here: every render_plot3d_* function
// is invoked through imgui_renderer::render_node() (imgui_renderer.cpp),
// which already wraps the call in ImGui::PushID(stable_id(node)) -- unlike
// a top-level ImGui::Begin() window, ImPlot3D::BeginPlot() and the PlotXxx
// series calls all consult the current ID stack, so that push is enough to
// disambiguate same-labeled plots/series without touching their labels.

// ── Plot3D container ──────────────────────────────────────────────────────────

/// The ImPlot3DSpec for series element @p node: ImPlot3D's defaults plus
/// whatever styling fields the element sets (see imgui_plot_style.hpp).
static ImPlot3DSpec item_spec(const ui_element& node) {
  ImPlot3DSpec spec;
  apply_plot_item_style(spec, node);
  return spec;
}

void render_plot3d(imgui_renderer& r, const ui_element& node_base, const context& s) {
  const auto& node = static_cast<const ui_plot3d&>(node_base);
  const std::string& title = node.title_ref();
  float w = node.width();
  float h = node.height();
  int32_t flags = node.flags();
  const std::string& x_label = node.x_label_ref();
  const std::string& y_label = node.y_label_ref();
  const std::string& z_label = node.z_label_ref();
  int32_t xf = node.x_flags();
  int32_t yf = node.y_flags();
  int32_t zf = node.z_flags();

  // Pushed around the whole plot: it decides the colours series take in
  // turn when they set none, and the scale of surfaces drawn inside.
  const int32_t colormap = plot_colormap_of(node);
  if (colormap != kPlotColormapDefault)
    ImPlot3D::PushColormap(ImPlot3DColormap(colormap));

  if (ImPlot3D::BeginPlot(title.c_str(), ImVec2(w, h), ImPlot3DFlags(flags))) {
    if (!x_label.empty() || !y_label.empty() || !z_label.empty() || xf != 0 || yf != 0 || zf != 0) {
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
  if (colormap != kPlotColormapDefault)
    ImPlot3D::PopColormap();
}

// ── Line / Scatter ────────────────────────────────────────────────────────────

void render_plot3d_line(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_xyz_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* zs = node.zs();
  if (!xs || !ys || !zs)
    return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  if (count > 0)
    ImPlot3D::PlotLine(label.c_str(), xs->data(), ys->data(), zs->data(), count, item_spec(node));
}

void render_plot3d_scatter(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_xyz_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* zs = node.zs();
  if (!xs || !ys || !zs)
    return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  if (count > 0)
    ImPlot3D::PlotScatter(label.c_str(), xs->data(), ys->data(), zs->data(), count, item_spec(node));
}

// ── Surface ───────────────────────────────────────────────────────────────────

void render_plot3d_surface(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_surface&>(node_base);
  const std::string& label = node.label_ref();
  int32_t x_count = node.x_count();
  int32_t y_count = node.y_count();
  float scale_min = node.scale_min();
  float scale_max = node.scale_max();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* zs = node.zs();
  if (!xs || !ys || !zs)
    return;
  int needed = x_count * y_count;
  if (needed <= 0)
    return;
  if (int(xs->size()) < needed || int(ys->size()) < needed || int(zs->size()) < needed)
    return;

  ImPlot3D::PlotSurface(
      label.c_str(),
      xs->data(),
      ys->data(),
      zs->data(),
      x_count,
      y_count,
      double(scale_min),
      double(scale_max),
      item_spec(node));
}

// ── Triangle / Quad / Mesh ────────────────────────────────────────────────────

void render_plot3d_triangle(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_xyz_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* zs = node.zs();
  if (!xs || !ys || !zs)
    return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  // Must be a multiple of 3.
  count -= count % 3;
  if (count > 0)
    ImPlot3D::PlotTriangle(label.c_str(), xs->data(), ys->data(), zs->data(), count, item_spec(node));
}

void render_plot3d_quad(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_xyz_series&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* zs = node.zs();
  if (!xs || !ys || !zs)
    return;
  int count = int(std::min({xs->size(), ys->size(), zs->size()}));
  // Must be a multiple of 4.
  count -= count % 4;
  if (count > 0)
    ImPlot3D::PlotQuad(label.c_str(), xs->data(), ys->data(), zs->data(), count, item_spec(node));
}

void render_plot3d_mesh(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_mesh&>(node_base);
  const std::string& label = node.label_ref();
  const auto* xs = node.xs();
  const auto* ys = node.ys();
  const auto* zs = node.zs();
  const auto* idx_vec_ptr = node.indices();
  if (!xs || !ys || !zs || !idx_vec_ptr)
    return;
  const auto& idx_vec = *idx_vec_ptr;
  int vtx_count = int(std::min({xs->size(), ys->size(), zs->size()}));
  int idx_count = int(idx_vec.size());
  idx_count -= idx_count % 3;
  if (vtx_count <= 0 || idx_count <= 0)
    return;

  // Convert int32_t indices to unsigned int.
  std::vector<unsigned int> uids;
  uids.reserve(static_cast<size_t>(idx_count));
  for (int i = 0; i < idx_count; ++i)
    uids.push_back(static_cast<unsigned int>(idx_vec[static_cast<size_t>(i)]));

  // Store as typed float* locals so template T=float is deduced unambiguously.
  const float* pxs = xs->data();
  const float* pys = ys->data();
  const float* pzs = zs->data();
  ImPlot3D::PlotMesh(label.c_str(), pxs, pys, pzs, uids.data(), vtx_count, idx_count, item_spec(node));
}

// ── Text annotation ───────────────────────────────────────────────────────────

void render_plot3d_text(imgui_renderer&, const ui_element& node_base, const context&) {
  const auto& node = static_cast<const ui_plot3d_text&>(node_base);
  const std::string& text = node.text_ref();
  float x = node.x();
  float y = node.y();
  float z = node.z();
  float angle = node.angle();
  float offset_x = node.offset_x();
  float offset_y = node.offset_y();
  if (!text.empty())
    ImPlot3D::PlotText(text.c_str(), double(x), double(y), double(z), double(angle), ImVec2(offset_x, offset_y));
}

} // namespace bdg::wish

#endif // WISH_IMGUI_ENABLED
