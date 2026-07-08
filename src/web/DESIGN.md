# Web Renderer — Design Document

## Purpose

`web_renderer` is a backend renderer for **wish** that runs Dear ImGui headlessly and streams the resulting draw data to a web browser over HTTP + WebSocket. The user opens `http://localhost:<port>` in any browser and sees the live UI rendered in a WebGL2 canvas — no native window, no SDL3 dependency.

This is an optional, compile-time feature gated by the CMake option `WISH_ENABLE_WEB`. The existing SDL3 renderer (`WISH_ENABLE_SDL3`) and the web renderer are independent flags; both can be compiled into the same binary, with the user selecting the active backend at runtime via `--renderer [sdl3|web]`. `-DWISH_ENABLE_SDL3=OFF -DWISH_ENABLE_WEB=ON` builds `wish server` with no windowing/GPU system dependency at all.

---

## Architecture

### Class Hierarchy

```
renderer            (abstract, src/renderer.hpp)
  └─ imgui_renderer (headless ImGui dispatch, src/imgui/imgui_renderer.cpp)
       ├─ sdl3_renderer   (existing windowed backend)
       └─ web_renderer    (headless + civetweb HTTP/WebSocket server)
```

`web_renderer` extends `imgui_renderer` exactly as `sdl3_renderer` does — it inherits the full ImGui draw-dispatch table and only overrides the lifecycle and frame-boundary methods.

### Key Files

```
src/web/web_renderer.hpp/.cpp      public class + lifecycle/frame overrides
src/web/draw_protocol.hpp/.cpp     pure, network-independent wire-format codec
src/web/civetweb_server.hpp/.cpp   pimpl isolating civetweb's C API
src/web/DESIGN.md                  this document
resources/embedded/web/            index.html, client.js, style.css
tests/test_web_renderer.cpp        unit + real-socket tests (no browser required)
```

### Why not `imgui-ws`

An earlier version of this design proposed building on the third-party [imgui-ws](https://github.com/ggerganov/imgui-ws) library. Investigation found that unworkable: `imgui-ws` vendors its own stale copy of Dear ImGui, predating the texture-system rewrite (`ImTextureData`/`ImDrawData::Textures`) that wish's ImGui (`docking` branch) already has. Using it as designed would either fail to compile against wish's current ImGui or require linking two incompatible ImGui copies into one binary (ODR violation / duplicate global context).

Instead:

- **Networking**: [civetweb](https://github.com/civetweb/civetweb) (MIT, FetchContent'd like SDL3/ImGui already are) provides HTTP static file serving and WebSocket upgrade/framing. No hand-rolled protocol code, no coupling to any third-party ImGui fork.
- **Draw-data protocol**: wish defines and owns a small first-party binary format (below) instead of `imgui-ws`'s serialization.
- **Browser client**: a hand-written JS + WebGL2 client (`resources/embedded/web/client.js`, ~500 lines) decodes the protocol and issues WebGL2 draw calls directly. No Emscripten/WASM, no build-time toolchain beyond what wish already needs.

---

## Rendering Approach: Draw List Streaming

After each `ImGui::Render()`, `web_renderer::end_frame()` serializes the frame's `ImDrawData` (vertex buffers, index buffers, draw commands) into a compact binary message and broadcasts it to every connected WebSocket client via `civetweb_server::broadcast()`. The browser client's WebGL2 renderer reconstructs and draws this geometry.

**Why draw list streaming:**

* Draw data is already computed server-side; no extra work to produce it.
* Server-side rendering logic (styles, layout, event handling) stays entirely in C++; the browser is a thin display + input surface.
* Keeping the browser thin avoids duplicating wish's UI-element dispatch logic in JavaScript.

---

## Binary Wire Protocol

Fully specified (and unit-tested) in `src/web/draw_protocol.hpp`. Summary:

All multi-byte integers/floats are little-endian (matches JS `DataView` and x86/ARM host order — no byteswapping on either side). Every WebSocket binary message is one envelope:

```
uint8   msg_type
uint8[3] reserved (zero)
uint32  payload_len
byte[payload_len] payload
```

Server → browser: `0x01 FRAME`, `0x02 TEX_CREATE`, `0x03 TEX_UPDATE`, `0x04 TEX_DESTROY`.
Browser → server: `0x10 INPUT`, `0x11 RESIZE`.

**FRAME** carries display pos/size/framebuffer-scale, then per `ImDrawList`: the vertex buffer (memcpy'd directly — `ImDrawVert` is a stable 20-byte pos/uv/col layout), the index buffer (`ImDrawIdx` is 16-bit in this repo's ImGui config; `draw_protocol.cpp` has a `static_assert` guarding that assumption), and per `ImDrawCmd`: clip rect, wish-assigned texture id, vtx/idx offsets, element count.

**TEX_CREATE/TEX_UPDATE** carry a texture id, pixel format (RGBA32 or Alpha8), full dimensions, and one or more `(x, y, w, h, pixels)` rects — the whole texture for a create, only the changed sub-rects for an update. `web_renderer::end_frame()` builds these by walking `ImDrawData::Textures` each frame and reacting to `ImTextureData::Status` (`WantCreate`/`WantUpdates`/`WantDestroy`) — the current ImGui texture-management model, and the single source of truth for both the font atlas and any user textures.

**INPUT/RESIZE** carry mouse/keyboard/wheel events and canvas size changes from the browser.

Deliberately **not** included: delta-compression against the previous frame, zlib/gzip. `poll_events()` already avoids sending frames when nothing changed; per-frame payload size is an accepted simplicity trade-off.

---

## Texture Handling

**Font atlas and user textures** are handled uniformly: `web_renderer::setup()` sets `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures`, opting into ImGui's incremental texture-management path (required — without it, `ImGui::NewFrame()` asserts, since it expects either this flag or a legacy pre-built atlas). `end_frame()` then walks `*ImDrawData::Textures` every frame:

- `WantCreate` → assign a new wish texture id, call `tex->SetTexID()`, broadcast a `TEX_CREATE`, set `Status = OK`.
- `WantUpdates` → broadcast a `TEX_UPDATE` covering only `tex->Updates[]`, set `Status = OK`.
- `WantDestroy` → broadcast `TEX_DESTROY`, set `Status = Destroyed`.

A browser tab connecting after textures are already `OK` (so no `Status` transition fires this frame) still needs the current texture set: `civetweb_server`'s `on_connect` callback queues the new connection in `web_renderer::pending_sync_`; `end_frame()` drains that queue and re-sends every live texture directly to just that connection via `civetweb_server::send_to()`.

**No `ImGuiBackendFlags_RendererHasVtxOffset`**: wish deliberately never sets this flag. The browser renders with core WebGL2 (GLES 3.0), which has no "draw with base vertex" call; leaving the flag unset keeps ImGui splitting draw lists so `ImDrawCmd::VtxOffset` is always `0`.

**`get_or_load_texture()`** (loading a named image from a session's resource directory, e.g. for an `Image` element) is not yet implemented for the web backend — it currently returns the `imgui_renderer` base's headless no-op. A future change can add `stb_image`-based decoding (already vendored for this purpose) and feed the result through the same `ImDrawData::Textures` upload path.

---

## Threading Model

```
Render thread (wish server render loop)
  begin_frame():
    drain input_queue_ / pending_resize_ into ImGuiIO
    call imgui_renderer::begin_frame() → ImGui::NewFrame()
  [render_session / render_node calls]
  end_frame():
    ImGui::Render()
    walk ImDrawData::Textures → broadcast TEX_CREATE/TEX_UPDATE/TEX_DESTROY
    drain pending_sync_ → send_to() each newly-connected client
    broadcast encode_frame(ImGui::GetDrawData())

civetweb worker threads (started by civetweb_server::start())
  HTTP request:      serve static files from document_root — no wish code runs here
  WS connect/ready:  civetweb_server adds the connection, invokes on_connect
                     (web_renderer queues it in pending_sync_, sets activity_)
  WS message:        civetweb_server invokes on_message; web_renderer decodes
                     it (INPUT → input_queue_, RESIZE → pending_resize_) and
                     sets activity_
  WS close:          civetweb_server removes the connection, invokes on_disconnect
```

Shared state uses `bison::synchronized<T>`, never raw `std::mutex`, per the repo's concurrency convention:

- `civetweb_server`'s connection set (for `broadcast()`).
- `web_renderer::pending_sync_`, `input_queue_`, `pending_resize_`.
- `web_renderer::activity_` is a plain `std::atomic<bool>` (mirrors `sdl3_renderer::quit_`) — `poll_events()` exchanges it to `false`, letting `wish::server::render_loop` skip drawing frames no browser is interacting with.

No ImGui state is ever touched from a civetweb worker thread; texture/frame bytes are serialized entirely on the render thread before crossing to `civetweb_server`.

---

## Browser Client

`resources/embedded/web/{index.html,client.js,style.css}` are plain static assets, folded into wish's existing embedded-resource archive (`resources/embedded/`, packed by the existing miniz + `GenerateResource.cmake` pipeline — no new build machinery). `web_renderer::setup()` extracts the archive once into a process-global temp directory (distinct from the per-session extraction `context::context()` already does) and points `civetweb_server`'s `document_root` at it.

`client.js` decodes the wire protocol above, issues WebGL2 draw calls (with scissor rects from clip rects, and a fragment shader that broadcasts a single-channel Alpha8 texture's red channel into RGBA — WebGL2/GLES3 has no hardware texture swizzle), and forwards DOM mouse/keyboard/resize events back as `INPUT`/`RESIZE` messages.

---

## CMake Integration

```cmake
option(WISH_ENABLE_SDL3      "Build the SDL3 renderer and calculator example"       ON)
option(WISH_ENABLE_WEB       "Build the web renderer (browser + WebSocket backend)" OFF)
```

```cmake
if(WISH_ENABLE_IMGUI AND WISH_ENABLE_WEB)
  # civetweb (WebSocket + HTTP): SSL/testing/standalone-server-executable
  # forced off; wish only needs the C library, and this is a localhost dev
  # tool with no TLS requirement.
  FetchContent_Declare(civetweb ...)
  # stb_image: single-header PNG/JPEG decode, independent of WISH_ENABLE_SDL3
  # (which the web backend must not require).
  FetchContent_Declare(stb ...)
  FetchContent_MakeAvailable(civetweb stb)

  target_sources(wish_server PRIVATE
    src/web/web_renderer.hpp        src/web/web_renderer.cpp
    src/web/draw_protocol.hpp       src/web/draw_protocol.cpp
    src/web/civetweb_server.hpp     src/web/civetweb_server.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_WEB_ENABLED)
  target_link_libraries(wish_server PRIVATE civetweb-c-library stb_image)
endif()
```

`web_renderer.hpp` never includes `<civetweb.h>` (isolated in `civetweb_server.cpp`), so `civetweb-c-library` is linked `PRIVATE` — unlike SDL3 (`PUBLIC`, since `sdl3_renderer.hpp` itself includes `<SDL3/SDL.h>`).

`app/CMakeLists.txt`: `wish-cli`/`wish-server` build under `WISH_ENABLE_IMGUI AND (WISH_ENABLE_SDL3 OR WISH_ENABLE_WEB)` so a web-only configuration doesn't need SDL3 at all. `wish-standalone` (in-process server+client, inherently interactive/windowed) remains SDL3-only.

---

## Runtime Renderer Selection

```
wish server --renderer sdl3                          # windowed SDL3 (default)
wish server --renderer web                            # browser via civetweb (port 8080)
wish server --renderer web --web_port 9090             # web renderer on a custom port
wish server --renderer web --web_bind 0.0.0.0           # bind all interfaces (default: 127.0.0.1)
```

`--web_port`/`--web_bind` are deliberately separate flags from `--port`/`--host`, which are bison's existing TCP RMI transport port/address — an unrelated concept from the web renderer's HTTP/WebSocket endpoint. Selection logic lives in `app/wish_cli/server/wish_server_app.cpp`'s `make_renderer()`, which throws `std::runtime_error` if the requested backend wasn't compiled in or the value is unrecognized.

---

## `should_quit()` Semantics

`web_renderer::should_quit()` returns `false` by default — the server runs until explicitly stopped (Ctrl+C, or `request_quit()`). There is no automatic quit on zero connected clients.

---

## Design Trade-offs

| Decision | Choice | Rationale |
| --- | --- | --- |
| **Networking** | `civetweb` | Small, MIT-licensed, embeddable HTTP+WebSocket library. Handshake/framing/static-file-serving come for free; no new protocol code to write or maintain. |
| **Wire protocol** | First-party, wish-owned | No version coupling to any third-party ImGui fork (the specific failure mode that ruled out `imgui-ws`). Simple enough to unit-test exhaustively (`tests/test_web_renderer.cpp`). |
| **Browser client** | Hand-written JS + WebGL2 | No Emscripten/WASM toolchain requirement. Mirrors the same "draw list streaming, thin client" approach `imgui-ws` used, without the version-coupling risk. |
| **Static assets** | Existing embedded-resource pipeline | Reuses `resources/embedded/` + miniz instead of introducing a second bundling mechanism. Trade-off: `web/*` assets are always embedded and extracted per-session even when `WISH_ENABLE_WEB=OFF` (a few KB, harmless). |
| **Multi-client input** | Merged state | A shared ImGui context across all connected browsers, allowing cooperative interactions with a single centralized server state — matches wish's general multi-client model. |
