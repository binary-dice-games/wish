// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <sdl/sdl3_renderer.hpp>

#include <SDL3/SDL.h>

namespace bdg::wish {

// Exposes the protected sdl_renderer() accessor so this test can issue
// direct SDL draw/readback calls to verify begin_render_target()/
// end_render_target() actually swap the active render target, not just that
// they run without throwing.
class testable_sdl3_renderer : public sdl3_renderer {
 public:
  using sdl3_renderer::display_scale;
  using sdl3_renderer::sdl_renderer;
};

} // namespace bdg::wish

using bdg::wish::testable_sdl3_renderer;

namespace {

// Reads the top-left pixel of whatever render target is currently active.
SDL_Color read_top_left_pixel(SDL_Renderer* r) {
  SDL_Surface* surf = SDL_RenderReadPixels(r, nullptr);
  SDL_Color c{};
  if (surf) {
    const auto* fmt_details = SDL_GetPixelFormatDetails(surf->format);
    Uint32 pixel = *static_cast<const Uint32*>(surf->pixels);
    SDL_GetRGBA(pixel, fmt_details, nullptr, &c.r, &c.g, &c.b, &c.a);
    SDL_DestroySurface(surf);
  }
  return c;
}

// Headless: no real display is required. SDL's "dummy" video driver plus its
// software render backend still support SDL_TEXTUREACCESS_TARGET textures,
// so begin_render_target()/end_render_target() can be exercised in CI.
class Sdl3RendererTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    renderer_ = std::make_unique<testable_sdl3_renderer>();
    renderer_->setup();
  }

  void TearDown() override {
    renderer_->teardown();
    renderer_.reset();
  }

  std::unique_ptr<testable_sdl3_renderer> renderer_;
};

} // namespace

// display_scale() compensates for SDL3 not separating window "points" from
// physical pixels on Windows (see its doc comment in sdl3_renderer.hpp):
// setup() must never leave it at zero or negative -- SDL_GetWindowDisplayScale()
// returning 0.0f (documented failure case, and the expected result under the
// headless "dummy" driver used here) must fall back to 1.0f, not propagate
// into style.FontSizeBase/ScaleAllSizes as a zero/negative multiplier that
// would collapse the whole UI to nothing.
TEST_F(Sdl3RendererTest, DisplayScaleFallsBackToOneUnderHeadlessDriver) {
  EXPECT_FLOAT_EQ(renderer_->display_scale(), 1.0f);
}

// FontSizeBase must track font_size_ * display_scale() so the default font
// renders at the compensated size, not the raw --font_size flag value.
// rebuild_font_atlas() (where FontSizeBase is set) only runs lazily off
// fonts_dirty_, on the first begin_frame() -- not in setup() -- so drive one
// frame first.
TEST_F(Sdl3RendererTest, FontSizeBaseIsScaledByDisplayScale) {
  renderer_->begin_frame();
  renderer_->end_frame();
  EXPECT_FLOAT_EQ(ImGui::GetStyle().FontSizeBase, 16.0f * renderer_->display_scale());
}

TEST_F(Sdl3RendererTest, BeginRenderTargetReturnsNonNullTexture) {
  ImTextureID tex = renderer_->begin_render_target(64, 64);
  EXPECT_NE(tex, ImTextureID{});
  renderer_->end_render_target();
}

// Draw a known color into the offscreen target, end it, draw a *different*
// known color, then re-enter the (reused) offscreen target without clearing
// it: if end_render_target() had failed to restore the previous target, the
// second draw would have landed on the offscreen texture and overwritten it.
TEST_F(Sdl3RendererTest, EndRenderTargetRestoresPreviousTarget) {
  SDL_Renderer* r = renderer_->sdl_renderer();

  ImTextureID tex = renderer_->begin_render_target(4, 4);
  ASSERT_NE(tex, ImTextureID{});
  SDL_SetRenderDrawColor(r, 255, 0, 0, 255); // red
  SDL_RenderClear(r);
  renderer_->end_render_target();

  SDL_SetRenderDrawColor(r, 0, 0, 255, 255); // blue
  SDL_RenderClear(r); // must land on the backbuffer, not the offscreen texture

  ImTextureID tex2 = renderer_->begin_render_target(4, 4);
  EXPECT_EQ(tex2, tex); // same size -> cached texture reused
  SDL_Color c = read_top_left_pixel(r);
  EXPECT_EQ(c.r, 255);
  EXPECT_EQ(c.b, 0);
  renderer_->end_render_target();
}

TEST_F(Sdl3RendererTest, EndRenderTargetWithoutBeginIsSafeNoOp) {
  EXPECT_NO_THROW(renderer_->end_render_target());
}

// flush_draw_list() must submit its ImDrawList immediately, landing on
// whatever target begin_render_target() made active -- not deferred to the
// next end_frame() (there is no frame/end_frame() in this test at all).
TEST_F(Sdl3RendererTest, FlushDrawListRendersOntoActiveTarget) {
  SDL_Renderer* r = renderer_->sdl_renderer();

  // A small solid-green source texture, drawn via AddImageQuad -- a real
  // externally-created texture, not the font atlas, so this doesn't depend
  // on ImGui's own texture-management/upload path at all.
  SDL_Texture* src = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 2, 2);
  ASSERT_NE(src, nullptr);
  SDL_Texture* prev = SDL_GetRenderTarget(r);
  SDL_SetRenderTarget(r, src);
  SDL_SetRenderDrawColor(r, 0, 255, 0, 255); // green
  SDL_RenderClear(r);
  SDL_SetRenderTarget(r, prev);

  ImTextureID target_tex = renderer_->begin_render_target(4, 4);
  ASSERT_NE(target_tex, ImTextureID{});
  SDL_SetRenderDrawColor(r, 0, 0, 0, 255); // black, so a lingering clear can't look like the quad
  SDL_RenderClear(r);

  ImDrawList draw_list(ImGui::GetDrawListSharedData());
  draw_list._ResetForNewFrame();
  // _ResetForNewFrame() zero-initializes the clip rect, which the SDL3
  // backend reads as an empty (0,0,0,0) scissor and skips the draw
  // entirely -- a real caller (e.g. genie's render_viewport) always pushes
  // a clip rect covering the target before drawing into it.
  draw_list.PushClipRect(ImVec2(0, 0), ImVec2(4, 4));
  draw_list.AddImageQuad(
      reinterpret_cast<ImTextureID>(src), ImVec2(0, 0), ImVec2(4, 0), ImVec2(4, 4), ImVec2(0, 4));
  draw_list.PopClipRect();

  renderer_->flush_draw_list(draw_list, 4, 4);

  SDL_Color c = read_top_left_pixel(r);
  EXPECT_EQ(c.r, 0);
  EXPECT_EQ(c.g, 255);
  EXPECT_EQ(c.b, 0);

  renderer_->end_render_target();
  SDL_DestroyTexture(src);
}

TEST_F(Sdl3RendererTest, FlushDrawListWithNonPositiveSizeIsSafeNoOp) {
  ImDrawList draw_list(ImGui::GetDrawListSharedData());
  draw_list._ResetForNewFrame();
  EXPECT_NO_THROW(renderer_->flush_draw_list(draw_list, 0, 0));
  EXPECT_NO_THROW(renderer_->flush_draw_list(draw_list, -1, 4));
}
