// MIT License © 2025 Binary Dice Games
/// @file plot3d_mesh.cpp
/// @brief Registers triangle, quad, and indexed mesh 3-D plot prototypes.
///
/// Covered classes: Plot3DTriangle, Plot3DQuad, Plot3DMesh.
#include "src/bison/bison_object.hpp"

#include "plot3d_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_plot3d_mesh() {
  // Plot3DTriangle — a set of filled triangles.
  // xs/ys/zs must have count divisible by 3; each group of 3 is one triangle.
  {
    auto proto = dynamic_ptr{"Plot3DTriangle"_rkey, {}};
    proto->addField(
        "xs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("X Vertices"),
            attr<Description>("Vertex X coordinates; length must be a multiple of 3."),
            attr<Category>("Data")});
    proto->addField(
        "ys"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Y Vertices"),
            attr<Description>("Vertex Y coordinates; length must be a multiple of 3."),
            attr<Category>("Data")});
    proto->addField(
        "zs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Z Vertices"),
            attr<Description>("Vertex Z coordinates; length must be a multiple of 3."),
            attr<Category>("Data")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DTriangle"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws filled 3-D triangles. xs/ys/zs are vertex arrays whose length "
                          "must be a multiple of 3 (every 3 consecutive vertices form one triangle). "
                          "Must be a child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Plot3DItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Plot3DTriangle"_key));
  }

  // Plot3DQuad — a set of filled quads.
  // xs/ys/zs must have count divisible by 4; each group of 4 is one quad.
  {
    auto proto = dynamic_ptr{"Plot3DQuad"_rkey, {}};
    proto->addField(
        "xs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("X Vertices"),
            attr<Description>("Vertex X coordinates; length must be a multiple of 4."),
            attr<Category>("Data")});
    proto->addField(
        "ys"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Y Vertices"),
            attr<Description>("Vertex Y coordinates; length must be a multiple of 4."),
            attr<Category>("Data")});
    proto->addField(
        "zs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Z Vertices"),
            attr<Description>("Vertex Z coordinates; length must be a multiple of 4."),
            attr<Category>("Data")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DQuad"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws filled 3-D quads. xs/ys/zs are vertex arrays whose length "
                          "must be a multiple of 4 (every 4 consecutive vertices form one quad). "
                          "Must be a child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Plot3DItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Plot3DQuad"_key));
  }

  // Plot3DMesh — indexed triangle mesh with separate vertex and index arrays.
  // indices is a flat list of unsigned integer indices into the vertex arrays;
  // stored as vector<int32_t> and converted to unsigned int at render time.
  {
    auto proto = dynamic_ptr{"Plot3DMesh"_rkey, {}};
    proto->addField(
        "xs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Vertex X"),
            attr<Description>("X coordinate of each mesh vertex."),
            attr<Category>("Data")});
    proto->addField(
        "ys"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Vertex Y"),
            attr<Description>("Y coordinate of each mesh vertex."),
            attr<Category>("Data")});
    proto->addField(
        "zs"_rkey,
        field{
            std::vector<float>{},
            attr<DisplayName>("Vertex Z"),
            attr<Description>("Z coordinate of each mesh vertex."),
            attr<Category>("Data")});
    proto->addField(
        "indices"_rkey,
        field{
            std::vector<int32_t>{},
            attr<DisplayName>("Indices"),
            attr<Description>("Flat list of vertex indices forming triangles; length must be a multiple of 3."),
            attr<Category>("Data")});
    (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Plot3DMesh"));
    (*proto)[dynamic::CLASS].addAttribute(
        attr<Description>("Draws an indexed 3-D triangle mesh. xs/ys/zs are vertex arrays; "
                          "indices is a flat list of vertex indices (groups of 3 per triangle). "
                          "Must be a child of a Plot3D element."));
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        "Plot3DItem"_key,
        dynamic::make_factory<ui_element>("wish"_key, "Plot3DMesh"_key));
  }
}

} // namespace bdg::wish
