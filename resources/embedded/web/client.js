// MIT License © 2025 Binary Dice Games
//
// wish web renderer browser client.
//
// Decodes the binary protocol documented in src/web/draw_protocol.hpp,
// renders it with WebGL2, and forwards DOM input events back to the server
// in the same protocol. This file is embedded into the wish server binary
// via the existing resources/embedded/ pipeline (see src/web/DESIGN.md) and
// served as a static asset by web_renderer's civetweb instance.
//
// wish's server never sets ImGuiBackendFlags_RendererHasVtxOffset, so every
// ImDrawCmd::VtxOffset is always 0 -- this client relies on that and never
// implements WebGL2's non-existent "draw with base vertex" (core GLES 3.0
// has no such call).

(function () {
  "use strict";

  // Module-level singletons -- TextEncoder/TextDecoder are stateless; a fresh
  // one per call is pure allocation waste.
  const TEXT_ENCODER = new TextEncoder();
  const TEXT_DECODER = new TextDecoder();

  // ── wire format constants (must match draw_protocol.hpp exactly) ─────────

  const MSG = {
    FRAME: 0x01,
    TEX_CREATE: 0x02,
    TEX_UPDATE: 0x03,
    TEX_DESTROY: 0x04,
    TEX_CHECK: 0x05,
    INPUT: 0x10,
    RESIZE: 0x11,
    CACHE_RESPONSE: 0x12,
    CLIPBOARD_TEXT: 0x13, // browser -> server: OS clipboard text before a paste
    CLIPBOARD_WRITE: 0x14, // server -> browser: text ImGui just copied/cut
    // Only meaningful when the server was built with -DWISH_ENABLE_AUTOMATION=ON
    // (see src/automation/DESIGN.md) -- a server built without it never
    // sends TREE_SNAPSHOT/LOG_EVENT and silently ignores QUERY_TREE, so
    // window.wish.getTree() simply never resolves and window.wish.logs
    // simply never grows against such a server.
    QUERY_TREE: 0x20,
    TREE_SNAPSHOT: 0x21,
    LOG_EVENT: 0x22, // pushed live, no request -- see window.wish.logs below
    REQUEST_RENDER: 0x23, // see window.wish.requestRender() below
  };

  const INPUT_KIND = {
    MOUSE_MOVE: 0,
    MOUSE_BUTTON: 1,
    MOUSE_WHEEL: 2,
    KEY: 3,
    CHAR: 4,
  };

  const TEX_FORMAT = {
    RGBA32: 0,
    ALPHA8: 1,
  };

  // ImGuiKey enum values (see imgui.h) for the subset of keys this client
  // maps from DOM KeyboardEvent.code. Named keys start at 512
  // (ImGuiKey_NamedKey_BEGIN); gamepad/rare keys are not mapped.
  const IMGUI_KEY = (() => {
    const names = [
      "Tab", "LeftArrow", "RightArrow", "UpArrow", "DownArrow", "PageUp", "PageDown",
      "Home", "End", "Insert", "Delete", "Backspace", "Space", "Enter", "Escape",
      "LeftCtrl", "LeftShift", "LeftAlt", "LeftSuper",
      "RightCtrl", "RightShift", "RightAlt", "RightSuper", "Menu",
      "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
      "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
      "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
      "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
      "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
      "Apostrophe", "Comma", "Minus", "Period", "Slash", "Semicolon", "Equal",
      "LeftBracket", "Backslash", "RightBracket", "GraveAccent",
    ];
    const table = {};
    names.forEach((name, i) => (table[name] = 512 + i));
    return table;
  })();

  // DOM KeyboardEvent.code -> ImGuiKey name (only entries that differ from a
  // 1:1 name match, e.g. "Digit1" -> "1", "KeyA" -> "A").
  const DOM_CODE_TO_KEY_NAME = (() => {
    const map = {
      ArrowLeft: "LeftArrow", ArrowRight: "RightArrow", ArrowUp: "UpArrow", ArrowDown: "DownArrow",
      ControlLeft: "LeftCtrl", ControlRight: "RightCtrl",
      ShiftLeft: "LeftShift", ShiftRight: "RightShift",
      AltLeft: "LeftAlt", AltRight: "RightAlt",
      MetaLeft: "LeftSuper", MetaRight: "RightSuper",
      Quote: "Apostrophe", BracketLeft: "LeftBracket", BracketRight: "RightBracket",
      Backquote: "GraveAccent",
    };
    for (let d = 0; d <= 9; ++d) map["Digit" + d] = String(d);
    for (const c of "ABCDEFGHIJKLMNOPQRSTUVWXYZ") map["Key" + c] = c;
    for (let f = 1; f <= 24; ++f) map["F" + f] = "F" + f;
    ["Tab", "Home", "End", "PageUp", "PageDown", "Insert", "Delete", "Backspace",
      "Space", "Enter", "Escape", "Menu"].forEach((n) => (map[n] = n));
    return map;
  })();

  function domCodeToImGuiKey(code) {
    const name = DOM_CODE_TO_KEY_NAME[code];
    return name !== undefined ? IMGUI_KEY[name] : undefined;
  }

  // ── byte-buffer writer (input/resize messages back to the server) ────────

  function encodeEnvelope(type, payloadBytes) {
    const out = new Uint8Array(8 + payloadBytes.length);
    const view = new DataView(out.buffer);
    view.setUint8(0, type);
    view.setUint32(4, payloadBytes.length, true);
    out.set(payloadBytes, 8);
    return out;
  }

  function encodeMouseMove(x, y) {
    const buf = new ArrayBuffer(9);
    const v = new DataView(buf);
    v.setUint8(0, INPUT_KIND.MOUSE_MOVE);
    v.setFloat32(1, x, true);
    v.setFloat32(5, y, true);
    return encodeEnvelope(MSG.INPUT, new Uint8Array(buf));
  }

  function encodeMouseButton(button, down, x, y) {
    const buf = new ArrayBuffer(11);
    const v = new DataView(buf);
    v.setUint8(0, INPUT_KIND.MOUSE_BUTTON);
    v.setUint8(1, button);
    v.setUint8(2, down ? 1 : 0);
    v.setFloat32(3, x, true);
    v.setFloat32(7, y, true);
    return encodeEnvelope(MSG.INPUT, new Uint8Array(buf));
  }

  function encodeMouseWheel(dx, dy) {
    const buf = new ArrayBuffer(9);
    const v = new DataView(buf);
    v.setUint8(0, INPUT_KIND.MOUSE_WHEEL);
    v.setFloat32(1, dx, true);
    v.setFloat32(5, dy, true);
    return encodeEnvelope(MSG.INPUT, new Uint8Array(buf));
  }

  function encodeKey(keyCode, down) {
    const buf = new ArrayBuffer(6);
    const v = new DataView(buf);
    v.setUint8(0, INPUT_KIND.KEY);
    v.setUint32(1, keyCode, true);
    v.setUint8(5, down ? 1 : 0);
    return encodeEnvelope(MSG.INPUT, new Uint8Array(buf));
  }

  function encodeChar(codepoint) {
    const buf = new ArrayBuffer(5);
    const v = new DataView(buf);
    v.setUint8(0, INPUT_KIND.CHAR);
    v.setUint32(1, codepoint, true);
    return encodeEnvelope(MSG.INPUT, new Uint8Array(buf));
  }

  function encodeResize(width, height, dpr) {
    const buf = new ArrayBuffer(12);
    const v = new DataView(buf);
    v.setFloat32(0, width, true);
    v.setFloat32(4, height, true);
    v.setFloat32(8, dpr, true);
    return encodeEnvelope(MSG.RESIZE, new Uint8Array(buf));
  }

  function encodeCacheResponse(textureId, hit) {
    const buf = new ArrayBuffer(5);
    const v = new DataView(buf);
    v.setUint32(0, textureId, true);
    v.setUint8(4, hit ? 1 : 0);
    return encodeEnvelope(MSG.CACHE_RESPONSE, new Uint8Array(buf));
  }

  // QUERY_TREE's payload is just UTF-8 JSON text (see draw_protocol.hpp's
  // decode_query_tree_message(), which passes it through unparsed) -- no
  // fixed-width fields to encode here, unlike the other outbound messages.
  function encodeQueryTree(requestId, root) {
    const json = JSON.stringify({ request_id: requestId, root: root });
    return encodeEnvelope(MSG.QUERY_TREE, TEXT_ENCODER.encode(json));
  }

  function encodeRequestRender() {
    return encodeEnvelope(MSG.REQUEST_RENDER, new Uint8Array(0));
  }

  // CLIPBOARD_TEXT's payload is plain UTF-8 text (see draw_protocol.hpp's
  // decode_clipboard_text_message()) -- no fixed-width fields either.
  function encodeClipboardText(text) {
    return encodeEnvelope(MSG.CLIPBOARD_TEXT, TEXT_ENCODER.encode(text));
  }

  // ── WebGL2 renderer ────────────────────────────────────────────────────────

  const VERTEX_SHADER_SRC = `#version 300 es
    precision highp float;
    uniform mat4 uProjMtx;
    in vec2 aPos;
    in vec2 aUV;
    in vec4 aColor;
    out vec2 vUV;
    out vec4 vColor;
    void main() {
      vUV = aUV;
      vColor = aColor;
      gl_Position = uProjMtx * vec4(aPos, 0.0, 1.0);
    }`;

  // WebGL2 (GLES 3.0) has no texture swizzle, so Alpha8 textures (the font
  // atlas) are uploaded as single-channel and broadcast r->rgba here,
  // matching what desktop backends get for free via GL_TEXTURE_SWIZZLE.
  const FRAGMENT_SHADER_SRC = `#version 300 es
    precision mediump float;
    uniform sampler2D uTexture;
    uniform bool uIsAlpha;
    in vec2 vUV;
    in vec4 vColor;
    out vec4 outColor;
    void main() {
      vec4 t = texture(uTexture, vUV);
      vec4 texColor = uIsAlpha ? vec4(t.r, t.r, t.r, t.r) : t;
      outColor = vColor * texColor;
    }`;

  function compileShader(gl, type, src) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, src);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      throw new Error("shader compile failed: " + log);
    }
    return shader;
  }

  function createProgram(gl) {
    const vs = compileShader(gl, gl.VERTEX_SHADER, VERTEX_SHADER_SRC);
    const fs = compileShader(gl, gl.FRAGMENT_SHADER, FRAGMENT_SHADER_SRC);
    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const log = gl.getProgramInfoLog(program);
      gl.deleteProgram(program);
      throw new Error("program link failed: " + log);
    }
    return program;
  }

  class Renderer {
    constructor(canvas) {
      this.canvas = canvas;
      const gl = canvas.getContext("webgl2");
      if (!gl)
        throw new Error("WebGL2 is not available in this browser");
      this.gl = gl;

      this.program = createProgram(gl);
      this.locProjMtx = gl.getUniformLocation(this.program, "uProjMtx");
      this.locTexture = gl.getUniformLocation(this.program, "uTexture");
      this.locIsAlpha = gl.getUniformLocation(this.program, "uIsAlpha");
      this.locPos = gl.getAttribLocation(this.program, "aPos");
      this.locUV = gl.getAttribLocation(this.program, "aUV");
      this.locColor = gl.getAttribLocation(this.program, "aColor");

      this.vao = gl.createVertexArray();
      this.vbo = gl.createBuffer();
      this.ebo = gl.createBuffer();
      gl.bindVertexArray(this.vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
      const STRIDE = 20; // ImDrawVert: pos(f32,f32) uv(f32,f32) col(u32) = 20 bytes
      gl.enableVertexAttribArray(this.locPos);
      gl.vertexAttribPointer(this.locPos, 2, gl.FLOAT, false, STRIDE, 0);
      gl.enableVertexAttribArray(this.locUV);
      gl.vertexAttribPointer(this.locUV, 2, gl.FLOAT, false, STRIDE, 8);
      gl.enableVertexAttribArray(this.locColor);
      gl.vertexAttribPointer(this.locColor, 4, gl.UNSIGNED_BYTE, true, STRIDE, 16);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ebo);
      gl.bindVertexArray(null);

      // textureId -> { tex: WebGLTexture, isAlpha: bool, width, height }
      this.textures = new Map();
      this.whiteTexture = this._createSolidTexture(255, 255, 255, 255);

      // Offscreen render targets (see "Offscreen Render Targets" in
      // src/web/DESIGN.md): targetId -> { fbo: WebGLFramebuffer, width, height }.
      // The color attachment backing each entry is also registered in
      // `this.textures` under the same id, so an ordinary draw command can
      // sample it like any other texture once rendered.
      this.renderTargets = new Map();

      // Reused every _drawCmdLists() call -- the orthographic projection is
      // recomputed in place rather than allocating a fresh Float32Array(16)
      // per frame.
      this._proj = new Float32Array(16);
    }

    _createSolidTexture(r, g, b, a) {
      const gl = this.gl;
      const tex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([r, g, b, a]));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      return { tex, isAlpha: false, width: 1, height: 1 };
    }

    // Handles both TEX_CREATE (one rect covering the whole texture) and
    // TEX_UPDATE (one or more sub-rects) -- the wire format is identical,
    // only the rect list's coverage differs.
    uploadTexture(textureId, format, width, height, rects) {
      const gl = this.gl;
      const isAlpha = format === TEX_FORMAT.ALPHA8;
      let entry = this.textures.get(textureId);
      const wholeTextureRect = rects.length === 1 && rects[0].x === 0 && rects[0].y === 0 &&
          rects[0].w === width && rects[0].h === height;

      if (!entry || entry.width !== width || entry.height !== height || wholeTextureRect) {
        const tex = entry ? entry.tex : gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        const internalFormat = isAlpha ? gl.R8 : gl.RGBA8;
        const glFormat = isAlpha ? gl.RED : gl.RGBA;
        // Allocate storage; if this call is itself the whole-texture rect,
        // the loop below uploads its pixels via texSubImage2D right after.
        gl.texImage2D(gl.TEXTURE_2D, 0, internalFormat, width, height, 0, glFormat, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        entry = { tex, isAlpha, width, height };
        this.textures.set(textureId, entry);
      } else {
        gl.bindTexture(gl.TEXTURE_2D, entry.tex);
      }

      const glFormat = isAlpha ? gl.RED : gl.RGBA;
      // Unpack alignment defaults to 4, which corrupts tightly-packed rows
      // whose width isn't a multiple of 4 (routine for font atlas glyph
      // sub-rects) -- packing is always tight in this wire format.
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      for (const rect of rects)
        gl.texSubImage2D(gl.TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, glFormat, gl.UNSIGNED_BYTE, rect.pixels);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    }

    destroyTexture(textureId) {
      const entry = this.textures.get(textureId);
      if (entry) {
        this.gl.deleteTexture(entry.tex);
        this.textures.delete(textureId);
      }
      // A render target's color attachment lives in `this.textures` too
      // (see renderToTarget()) -- also free the framebuffer wrapping it.
      const rt = this.renderTargets.get(textureId);
      if (rt) {
        this.gl.deleteFramebuffer(rt.fbo);
        this.renderTargets.delete(textureId);
      }
    }

    // Shared by render() (canvas) and renderToTarget() (offscreen render
    // target): issues the actual draw calls for `frame` against whichever
    // framebuffer is currently bound, at (fbWidth, fbHeight).
    //
    // `flipY`: GL's viewport mapping (NDC y=-1 -> texel row 0) is the same
    // for an FBO as for the canvas, but an ordinarily *uploaded* image
    // (texSubImage2D) has row 0 = the image's own top row. With this
    // client's projection (world-space top -> NDC y=+1, matching
    // imgui_impl_opengl3, already correct for the canvas), a scene *rendered
    // into* an FBO would end up with its bottom at texel row 0 -- the
    // opposite of an uploaded image's convention, so a render target's
    // composited quad would appear vertically flipped. Fix it at the
    // source: swap the T/B values feeding the projection's Y column when
    // drawing into a render target (a standard technique for flipping an
    // orthographic projection) so the target's texel row 0 ends up holding
    // the top of the scene like every other texture -- see "Offscreen
    // Render Targets" in src/web/DESIGN.md.
    _drawCmdLists(frame, fbWidth, fbHeight, flipY) {
      const gl = this.gl;

      gl.viewport(0, 0, fbWidth, fbHeight);
      gl.enable(gl.BLEND);
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.CULL_FACE);
      gl.enable(gl.SCISSOR_TEST);

      gl.useProgram(this.program);
      const L = frame.displayPos.x, R = frame.displayPos.x + frame.displaySize.x;
      const top = frame.displayPos.y, bottom = frame.displayPos.y + frame.displaySize.y;
      const T = flipY ? bottom : top, B = flipY ? top : bottom;
      // Column-major orthographic projection matching imgui_impl_opengl3,
      // written into the reused buffer rather than a fresh allocation.
      const proj = this._proj;
      proj[0] = 2 / (R - L); proj[1] = 0; proj[2] = 0; proj[3] = 0;
      proj[4] = 0; proj[5] = 2 / (T - B); proj[6] = 0; proj[7] = 0;
      proj[8] = 0; proj[9] = 0; proj[10] = -1; proj[11] = 0;
      proj[12] = (R + L) / (L - R); proj[13] = (T + B) / (B - T); proj[14] = 0; proj[15] = 1;
      gl.uniformMatrix4fv(this.locProjMtx, false, proj);
      gl.uniform1i(this.locTexture, 0);
      gl.activeTexture(gl.TEXTURE0);

      gl.bindVertexArray(this.vao);
      for (const list of frame.cmdLists) {
        gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
        gl.bufferData(gl.ARRAY_BUFFER, list.vtx, gl.DYNAMIC_DRAW);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ebo);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, list.idx, gl.DYNAMIC_DRAW);

        for (const cmd of list.cmds) {
          if (cmd.vtxOffset !== 0) {
            // Would require glDrawElementsBaseVertex, unavailable in core
            // WebGL2/GLES3 -- wish's server never sets
            // ImGuiBackendFlags_RendererHasVtxOffset, so this should not
            // happen; skip defensively rather than drawing garbage.
            console.warn("[wish] dropping draw command with nonzero vtxOffset:", cmd.vtxOffset);
            continue;
          }

          const clipX0 = (cmd.clipRect.x0 - frame.displayPos.x) * frame.fbScale.x;
          const clipY0 = (cmd.clipRect.y0 - frame.displayPos.y) * frame.fbScale.y;
          const clipX1 = (cmd.clipRect.x1 - frame.displayPos.x) * frame.fbScale.x;
          const clipY1 = (cmd.clipRect.y1 - frame.displayPos.y) * frame.fbScale.y;
          if (clipX1 <= clipX0 || clipY1 <= clipY0)
            continue;
          gl.scissor(Math.round(clipX0), Math.round(fbHeight - clipY1), Math.round(clipX1 - clipX0),
              Math.round(clipY1 - clipY0));

          const entry = this.textures.get(cmd.textureId) || this.whiteTexture;
          gl.bindTexture(gl.TEXTURE_2D, entry.tex);
          gl.uniform1i(this.locIsAlpha, entry.isAlpha ? 1 : 0);

          gl.drawElements(gl.TRIANGLES, cmd.elemCount, gl.UNSIGNED_SHORT, cmd.idxOffset * 2);
        }
      }
      gl.bindVertexArray(null);
    }

    // frame: { targetId, displayPos, displaySize, fbScale, cmdLists: [{vtx, idx, cmds: [...]}] }
    render(frame) {
      const gl = this.gl;
      const fbWidth = Math.max(1, Math.round(frame.displaySize.x * frame.fbScale.x));
      const fbHeight = Math.max(1, Math.round(frame.displaySize.y * frame.fbScale.y));
      if (this.canvas.width !== fbWidth) this.canvas.width = fbWidth;
      if (this.canvas.height !== fbHeight) this.canvas.height = fbHeight;

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.clearColor(0.117, 0.117, 0.117, 1.0); // matches sdl3_renderer's clear color
      gl.clear(gl.COLOR_BUFFER_BIT);

      this._drawCmdLists(frame, fbWidth, fbHeight, /*flipY=*/false);
    }

    // Renders `frame` into the offscreen render target identified by
    // `frame.targetId` instead of the canvas, creating (or resizing) its
    // framebuffer + color texture on demand.
    //
    // Unlike sdl3_renderer::flush_draw_list() (which never clears its
    // target either), this *does* clear to transparent black first. A
    // texture's real id here is only valid from the frame *after* it's
    // first requested (see get_or_load_texture()'s doc comment in
    // web_renderer.hpp) -- a draw command referencing it before then falls
    // back to the opaque whiteTexture placeholder (see the `entry ||
    // this.whiteTexture` fallback below), which is opaque everywhere,
    // including the texture's actually-transparent background pixels. sdl3
    // never hits this because get_or_load_texture() there uploads
    // synchronously on the very first call -- no such placeholder frame
    // exists. Without clearing, that one opaque frame would permanently
    // "poison" the target's alpha: blending a now-correct, genuinely
    // transparent pixel onto an already-opaque destination leaves the
    // destination opaque (over-blending a transparent source is a no-op),
    // so the placeholder's opacity could never be undone by a later,
    // correct redraw. Clearing first makes every flush a fresh start, so
    // the target self-corrects the moment the real texture becomes
    // available -- exactly like the canvas already does via its own
    // per-frame clear in render(). See "Offscreen Render Targets" in
    // src/web/DESIGN.md.
    renderToTarget(frame) {
      const gl = this.gl;
      const id = frame.targetId;
      const w = Math.max(1, Math.round(frame.displaySize.x));
      const h = Math.max(1, Math.round(frame.displaySize.y));

      let rt = this.renderTargets.get(id);
      if (!rt || rt.width !== w || rt.height !== h) {
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        const fbo = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);

        rt = { fbo, width: w, height: h };
        this.renderTargets.set(id, rt);
        // Registered under the same id as the render target itself, so an
        // ordinary AddImageQuad draw command compositing this scene
        // elsewhere samples it exactly like any other texture.
        this.textures.set(id, { tex, isAlpha: false, width: w, height: h });
      }

      gl.bindFramebuffer(gl.FRAMEBUFFER, rt.fbo);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      this._drawCmdLists(frame, w, h, /*flipY=*/true);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }
  }

  // ── frame decoding ───────────────────────────────────────────────────────

  function decodeFrame(view, offset) {
    const targetId = view.getUint32(offset, true);
    offset += 4;
    const displayPos = { x: view.getFloat32(offset, true), y: view.getFloat32(offset + 4, true) };
    const displaySize = { x: view.getFloat32(offset + 8, true), y: view.getFloat32(offset + 12, true) };
    const fbScale = { x: view.getFloat32(offset + 16, true), y: view.getFloat32(offset + 20, true) };
    offset += 24;
    offset += 8; // total_vtx_count, total_idx_count (informational only)
    const cmdListCount = view.getUint32(offset, true);
    offset += 4;

    const cmdLists = [];
    for (let i = 0; i < cmdListCount; ++i) {
      const vtxCount = view.getUint32(offset, true);
      const idxCount = view.getUint32(offset + 4, true);
      offset += 8;
      const vtxBytes = vtxCount * 20;
      const vtx = new Uint8Array(view.buffer, view.byteOffset + offset, vtxBytes);
      offset += vtxBytes;
      const idxBytes = idxCount * 2;
      // Uint16Array requires 2-byte alignment; the envelope/FRAME header is
      // a fixed multiple of 4 bytes and every vtx block is a multiple of 20
      // bytes, so this offset is always even.
      const idx = new Uint16Array(view.buffer, view.byteOffset + offset, idxCount);
      offset += idxBytes;

      const drawCmdCount = view.getUint32(offset, true);
      offset += 4;
      const cmds = [];
      for (let c = 0; c < drawCmdCount; ++c) {
        cmds.push({
          clipRect: {
            x0: view.getFloat32(offset, true), y0: view.getFloat32(offset + 4, true),
            x1: view.getFloat32(offset + 8, true), y1: view.getFloat32(offset + 12, true),
          },
          textureId: view.getUint32(offset + 16, true),
          vtxOffset: view.getUint32(offset + 20, true),
          idxOffset: view.getUint32(offset + 24, true),
          elemCount: view.getUint32(offset + 28, true),
        });
        offset += 32;
      }
      cmdLists.push({ vtx, idx, cmds });
    }
    return { targetId, displayPos, displaySize, fbScale, cmdLists };
  }

  function decodeTextureMessage(view, offset) {
    const textureId = view.getUint32(offset, true);
    const format = view.getUint8(offset + 4);
    const width = view.getUint32(offset + 5, true);
    const height = view.getUint32(offset + 9, true);
    let o = offset + 13;
    const rectCount = view.getUint32(o, true);
    o += 4;
    const bpp = format === TEX_FORMAT.ALPHA8 ? 1 : 4;
    const rects = [];
    for (let i = 0; i < rectCount; ++i) {
      const x = view.getUint32(o, true), y = view.getUint32(o + 4, true);
      const w = view.getUint32(o + 8, true), h = view.getUint32(o + 12, true);
      o += 16;
      const n = w * h * bpp;
      const pixels = new Uint8Array(view.buffer, view.byteOffset + o, n);
      o += n;
      rects.push({ x, y, w, h, pixels });
    }
    return { textureId, format, width, height, rects };
  }

  // TEX_CHECK shares its texture_id/format/width/height layout with
  // TEX_CREATE/TEX_UPDATE (see decodeTextureMessage), then adds a crc32 and
  // a length-prefixed path instead of pixel rects -- see draw_protocol.hpp.
  function decodeTextureCheck(view, offset) {
    const textureId = view.getUint32(offset, true);
    const format = view.getUint8(offset + 4);
    const width = view.getUint32(offset + 5, true);
    const height = view.getUint32(offset + 9, true);
    const crc32 = view.getUint32(offset + 13, true);
    const pathLen = view.getUint32(offset + 17, true);
    const pathBytes = new Uint8Array(view.buffer, view.byteOffset + offset + 21, pathLen);
    const path = TEXT_DECODER.decode(pathBytes);
    return { textureId, format, width, height, crc32, path };
  }

  // ── wiring: WebSocket + input capture ────────────────────────────────────

  const canvas = document.getElementById("wish-canvas");
  const renderer = new Renderer(canvas);

  // Draw the latest decoded FRAME inside a requestAnimationFrame callback
  // rather than synchronously in the WebSocket "message" handler. Chrome's
  // compositor schedules canvas repaints around rAF; raw WebGL draws issued
  // from an arbitrary background task (like a socket message) can be left
  // uncomposited -- the backing store updates, but nothing new reaches the
  // screen -- until some *other* rAF-triggering event (e.g. a mouse move)
  // happens to come along. Without this, a server-pushed UI change that
  // isn't accompanied by user input (e.g. a modal dialog another widget's
  // click handler just opened) can sit invisible for seconds after arriving.
  // "Latest frame wins": if several FRAME messages arrive before the next
  // rAF tick, only the most recent is drawn -- the server always resends
  // the complete scene every frame (ImGui is immediate-mode), so nothing is
  // lost by skipping intermediate ones. Only the raw DataView is held here;
  // decodeFrame() runs inside the rAF callback so superseded frames are
  // never decoded at all.
  let pendingFrameView = null;
  let frameRafScheduled = false;
  function scheduleFrameRender() {
    if (frameRafScheduled) return;
    frameRafScheduled = true;
    requestAnimationFrame(() => {
      frameRafScheduled = false;
      if (pendingFrameView) {
        const frame = decodeFrame(pendingFrameView, 8);
        pendingFrameView = null;
        renderer.render(frame);
        // First FRAME processed means the canvas now shows something real --
        // Playwright scripts wait on this before interacting (see
        // src/automation/DESIGN.md's "Readiness").
        window.wish.ready = true;
        // Resolve exactly one pending requestRender() promise per frame
        // actually drawn -- see window.wish.requestRender()'s doc comment.
        const resolve = window.wish._renderResolvers.shift();
        if (resolve) resolve();
      }
    });
  }

  // Set (by TEX_CHECK) for a texture the resource cache didn't have -- the
  // resulting TEX_CREATE's pixels get persisted for next time. Cleared once
  // that TEX_CREATE arrives. See resource_cache.js and src/web/DESIGN.md.
  const pendingCacheStore = new Map(); // textureId -> {path, crc32}

  if (window.WishResourceCache) {
    WishResourceCache.open().then(() => WishResourceCache.sweepExpired(WishResourceCache.TTL_MS));
  }

  const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(proto + "//" + window.location.host + "/ws");
  ws.binaryType = "arraybuffer";

  // Local send buffer size past which coalesced pointer motion is dropped
  // rather than queued -- keeps a slow/congested link from accumulating an
  // unbounded backlog of stale samples. Button/key/wheel messages pass
  // `droppable` falsy and are always sent.
  const SEND_BACKPRESSURE_LIMIT = 1 << 20; // 1 MiB
  function send(bytes, droppable) {
    if (ws.readyState !== WebSocket.OPEN)
      return;
    if (droppable && ws.bufferedAmount > SEND_BACKPRESSURE_LIMIT)
      return;
    ws.send(bytes);
  }

  function sendResize() {
    const dpr = window.devicePixelRatio || 1;
    send(encodeResize(canvas.clientWidth, canvas.clientHeight, dpr));
  }

  // ── automation: window.wish tree-query + live-log shim ────────────────────
  //
  // The only piece Playwright needs beyond its own screenshot/mouse/keyboard
  // APIs -- see "Screenshots and input via Playwright" in
  // src/automation/DESIGN.md. No-ops against a server built without
  // -DWISH_ENABLE_AUTOMATION=ON: getTree()'s Promise never resolves (no
  // TREE_SNAPSHOT ever arrives), and `logs` simply stays empty (no
  // LOG_EVENT ever arrives).
  window.wish = {
    ready: false,
    _pending: new Map(), // request_id -> {resolve, reject}, used by getTree()

    // Every log() call made through this session's logger service, pushed
    // live as LOG_EVENT messages arrive (see logger::log(), src/context/
    // logger.hpp) and appended here in the exact order they happened --
    // interleaved with whatever actions this script took in between, so
    // e.g. "click a button, then see the log entry it caused appear after
    // the entries seen before the click" requires no extra correlation.
    // Unbounded on the JS side (the server's own buffer is capped -- see
    // logger::kMaxRecentLogs); a long-running script that cares should trim
    // it itself.
    logs: [],
    _nextId: 1,
    _renderResolvers: [], // FIFO of requestRender() resolvers, see below

    /**
     * Query the live widget tree, optionally restricted to `root` (a
     * dot-path) and its descendants.
     * @param {string} [root] Dot-path to restrict to; omit for the whole tree.
     * @returns {Promise<{request_id: number, widgets: object[]}>}
     */
    getTree(root = "") {
      const id = this._nextId++;
      return new Promise((resolve, reject) => {
        this._pending.set(id, { resolve, reject });
        send(encodeQueryTree(id, root));
      });
    },

    /**
     * Look up one widget by its exact dot-path.
     * @param {string} path
     * @returns {Promise<object|null>} The widget's snapshot entry, or `null`
     *   if `path` does not currently exist in the tree.
     */
    async getWidget(path) {
      const snap = await this.getTree(path);
      return snap.widgets.find((w) => w.path === path) ?? null;
    },

    /**
     * Ask the server to draw and broadcast one more frame now, and resolve
     * once that frame has actually landed on the canvas -- only meaningful
     * (and only ever needed) when the server was launched with a renderer
     * that opted into `renderer::render_on_demand()`, where frames
     * otherwise aren't drawn just because of routine WS/input activity. Safe
     * to call regardless (a harmless extra frame if the server isn't
     * render_on_demand). `getTree()`/`getWidget()` already request a fresh
     * render themselves server-side; callers that want a guaranteed-fresh
     * screenshot should await this immediately before capturing pixels.
     * @returns {Promise<void>}
     */
    requestRender() {
      return new Promise((resolve) => {
        this._renderResolvers.push(resolve);
        send(encodeRequestRender());
      });
    },

    /**
     * Every log entry received so far, oldest first. Synchronous -- no
     * server round trip, since entries are already pushed into `logs` as
     * they happen (see the `logs` field above).
     * @returns {object[]} Each `{seq, timestamp, level, message}`.
     */
    getLogs() {
      return this.logs.slice();
    },
  };

  const disconnectedBanner = document.getElementById("wish-disconnected-banner");

  function showDisconnectedBanner() {
    if (disconnectedBanner) disconnectedBanner.classList.remove("hidden");
  }

  ws.addEventListener("open", () => {
    console.log("[wish] connected");
    sendResize();
  });
  ws.addEventListener("close", () => {
    console.log("[wish] disconnected");
    showDisconnectedBanner();
  });
  ws.addEventListener("error", (err) => {
    console.error("[wish] socket error", err);
    showDisconnectedBanner();
  });

  ws.addEventListener("message", async (event) => {
    const buf = event.data;
    const view = new DataView(buf);
    const type = view.getUint8(0);
    const payloadLen = view.getUint32(4, true);
    if (8 + payloadLen !== buf.byteLength) {
      console.error("[wish] malformed message: length mismatch");
      return;
    }
    const offset = 8;
    switch (type) {
      case MSG.FRAME: {
        // Peek the target id (first payload field, offset 8) without decoding
        // the whole frame.
        const targetId = view.getUint32(offset, true);
        if (targetId !== 0) {
          // An offscreen render-target frame is a side effect a later
          // canvas frame's compositing draw command depends on -- decode and
          // render it synchronously rather than through the rAF-deferred
          // "latest frame wins" path below, which would silently drop it if a
          // canvas FRAME lands in the same tick. See "Offscreen Render
          // Targets" in src/web/DESIGN.md.
          renderer.renderToTarget(decodeFrame(view, offset));
        } else {
          pendingFrameView = view;
          scheduleFrameRender();
        }
        break;
      }
      case MSG.TEX_CREATE:
      case MSG.TEX_UPDATE: {
        const t = decodeTextureMessage(view, offset);
        renderer.uploadTexture(t.textureId, t.format, t.width, t.height, t.rects);
        // Only set when this TEX_CREATE follows a TEX_CHECK miss (see
        // MSG.TEX_CHECK below) -- a texture never offered for caching (the
        // font atlas, or anything under a private/ prefix server-side) has
        // no pending entry here and is simply never persisted.
        const pending = pendingCacheStore.get(t.textureId);
        if (pending && window.WishResourceCache) {
          pendingCacheStore.delete(t.textureId);
          // TEX_CREATE always carries exactly one whole-texture rect (see
          // draw_protocol.cpp's encode_texture_update).
          WishResourceCache.store(
              pending.path, pending.crc32, t.width, t.height, t.format === TEX_FORMAT.ALPHA8, t.rects[0].pixels);
        }
        break;
      }
      case MSG.TEX_DESTROY:
        renderer.destroyTexture(view.getUint32(offset, true));
        break;
      case MSG.TEX_CHECK: {
        const t = decodeTextureCheck(view, offset);
        const cached = window.WishResourceCache ? await WishResourceCache.lookup(t.path, t.crc32) : null;
        if (cached) {
          renderer.uploadTexture(
              t.textureId, t.format, t.width, t.height, [{ x: 0, y: 0, w: t.width, h: t.height, pixels: cached.pixels }]);
          send(encodeCacheResponse(t.textureId, true));
        } else {
          pendingCacheStore.set(t.textureId, { path: t.path, crc32: t.crc32 });
          send(encodeCacheResponse(t.textureId, false));
        }
        break;
      }
      case MSG.TREE_SNAPSHOT: {
        const jsonBytes = new Uint8Array(view.buffer, view.byteOffset + offset, payloadLen);
        const snapshot = JSON.parse(TEXT_DECODER.decode(jsonBytes));
        const pending = window.wish._pending.get(snapshot.request_id);
        if (pending) {
          window.wish._pending.delete(snapshot.request_id);
          pending.resolve(snapshot);
        }
        break;
      }
      case MSG.LOG_EVENT: {
        // Pushed unprompted, no request_id to resolve against -- just
        // append to window.wish.logs in the order it arrived.
        const jsonBytes = new Uint8Array(view.buffer, view.byteOffset + offset, payloadLen);
        const event = JSON.parse(TEXT_DECODER.decode(jsonBytes));
        window.wish.logs.push(...event.logs);
        break;
      }
      case MSG.CLIPBOARD_WRITE: {
        // ImGui just copied/cut this text server-side (Ctrl+C/Ctrl+X inside
        // some widget) -- push it to the real OS clipboard. Requires a
        // clipboard-write permission the browser may have already granted
        // implicitly (recent user gesture); silently drops on failure
        // (e.g. permission denied) rather than surfacing a popup, same as
        // ImGui's own fallback would do nothing visible either.
        const textBytes = new Uint8Array(view.buffer, view.byteOffset + offset, payloadLen);
        const text = TEXT_DECODER.decode(textBytes);
        if (navigator.clipboard && navigator.clipboard.writeText)
          navigator.clipboard.writeText(text).catch(() => {});
        break;
      }
      default:
        console.warn("[wish] unknown message type", type);
    }
  });

  window.addEventListener("resize", sendResize);

  // canvas.getBoundingClientRect() forces a synchronous layout. The canvas is
  // a single fixed full-viewport element, so its rect only moves on resize or
  // scroll -- cache it and refresh only then, instead of reading it on every
  // pointer event.
  let canvasRect = canvas.getBoundingClientRect();
  function refreshCanvasRect() {
    canvasRect = canvas.getBoundingClientRect();
  }
  window.addEventListener("resize", refreshCanvasRect);
  window.addEventListener("scroll", refreshCanvasRect, true);

  // DOM MouseEvent.button: 0=left,1=middle,2=right. ImGui: 0=left,1=right,2=middle.
  const DOM_BUTTON_TO_IMGUI = { 0: 0, 1: 2, 2: 1 };

  function canvasPos(e) {
    return { x: e.clientX - canvasRect.left, y: e.clientY - canvasRect.top };
  }

  // Coalesce pointer motion to at most one INPUT message per animation frame.
  // A high-poll-rate mouse emits hundreds of mousemove events per second, but
  // the server caps rendering near 60 FPS and applies its own 4 px / 100 ms
  // motion debounce, so sub-frame motion samples are pure wire traffic.
  let pendingMove = null;
  let moveRafScheduled = false;
  function flushPendingMove() {
    moveRafScheduled = false;
    if (pendingMove) {
      send(encodeMouseMove(pendingMove.x, pendingMove.y), /*droppable=*/true);
      pendingMove = null;
    }
  }
  window.addEventListener("mousemove", (e) => {
    pendingMove = canvasPos(e);
    if (!moveRafScheduled) {
      moveRafScheduled = true;
      requestAnimationFrame(flushPendingMove);
    }
  });
  canvas.addEventListener("mousedown", (e) => {
    e.preventDefault();
    canvas.focus();
    flushPendingMove(); // keep motion ordered before the button press
    const p = canvasPos(e);
    const btn = DOM_BUTTON_TO_IMGUI[e.button];
    if (btn !== undefined)
      send(encodeMouseButton(btn, true, p.x, p.y));
  });
  window.addEventListener("mouseup", (e) => {
    const btn = DOM_BUTTON_TO_IMGUI[e.button];
    if (btn !== undefined) {
      flushPendingMove();
      const p = canvasPos(e);
      send(encodeMouseButton(btn, false, p.x, p.y));
    }
  });
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    send(encodeMouseWheel(-e.deltaX / 100, -e.deltaY / 100));
  }, { passive: false });
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());

  // While a Ctrl+V clipboard read is pending (see the keydown handler
  // below), every OTHER key event that arrives is queued here instead of
  // sent immediately, and flushed in original order right after the paste's
  // own CLIPBOARD_TEXT + keydown go out. Without this, a fast key release
  // (most commonly Ctrl's own keyup, dispatched synchronously with no delay)
  // can reach the server *before* the deliberately-delayed paste keydown,
  // corrupting modifier state server-side (e.g. io.KeyCtrl already false by
  // the time ImGui processes the delayed 'V' keydown) and silently breaking
  // the paste shortcut.
  let pasteReadPending = false;
  let queuedDuringPaste = [];

  function flushQueuedKeyEvents() {
    for (const qe of queuedDuringPaste) {
      const key = domCodeToImGuiKey(qe.code);
      if (key !== undefined)
        send(encodeKey(key, qe.down));
    }
    queuedDuringPaste = [];
  }

  canvas.tabIndex = 0; // make the canvas focusable so it receives key events
  window.addEventListener("keydown", (e) => {
    // Ctrl+V (paste): the OS clipboard can only be read asynchronously (see
    // "Clipboard bridging" in src/web/DESIGN.md), so intercept it here,
    // push the current clipboard text to the server via CLIPBOARD_TEXT,
    // *then* forward the normal key event -- GetClipboardTextFn is a
    // synchronous ImGui callback, so the text must already be cached
    // server-side by the time ImGui itself processes this keydown.
    if ((e.ctrlKey || e.metaKey) && e.code === "KeyV") {
      e.preventDefault();
      pasteReadPending = true;
      const forwardKeydown = () => {
        const key = domCodeToImGuiKey(e.code);
        if (key !== undefined)
          send(encodeKey(key, true));
        pasteReadPending = false;
        flushQueuedKeyEvents();
      };
      if (navigator.clipboard && navigator.clipboard.readText) {
        navigator.clipboard.readText()
            .then((text) => {
              send(encodeClipboardText(text));
              forwardKeydown();
            })
            // Permission denied, or clipboard has no text -- still forward
            // the keydown so ImGui's own session-local fallback clipboard
            // (if anything was copied within this app) can still paste.
            .catch(forwardKeydown);
      } else {
        forwardKeydown();
      }
      return;
    }

    if (pasteReadPending) {
      queuedDuringPaste.push({ code: e.code, down: true });
      e.preventDefault();
      return;
    }

    const key = domCodeToImGuiKey(e.code);
    if (key !== undefined) {
      send(encodeKey(key, true));
      e.preventDefault();
    }
    if (e.key.length === 1 && !e.ctrlKey && !e.metaKey)
      send(encodeChar(e.key.codePointAt(0)));
  });
  window.addEventListener("keyup", (e) => {
    if (pasteReadPending) {
      queuedDuringPaste.push({ code: e.code, down: false });
      e.preventDefault();
      return;
    }
    const key = domCodeToImGuiKey(e.code);
    if (key !== undefined) {
      send(encodeKey(key, false));
      e.preventDefault();
    }
  });
})();
