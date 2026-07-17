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
