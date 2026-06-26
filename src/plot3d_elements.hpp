// MIT License © 2025 Binary Dice Games
/// @file plot3d_elements.hpp
/// @brief Internal declarations for per-plot3d-element registration functions.
///        Not part of the public wish API.
#pragma once

#include <wish/ui_element.hpp>

namespace bdg::wish {

void register_plot3d();           // Plot3D (container), Plot3DItem (series base)
void register_plot3d_series();    // Plot3DLine, Plot3DScatter
void register_plot3d_surface();   // Plot3DSurface
void register_plot3d_mesh();      // Plot3DTriangle, Plot3DQuad, Plot3DMesh
void register_plot3d_annotations(); // Plot3DText

}  // namespace bdg::wish
