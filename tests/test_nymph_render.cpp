// MIT License © 2026 Binary Dice Games
/// @file test_nymph_render.cpp
/// @brief nymph's offscreen rasterizer: a wish plot tree to RGBA pixels with
///        no window.
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include "src/rmi/shared/ids.hpp"

#include "modules/bdg/dev/nymph/server/nymph_figure_renderer.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

using bdg::wish::context;
using bdg::wish::import_yaml;
namespace nymph = bdg::wish::nymph;

namespace {

const char* kLinePlot = R"(
type: Plot
title: Spike
height: 284
children:
  - type: PlotLine
    label: value
    xs: [0, 1, 2, 3, 4]
    ys: [12, 6, 4, 8, 10]
)";

const char* kPlot3D = R"(
type: Plot3D
title: Spike 3D
height: 284
children:
  - type: Plot3DLine
    label: path
    xs: [0, 1, 2, 3]
    ys: [0, 1, 0, 1]
    zs: [0, 1, 2, 3]
)";

std::array<uint8_t, 4> pixel(const nymph::image& img, int x, int y) {
  const uint8_t* p = img.rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4;
  return {p[0], p[1], p[2], p[3]};
}

size_t distinct_colors(const nymph::image& img) {
  std::set<uint32_t> colors;
  for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
    colors.insert(uint32_t(img.rgba[i]) << 16 | uint32_t(img.rgba[i + 1]) << 8 | img.rgba[i + 2]);
  return colors.size();
}

class NymphRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
    sess_ = std::make_unique<context>(bdg::bison::rmi::shared::generate_id());
  }

  nymph::image render(const char* yaml, nymph::image_options options = small()) {
    auto tree = import_yaml(yaml);
    return nymph::render_figure(*tree.at(""), *sess_, options);
  }

  static nymph::image_options small() {
    nymph::image_options o;
    o.width = 400;
    o.height = 300;
    return o;
  }

  std::unique_ptr<context> sess_;
};

} // namespace

TEST_F(NymphRenderTest, PlotRendersAtRequestedSize) {
  auto img = render(kLinePlot);
  ASSERT_EQ(img.width, 400);
  ASSERT_EQ(img.height, 300);
  ASSERT_EQ(img.rgba.size(), 400u * 300u * 4u);
  // Axes, grid, the series line and text: far more than a flat fill.
  EXPECT_GT(distinct_colors(img), 4u);
}

// The series itself is drawn, not just the empty axes.
TEST_F(NymphRenderTest, DataChangesThePixels) {
  auto with_data = render(kLinePlot);
  auto without = render("type: Plot\ntitle: Spike\nheight: 284\n");
  EXPECT_NE(with_data.rgba, without.rgba);
  EXPECT_GT(distinct_colors(with_data), distinct_colors(without));
}

TEST_F(NymphRenderTest, CornerIsLightThemeBackground) {
  auto img = render(kLinePlot);
  ImGuiStyle light;
  ImGui::StyleColorsLight(&light);
  const ImVec4 bg = light.Colors[ImGuiCol_WindowBg];
  auto corner = pixel(img, 0, 0);
  EXPECT_NEAR(corner[0], bg.x * 255.0f, 1.0f);
  EXPECT_NEAR(corner[1], bg.y * 255.0f, 1.0f);
  EXPECT_NEAR(corner[2], bg.z * 255.0f, 1.0f);
  EXPECT_EQ(corner[3], 255);
}

TEST_F(NymphRenderTest, ScaleMultipliesPixelSize) {
  auto options = small();
  options.scale = 2;
  auto img = render(kLinePlot, options);
  EXPECT_EQ(img.width, 800);
  EXPECT_EQ(img.height, 600);
  EXPECT_GT(distinct_colors(img), 4u);
}

// The whole image is scaled, not drawn at 1x into one corner of a larger
// canvas: every 2x2 block of the scaled render averages to about the pixel
// the unscaled render has there.
TEST_F(NymphRenderTest, ScaledImageIsTheSamePictureEnlarged) {
  auto base = render(kLinePlot);
  auto options = small();
  options.scale = 2;
  auto big = render(kLinePlot, options);
  ASSERT_EQ(big.width, base.width * 2);
  ASSERT_EQ(big.height, base.height * 2);

  size_t close = 0;
  const size_t total = static_cast<size_t>(base.width) * base.height;
  for (int y = 0; y < base.height; ++y) {
    for (int x = 0; x < base.width; ++x) {
      int diff = 0;
      for (int c = 0; c < 3; ++c) {
        int sum = 0;
        for (int dy = 0; dy < 2; ++dy)
          for (int dx = 0; dx < 2; ++dx)
            sum += pixel(big, 2 * x + dx, 2 * y + dy)[c];
        diff = std::max(diff, std::abs(sum / 4 - pixel(base, x, y)[c]));
      }
      if (diff <= 48)
        ++close;
    }
  }
  // Text and thin lines are rasterized afresh at the higher density, so
  // their pixels differ; the plot area, bars, frame and background do not.
  EXPECT_GT(close, total * 9 / 10) << close << " of " << total << " pixels match";

  // And the content reaches the far corner region of the scaled image.
  std::set<uint32_t> corner_colors;
  for (int y = big.height / 2; y < big.height; ++y)
    for (int x = big.width / 2; x < big.width; ++x) {
      auto p = pixel(big, x, y);
      corner_colors.insert(uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]);
    }
  EXPECT_GT(corner_colors.size(), 4u);
}

TEST_F(NymphRenderTest, Plot3DRenders) {
  auto img = render(kPlot3D);
  EXPECT_GT(distinct_colors(img), 4u);
}

TEST_F(NymphRenderTest, SameTreeGivesSameBytes) {
  auto first = render(kLinePlot);
  auto second = render(kLinePlot);
  EXPECT_EQ(first.rgba, second.rgba);
}

TEST_F(NymphRenderTest, DarkThemeDiffersFromLight) {
  auto options = small();
  options.theme = "dark";
  auto dark = render(kLinePlot, options);
  auto light = render(kLinePlot);
  EXPECT_NE(pixel(dark, 0, 0), pixel(light, 0, 0));
}

// The form renders from on_event(), which the render loop calls while the
// live renderer's own ImGui frame is still open.
TEST_F(NymphRenderTest, SafeInsideAnotherContextsOpenFrame) {
  ImGuiContext* outer = ImGui::CreateContext();
  ImGui::SetCurrentContext(outer);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char* pixels;
  int fw, fh;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
  io.Fonts->SetTexID(ImTextureID{1});
  ImGui::NewFrame();
  ImGui::Begin("outer");

  auto img = render(kLinePlot);
  EXPECT_GT(distinct_colors(img), 4u);
  EXPECT_EQ(ImGui::GetCurrentContext(), outer);

  ImGui::TextUnformatted("still usable");
  ImGui::End();
  ImGui::EndFrame();
  ImGui::DestroyContext(outer);
}

TEST_F(NymphRenderTest, RestoresContextsWhenNoneWasCurrent) {
  ASSERT_EQ(ImGui::GetCurrentContext(), nullptr);
  render(kLinePlot);
  EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
}

TEST_F(NymphRenderTest, RejectsBadOptions) {
  auto options = small();
  options.width = 8;
  EXPECT_THROW(render(kLinePlot, options), std::runtime_error);
  options = small();
  options.scale = 9;
  EXPECT_THROW(render(kLinePlot, options), std::runtime_error);
  options = small();
  options.theme = "nope";
  EXPECT_THROW(render(kLinePlot, options), std::runtime_error);
  EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
}
