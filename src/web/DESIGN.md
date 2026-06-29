# Web Renderer — Design Document

## Purpose

`web_renderer` is a backend renderer for wish that runs Dear ImGui headlessly and
streams the resulting draw data to a web browser over HTTP + WebSocket.  The user
opens `http://localhost:<port>` in any browser and sees the live UI rendered in a
WebGL canvas — no native window, no SDL dependency.

This is an optional, compile-time feature gated by the CMake option
`WISH_ENABLE_WEB`.  The existing SDL3 renderer (`WISH_ENABLE_SDL3`) and the web
renderer are independent flags; both can be compiled into the same binary, with
the user selecting the active backend at runtime via `--renderer [sdl3|web]`.

---

## Architecture

### Class Hierarchy

```
renderer            (abstract, src/renderer.hpp)
  └─ imgui_renderer (headless ImGui dispatch, src/imgui/imgui_renderer.cpp)
       ├─ sdl3_renderer   (existing windowed backend)
       └─ web_renderer    (new: headless + embedded HTTP/WebSocket server)
```

`web_renderer` extends `imgui_renderer` exactly as `sdl3_renderer` does — it
inherits the full ImGui draw-dispatch table and only overrides the lifecycle and
frame-boundary methods.

### Key Files

```
src/web_renderer.hpp     public class declaration
src/web_renderer.cpp              implementation + embedded browser client
src/web/DESIGN.md                 this document
tests/test_web_renderer.cpp       unit tests (headless, no browser required)
```

### Embedded Web Server

[cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only, MIT) provides
both the HTTP file server and the WebSocket upgrade.  Its header is large (~7000
lines), so `web_renderer.hpp` exposes no httplib types — a `server_impl` pimpl
struct (defined only in `.cpp`) owns the `httplib::Server` and the list of live
WebSocket sinks.

---

## Rendering Approach: Draw List Streaming

After each `ImGui::Render()`, the server serializes the full `ImDrawData` —
vertex buffers, index buffers, and draw commands — into a compact binary frame
and broadcasts it to every connected browser client.  The browser reconstructs
the geometry and renders it with WebGL2.

**Why draw list streaming rather than widget-state streaming:**

- Draw data is already computed server-side; no extra work to produce it.
- The protocol is simple: arrays of plain structs, no widget semantics in the
  browser.
- Server-side rendering logic (styles, layout, event handling) stays entirely
  in C++.
- This is the approach used by [imgui-ws](https://github.com/ggerganov/imgui-ws),
  the canonical reference for remote ImGui rendering.

---

## Binary Protocol

All frames are WebSocket binary messages.  Byte order is little-endian.

### ATLAS (type `0x01`) — sent once after setup to every new client

```
[u8]   0x01
[u32]  handle = 1
[u32]  png_byte_count
[...]  PNG-encoded RGBA font atlas pixels
```

The font atlas handle is always 1; it matches the `ImTextureID` set via
`io.Fonts->SetTexID`.

### FRAME (type `0x02`) — broadcast every rendered frame

```
[u8]   0x02
[u16]  display_width
[u16]  display_height
[u16]  num_draw_lists

Per draw list:
  [u32]  vtx_count
  [u32]  idx_count
  [u32]  cmd_count
  vtx_count × 20 bytes   — ImDrawVert: [f32 x][f32 y][f32 u][f32 v][u32 col]
  idx_count  × 2  bytes  — u16 index
  cmd_count  × 28 bytes  — [f32 clip×4][u32 tex_id][u32 elem_count][u32 idx_off][u32 vtx_off]
```

### TEXTURE (type `0x03`) — sent once per unique image path

```
[u8]   0x03
[u32]  handle  (≥ 2, stable per resource path)
[u32]  png_byte_count
[...]  PNG-encoded texture pixels
```

### Input (browser → server, JSON text frames)

```json
{ "t": "mousemove",  "x": 123.0, "y": 456.0 }
{ "t": "mousedown",  "x": 123.0, "y": 456.0, "btn": 0 }
{ "t": "mouseup",    "x": 123.0, "y": 456.0, "btn": 0 }
{ "t": "mousewheel", "dx": 0.0,  "dy": -1.5 }
{ "t": "keydown",    "key": 65,  "mod": 0 }
{ "t": "keyup",      "key": 65,  "mod": 0 }
{ "t": "char",       "ch": 65 }
{ "t": "resize",     "w": 1280,  "h": 720 }
```

`btn`: 0=left, 1=right, 2=middle (ImGui convention).
`key`: ImGui key enum value; the browser client maps DOM `KeyboardEvent.code`
via a static lookup table.
`mod`: bitfield — bit 0 = Ctrl, bit 1 = Shift, bit 2 = Alt.

Parsed with nlohmann/json, already bundled via `extern/bison/extern/json`.

---

## Threading Model

```
Render thread (wish server render loop)
  begin_frame():
    drain input_queue_ (wlock) → feed into ImGuiIO
    call imgui_renderer::begin_frame() → ImGui::NewFrame()
  [render_session / render_node calls]
  end_frame():
    ImGui::Render()
    if first frame: send_font_atlas_to_all()
    broadcast_frame()   — serialise ImDrawData, call impl_->broadcast()

httplib thread pool (started in setup(), stopped in teardown())
  on WebSocket open:    add sink to impl_->sinks  (sinks_mutex)
                        send font atlas to new client
  on WebSocket message: push InputEvent to input_queue_  (wlock)
  on WebSocket close:   remove sink from impl_->sinks  (sinks_mutex)
  on GET /:             serve embedded kClientHtml
```

Shared state between the two thread groups:

| Object | Lock | Owner |
|--------|------|-------|
| `input_queue_` | `bison::synchronized<>` | render thread drains; httplib threads push |
| `impl_->sinks` | plain `std::mutex` inside pimpl | httplib threads update; render thread reads for broadcast |

The locks are orthogonal — neither thread holds both simultaneously, so no
deadlock risk.

---

## Texture Handling

**Font atlas** — built in `setup()`.  The atlas pixels are PNG-encoded with
`stbi_write_png_to_func` (available from ImGui's bundled `backends/` tree via
`stb_image_write.h`) and sent as an ATLAS message on the first `end_frame()` and
to every new client on connect.  `ImTextureID{1}` is assigned via
`io.Fonts->SetTexID`.

**User textures** (Image widget) — on the first call to
`get_or_load_texture(src, resource_dir)` for a given path, the file is loaded,
re-encoded to PNG, assigned a stable integer handle (≥ 2), broadcast as a
TEXTURE message, and cached.  Subsequent frames return the cached handle
immediately with no I/O.

---

## Browser Client

The browser client is a single HTML page (~30 lines) with an inline JavaScript
WebGL2 renderer (~400 lines), embedded as the string constant `kClientHtml` in
`src/web_renderer.cpp`.  Embedding avoids any external file serving dependency.

**Rendering pipeline:**
1. Connect WebSocket; send initial `resize` message.
2. On ATLAS / TEXTURE: decode PNG via `createImageBitmap`, upload WebGL texture,
   store under handle.
3. On FRAME: parse binary payload with `DataView`.  For each draw list upload
   VBO + IBO; for each draw command set scissor, bind texture, call
   `gl.drawElements(TRIANGLES, elem_count, UNSIGNED_SHORT, idx_offset × 2)`.
4. Orthographic projection matrix maps screen-pixel coordinates to NDC.
5. On WebSocket close: reconnect after 2 s.

---

## CMake Integration

```cmake
option(WISH_ENABLE_SDL3 "Build the SDL3 renderer"                              ON)
option(WISH_ENABLE_WEB  "Build the web renderer (HTTP+WebSocket browser backend)" OFF)
```

Both options are independent.  When both are `ON`, the app binary includes both
backends and the user selects one at runtime.

```cmake
if(WISH_ENABLE_IMGUI AND WISH_ENABLE_WEB)
  FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.7
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(httplib)

  target_sources(wish_server PRIVATE
    src/web_renderer.hpp
    src/web_renderer.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_WEB_ENABLED)
  target_include_directories(wish_server PRIVATE ${httplib_SOURCE_DIR})

  if(WIN32)
    target_link_libraries(wish_server PRIVATE ws2_32 mswsock)
  endif()
endif()
```

---

## Runtime Renderer Selection

When the app binary contains more than one compiled renderer, the user selects
one at startup:

```
wish-server --renderer sdl3             # windowed SDL3 (default when available)
wish-server --renderer web              # browser via HTTP+WebSocket (port 8080)
wish-server --renderer web --port 9090  # web renderer on custom port
```

Selection logic (in `app/wish_server/wish_server.cpp`):

- Parse `--renderer`; validate against `#ifdef`-guarded compiled-in options.
- If only one renderer is compiled in, the flag is optional and defaults to it.
- If no renderer is compiled (null_renderer only), warn and run headless.
- `--port` applies only to the `web` renderer.

---

## `should_quit()` Semantics

`web_renderer::should_quit()` returns `false` by default — the server runs until
explicitly stopped.  `request_quit()` sets an internal atomic flag to `true`.
There is no automatic quit on zero connected clients (unlike the SDL3 renderer
where closing the window quits).

---

## Design Trade-offs

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Web framework | cpp-httplib | Header-only, MIT, HTTP + WS in one lib; no libwebsockets dep |
| Serialisation | Custom binary | Vertex data is large; JSON inflates 5–10×; no extra lib needed |
| Static assets | Embedded string | Self-contained binary; no file-serving path or resource system entries |
| Multi-client input | Merged into single ImGuiIO | Consistent with imgui-ws; one UI state per server |
| Font encoding | stb_image_write PNG | Already in imgui tree; avoids extra dep |
