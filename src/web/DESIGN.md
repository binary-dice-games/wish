# Web Renderer — Design Document

## Purpose

`web_renderer` is a backend renderer for **wish** that runs Dear ImGui headlessly and streams the resulting draw data to a web browser over HTTP + WebSocket. The user opens `http://localhost:<port>` in any browser and sees the live UI rendered in a WebGL2 canvas — no native window, no SDL3 dependency.

This is an optional, compile-time feature gated by the CMake option `WISH_ENABLE_WEB`. The existing SDL3 renderer (`WISH_ENABLE_SDL3`) and the web renderer are independent flags; both can be compiled into the same binary, with the user selecting the active backend at runtime via `--renderer [sdl3|web]`. `-DWISH_ENABLE_SDL3=OFF -DWISH_ENABLE_WEB=ON` builds `wish server` with no windowing/GPU system dependency at all.

---

## Architecture

### Class Hierarchy

```
renderer            (abstract, src/server/renderer.hpp)
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
resources/embedded/web/            index.html, client.js, style.css, resource_cache.js
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

Server → browser: `0x01 FRAME`, `0x02 TEX_CREATE`, `0x03 TEX_UPDATE`, `0x04 TEX_DESTROY`, `0x05 TEX_CHECK`, `0x14 CLIPBOARD_WRITE`.
Browser → server: `0x10 INPUT`, `0x11 RESIZE`, `0x12 CACHE_RESPONSE`, `0x13 CLIPBOARD_TEXT`.

**FRAME** carries a `target_id` (see "Offscreen Render Targets" below; `0` means the visible canvas), then display pos/size/framebuffer-scale, then per `ImDrawList`: the vertex buffer (memcpy'd directly — `ImDrawVert` is a stable 20-byte pos/uv/col layout), the index buffer (`ImDrawIdx` is 16-bit in this repo's ImGui config; `draw_protocol.cpp` has a `static_assert` guarding that assumption), and per `ImDrawCmd`: clip rect, wish-assigned texture id, vtx/idx offsets, element count.

**TEX_CREATE/TEX_UPDATE** carry a texture id, pixel format (RGBA32 or Alpha8), full dimensions, and one or more `(x, y, w, h, pixels)` rects — the whole texture for a create, only the changed sub-rects for an update. `web_renderer::end_frame()` builds these by walking `ImDrawData::Textures` each frame and reacting to `ImTextureData::Status` (`WantCreate`/`WantUpdates`/`WantDestroy`) — the current ImGui texture-management model, and the single source of truth for both the font atlas and any user textures.

**TEX_CHECK/CACHE_RESPONSE** implement the persistent browser resource cache (see "Persistent Browser Resource Cache" below): `TEX_CHECK` carries a texture id, format/dimensions, a content-version `crc32`, and a length-prefixed `path` string — metadata only, no pixel payload. `CACHE_RESPONSE` is the browser's reply: a texture id and a `hit`/`miss` byte.

**INPUT/RESIZE** carry mouse/keyboard/wheel events and canvas size changes from the browser.

Deliberately **not** included: delta-compression against the previous frame, zlib/gzip. `poll_events()` already avoids sending frames when nothing changed; per-frame payload size is an accepted simplicity trade-off.

---

## Clipboard Bridging

ImGui's clipboard API (`ImGui::GetClipboardText()`/`SetClipboardText()`)
resolves through `ImGuiPlatformIO::Platform_GetClipboardTextFn`/
`Platform_SetClipboardTextFn` — **not** the older `ImGuiIO::
GetClipboardTextFn`/`SetClipboardTextFn` fields, whose legacy-to-PlatformIO
remap is not reliably active by the time `NewFrame()` first runs in this
renderer. `web_renderer::setup()` sets `ImGui::GetPlatformIO().Platform_
Get/SetClipboardTextFn` directly to two static callbacks
(`web_renderer::get_clipboard_text`/`set_clipboard_text`). Since those
callbacks take an `ImGuiContext*`, not a `void*` user-data pointer, they
recover the owning `web_renderer` via a file-local static instance pointer
(`g_clipboard_renderer`, set in `setup()`, cleared in `teardown()`) —
acceptable under the same "one active renderer per process" assumption
`sdl3_renderer` already makes.

Without any wiring, ImGui falls back to its own internal, session-local
clipboard buffer: copy/paste still works *within* one running app, but
never interoperates with the browser's real OS clipboard (e.g. pasting
JSON copied from an external editor into a `TextEditor` widget).

**Wire messages**: `CLIPBOARD_TEXT` (`0x13`, browser → server) carries the
browser's current OS clipboard text as a plain UTF-8 payload (no JSON,
mirroring `QUERY_TREE`'s raw-text convention). `CLIPBOARD_WRITE` (`0x14`,
server → browser) carries text ImGui just copied/cut, for the browser to
push via `navigator.clipboard.writeText()`.

**Copy/cut** (server → browser) is straightforward: `SetClipboardTextFn`
fires synchronously when some widget's Ctrl+C/X is processed;
`web_renderer::set_clipboard_text()` immediately broadcasts a
`CLIPBOARD_WRITE`. `client.js` writes it to `navigator.clipboard` on
receipt, silently dropping on failure (e.g. a not-yet-granted clipboard
permission) rather than surfacing an error popup.

**Paste** (browser → server) is the hard direction: `navigator.clipboard
.readText()` is async, but `GetClipboardTextFn` is a synchronous ImGui
callback with no way to await a round trip when it's actually called.
`client.js`'s keydown handler special-cases Ctrl+V: it `preventDefault()`s,
awaits `readText()`, sends `CLIPBOARD_TEXT` (cached server-side into
`web_renderer::pending_clipboard_text_`, read back by
`get_clipboard_text()`), and *only then* forwards the actual `V` keydown —
so by the time ImGui processes the paste keystroke, the fresh clipboard
text is already cached. Every other key event that arrives while that read
is still pending (most importantly Ctrl's own keyup, which a real user
releases only after V) is queued client-side and flushed, in original
order, immediately after the delayed keydown goes out — without this, a
fast keyup reaching the server before the deliberately-delayed keydown
corrupts modifier state for that frame and silently breaks the shortcut.

**Prerequisite bug, fixed alongside this feature**: `web_renderer` sent
literal `ImGuiKey_LeftCtrl`/`RightCtrl`/etc. key events but never the
*merged* `ImGuiMod_Ctrl`/`Shift`/`Alt`/`Super` events. `IsKeyDown
(ImGuiMod_Ctrl)` — which every Ctrl-modified ImGui shortcut reads,
including `TextEditor`'s own Ctrl+A/C/V/X — checks a separate pseudo-key
slot (`ImGuiKey_ReservedForModCtrl`) that only an explicit
`AddKeyEvent(ImGuiMod_Ctrl, ...)` populates; sending only the literal L/R
keys left it permanently "up". This meant **no** Ctrl-modified shortcut
ever worked in the web renderer before this fix, unrelated to clipboard
source. `begin_frame()` now tracks each L/R modifier's raw down state
itself and additionally emits the merged event on every change, mirroring
`imgui_impl_sdl3.cpp`'s `ImGui_ImplSDL3_UpdateKeyModifiers()` — the
pattern every official ImGui backend follows. Regression test:
`WebRendererTest.BeginFrame_LeftCtrlKeyEventAlsoSetsMergedModFlag`.

---

## Texture Handling

**Font atlas and user textures** are handled uniformly: `web_renderer::setup()` sets `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures`, opting into ImGui's incremental texture-management path (required — without it, `ImGui::NewFrame()` asserts, since it expects either this flag or a legacy pre-built atlas). `end_frame()` then walks `*ImDrawData::Textures` every frame:

- `WantCreate` → assign a new wish texture id, call `tex->SetTexID()`, broadcast a `TEX_CREATE` (or, for a cacheable texture, send a `TEX_CHECK` to every connected client instead — see "Persistent Browser Resource Cache" below), set `Status = OK`.
- `WantUpdates` → broadcast a `TEX_UPDATE` covering only `tex->Updates[]`, set `Status = OK`.
- `WantDestroy` → broadcast `TEX_DESTROY`, set `Status = Destroyed`.

A browser tab connecting after textures are already `OK` (so no `Status` transition fires this frame) still needs the current texture set: `civetweb_server`'s `on_connect` callback queues the new connection in `web_renderer::pending_sync_`; `end_frame()` drains that queue and re-sends every live texture directly to just that connection via `civetweb_server::send_to()`.

**No `ImGuiBackendFlags_RendererHasVtxOffset`**: wish deliberately never sets this flag. The browser renders with core WebGL2 (GLES 3.0), which has no "draw with base vertex" call; leaving the flag unset keeps ImGui splitting draw lists so `ImDrawCmd::VtxOffset` is always `0`.

**`get_or_load_texture()`** (loading a named image from a session's resource directory, e.g. for an `Image` element) decodes the file with `stb_image` (vendored for this purpose — `WISH_ENABLE_SDL3`'s `SDL3_image` dependency is not guaranteed to be on in a web-only build) into an RGBA32 `ImTextureData`, then calls `ImGui::RegisterUserTexture()` so it flows through the exact same `end_frame()` walk described above, rather than a separate ad hoc broadcast. Results are cached by `src` in `web_renderer::loaded_by_src_` (not the `imgui_renderer` base's `texture_cache_`, which can only store a settled `ImTextureID` — a freshly-registered texture's id isn't known until `end_frame()` assigns one). Consequently the returned id is always null on the frame a texture is first requested — `ImGui::Render()`, which resolves `WantCreate` and assigns the real id, runs later in `end_frame()`, strictly after `render_node()` (and this call) has returned — and only valid from the following frame onward. This mirrors `get_or_load_font()`'s first-call-returns-nothing contract; callers (e.g. `render_image()` in `imgui_ui_renderer.cpp`) already treat a null id as "nothing to draw yet". Loaded textures live for the renderer's lifetime and are unregistered/freed in `teardown()`, same as `sdl3_renderer`'s texture cache.

---

## Offscreen Render Targets

`imgui_renderer` exposes `begin_render_target(w, h)` / `end_render_target()` /
`flush_draw_list(draw_list, w, h)` so a `render_*` function (e.g. genie's
`render_viewport()`) can draw a batch of commands into an offscreen target
and composite the result elsewhere in the same frame — see the doc comments
on those three methods in `src/imgui/imgui_renderer.hpp`. `sdl3_renderer`
implements this with a real `SDL_TEXTUREACCESS_TARGET` GPU texture;
`web_renderer` has no GPU to redirect draws to (it runs ImGui headlessly),
so the redirection has to happen in the *browser* instead.

**Design**: a render target's color attachment is assigned an id from the
same space as ordinary wish texture ids (`web_renderer::next_texture_id_`).
That id serves two roles on the wire: it's the `target_id` a `FRAME` message
carries when it should render into that offscreen target instead of the
canvas, and it's a normal texture id any later `ImDrawCmd` (e.g. an
`AddImageQuad` compositing the rendered scene into the surrounding UI) can
reference — no separate "render target" concept exists client-side beyond
`Renderer.renderTargets`, which just remembers which WebGL framebuffer
backs a given texture id.

**`web_renderer`** mirrors `sdl3_renderer`'s single-slot design exactly: one
cached `render_target_id_`/`render_target_w_`/`render_target_h_`, recreated
(and the old id torn down via an ordinary `TEX_DESTROY`) only when the
requested size changes. `current_target_id_`/`saved_render_target_id_` play
the role `sdl3_renderer::render_target_`/`saved_render_target_` play for a
real ambient GPU target — since there's no ambient server-side state to
save/restore here, they're just plain ids `begin_render_target()`/
`end_render_target()` swap. `flush_draw_list()` wraps its `ImDrawList` in a
throwaway `ImDrawData` (matching `sdl3_renderer`'s own technique) and
broadcasts it immediately via `encode_frame(draw_data, current_target_id_)`
— not deferred to `end_frame()` — so the offscreen target is up to date
before any later draw command in the same frame samples it.

**`client.js`** creates a WebGL2 framebuffer + `RGBA8` color texture per
render-target id on first use (or on a size change), registers the color
texture into the same `Renderer.textures` map ordinary uploaded textures
live in (so compositing "just works"), and frees both via the existing
`TEX_DESTROY` path (`Renderer.destroyTexture()` additionally drops any
`renderTargets` entry for that id). A `FRAME` with a non-zero `target_id`
renders synchronously in the WebSocket message handler rather than through
the usual `requestAnimationFrame` "latest frame wins" path used for the
canvas: that path is fine for the canvas (ImGui always resends the full UI,
so skipping an intermediate frame is free), but a render-target frame is a
side effect other draw commands in the *same* logical frame depend on, and
must not be silently dropped just because a canvas `FRAME` happens to land
in the same tick.

**Vertical flip.** GL's viewport mapping (`NDC y=-1` → texel row 0) is the
same for an FBO as for the default framebuffer, but an ordinarily *uploaded*
image (`texSubImage2D`) has row 0 = the image's own top row. Combined with
this client's projection matrix (world-space top maps to `NDC y=+1`,
matching `imgui_impl_opengl3`, and already correct for the canvas), a scene
*rendered into* an FBO ends up with its bottom at texel row 0 — the
opposite of an uploaded image's convention. Left alone, a composited
render-target quad would appear vertically flipped. `Renderer._drawCmdLists()`
takes a `flipY` flag that swaps the `T`/`B` values feeding the projection
matrix's Y column when drawing into a render target (a standard technique
for flipping an orthographic projection), so the target's texel row 0 ends
up holding the top of the scene like every other texture — no special
casing needed anywhere a render target's texture is later sampled.

No clear is issued before drawing into a render target (mirrors
`sdl3_renderer::flush_draw_list()`, which doesn't clear either) — the
caller's draw commands are expected to fully cover it.

---

## Persistent Browser Resource Cache

Reconnecting (or reopening) the browser client used to re-download every texture's full pixel payload from scratch, even for content that hadn't changed since the last visit. `TEX_CHECK`/`CACHE_RESPONSE` let the server ask "do you already have this cached?" before paying that cost again, with the browser persisting resources across page loads in IndexedDB (`resources/embedded/web/resource_cache.js`).

**Cache identity: `(path, crc32)`.** `path` is the texture's `src`, relative to the session's `resource_dir` (e.g. `"res/icons/folder.png"` or `"uploads/logo.png"`). `crc32` is a content-version number computed once at load time in `get_or_load_texture()`:

- For **embedded assets** (anything under `resource_dir/res/`), it reuses the zip's own per-file CRC-32 — already computed by miniz while unpacking the archive (`resource_store::extract_to()`'s `out_crc32` out-param → `context::embedded_crc32s`, keyed with a `"res/"` prefix to match `src` directly) — rather than re-hashing bytes already trusted.
- For **session-uploaded files** (via `file_service::upload()`, whose RMI payload is just raw `{name, data}` with no checksum), the CRC-32 is computed on the fly from the same bytes `stb_image` just decoded, using `mz_crc32` (the same primitive, so both origins get one consistent versioning scheme).

This metadata is recorded per texture in `web_renderer::texture_meta_` (`{src, crc32, cacheable}`, keyed by `ImTextureData*`). Textures with **no** such metadata — the font atlas, and any other user texture not registered through `get_or_load_texture()` — have no stable on-disk identity and structurally cannot enter the cache-check path; they always use the plain `TEX_CREATE`/`TEX_UPDATE` flow described above.

**The `private/` convention.** Content placed under `resource_dir/private/` (a reserved naming convention documented on `context::resource_dir`, not a separate sandboxing mechanism — `file_service` itself does not treat it specially) is marked `cacheable = false` and is *never* offered to the browser cache, even though it is still cached server-side like any other resource. This is for session content that may hold personal data (uploaded photos, etc.) that must never persist in browser storage. Because `cacheable = false` routes through the exact same code path as "no metadata at all" (the font atlas case above), there is no separate "don't store" flag on the wire to get wrong — a private-path texture is indistinguishable, from the wire protocol's perspective, from a texture the cache mechanism doesn't know about.

**Scope: both a texture's first `WantCreate` and the `pending_sync_` resync path.** The cache-check handshake fires from two places in `end_frame()`:

- **`WantCreate`** (a texture's first-ever creation in a running session) — for a cacheable texture, `TEX_CHECK` is sent via `civetweb_server::send_to()` to every connection in `web_renderer::connected_ids_` (every client currently connected, not just newly-joined ones), instead of the unconditional `broadcast()` a non-cacheable texture still gets. This is deliberately *not* scoped to "a client freshly connected this session" — the common case is the opposite: the same browser tab has been open for a while and is simply loading an image for the first time (e.g. the user just typed a path into an `Image` element), and that tab may already have this exact `(path, crc32)` persisted from an *earlier run* of the app. Skipping the check here would mean the persistent cache never pays off for the most frequent path.
- **`pending_sync_`** (an already-`OK` texture resent to a *newly-(re)connected* browser) — unchanged from before: still needed for a texture that reached `OK` on some earlier frame, since `WantCreate` only fires once per texture's lifetime and a later-joining connection never sees that transition.

A texture handled by the `WantCreate` branch this frame is tracked in a per-`end_frame()`-call `created_this_frame` set so the `pending_sync_` drain (which runs later in the same call) skips it — otherwise a client that connects on the exact frame a texture is first created would get sent two `TEX_CHECK`s for the same id.

**Handshake:** for each cacheable texture offered this way, the server records the texture id as awaiting a reply (`web_renderer::awaiting_cache_response_`, keyed per connection) and the browser looks up `(path, crc32)` in its IndexedDB store (`WishResourceCache.lookup()`):

- **Hit** — the browser builds the WebGL texture directly from the cached bytes and replies `CACHE_RESPONSE{hit=true}`. The server sends nothing further for that texture/connection.
- **Miss** — the browser replies `CACHE_RESPONSE{hit=false}`, remembering the texture id is pending a store. The server (on the next `end_frame()`, draining `cache_response_queue_`) sends a normal full `TEX_CREATE`; the browser's `client.js` then persists those pixels to IndexedDB under `(path, crc32)` with a timestamp.

**Accepted trade-off — brief pending-check window.** No buffering exists for a `FRAME`, `TEX_UPDATE`, or `TEX_DESTROY` referencing a texture whose `TEX_CHECK` hasn't resolved yet on some connection: `client.js`'s renderer already falls back to a solid white texture for any unrecognized `textureId` in a `FRAME` (see `Renderer.render()`), `uploadTexture()` tolerates a `TEX_UPDATE` for an id it hasn't created yet (allocates it on the spot), and `destroyTexture()` no-ops for an unknown id — so the texture just renders blank (or, for a stray sub-rect update, partially populated) for the one round-trip until the hit/miss resolves, then snaps in. A miss's eventual full `TEX_CREATE` always carries the texture's *current* pixels (built from live `ImTextureData` state, not a stale snapshot), so any `WantUpdates` that raced the still-open check are folded in for free. This was judged simpler than adding real per-connection buffering for a window that's normally a single network round-trip, and now covers `WantCreate` as well as `pending_sync_`.

**TTL eviction.** `resource_cache.js` stores a `storedAt` timestamp per entry and sweeps entries older than `WishResourceCache.TTL_MS` (30 days, a top-of-file constant) once at client startup, via an IndexedDB index on `storedAt` rather than a full-store scan.

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
    walk ImDrawData::Textures → broadcast TEX_CREATE/TEX_UPDATE/TEX_DESTROY,
      or send_to() TEX_CHECK to every connected_ids_ entry for a cacheable
      WantCreate
    drain pending_sync_ → send_to() each newly-connected client (skipping
      textures already handled by the WantCreate walk above this frame)
    broadcast encode_frame(ImGui::GetDrawData())

civetweb worker threads (started by civetweb_server::start())
  HTTP request:      serve static files from document_root — no wish code runs here
  WS connect/ready:  civetweb_server adds the connection, invokes on_connect
                     (web_renderer adds it to connected_ids_, queues it in
                     pending_sync_, sets activity_)
  WS message:        civetweb_server invokes on_message; web_renderer decodes
                     it (INPUT → input_queue_, RESIZE → pending_resize_) and
                     sets activity_
  WS close:          civetweb_server removes the connection, invokes
                     on_disconnect (web_renderer removes it from
                     connected_ids_ and awaiting_cache_response_)
```

Shared state uses `bison::synchronized<T>`, never raw `std::mutex`, per the repo's concurrency convention:

- `civetweb_server`'s connection set (for `broadcast()`).
- `web_renderer::pending_sync_`, `connected_ids_`, `input_queue_`, `pending_resize_`.
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
