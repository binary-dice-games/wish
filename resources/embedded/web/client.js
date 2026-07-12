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
    // Only meaningful when the server was built with -DWISH_ENABLE_AUTOMATION=ON
    // (see src/automation/DESIGN.md) -- a server built without it never
    // sends TREE_SNAPSHOT/LOG_EVENT and silently ignores QUERY_TREE, so
    // window.wish.getTree() simply never resolves and window.wish.logs
    // simply never grows against such a server.
    QUERY_TREE: 0x20,
    TREE_SNAPSHOT: 0x21,
    LOG_EVENT: 0x22, // pushed live, no request -- see window.wish.logs below
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
    return encodeEnvelope(MSG.QUERY_TREE, new TextEncoder().encode(json));
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
      if (!entry)
        return;
      this.gl.deleteTexture(entry.tex);
      this.textures.delete(textureId);
    }

    // frame: { displayPos, displaySize, fbScale, cmdLists: [{vtx, idx, cmds: [...]}] }
    render(frame) {
      const gl = this.gl;
      const fbWidth = Math.max(1, Math.round(frame.displaySize.x * frame.fbScale.x));
      const fbHeight = Math.max(1, Math.round(frame.displaySize.y * frame.fbScale.y));
      if (this.canvas.width !== fbWidth) this.canvas.width = fbWidth;
      if (this.canvas.height !== fbHeight) this.canvas.height = fbHeight;

      gl.viewport(0, 0, fbWidth, fbHeight);
      gl.clearColor(0.117, 0.117, 0.117, 1.0); // matches sdl3_renderer's clear color
      gl.clear(gl.COLOR_BUFFER_BIT);

      gl.enable(gl.BLEND);
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.CULL_FACE);
      gl.enable(gl.SCISSOR_TEST);

      gl.useProgram(this.program);
      const L = frame.displayPos.x, R = frame.displayPos.x + frame.displaySize.x;
      const T = frame.displayPos.y, B = frame.displayPos.y + frame.displaySize.y;
      // Column-major orthographic projection matching imgui_impl_opengl3.
      const proj = new Float32Array([
        2 / (R - L), 0, 0, 0,
        0, 2 / (T - B), 0, 0,
        0, 0, -1, 0,
        (R + L) / (L - R), (T + B) / (B - T), 0, 1,
      ]);
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
  }

  // ── frame decoding ───────────────────────────────────────────────────────

  function decodeFrame(view, offset) {
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
    return { displayPos, displaySize, fbScale, cmdLists };
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
    const path = new TextDecoder().decode(pathBytes);
    return { textureId, format, width, height, crc32, path };
  }

  // ── wiring: WebSocket + input capture ────────────────────────────────────

  const canvas = document.getElementById("wish-canvas");
  const renderer = new Renderer(canvas);

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

  function send(bytes) {
    if (ws.readyState === WebSocket.OPEN)
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
      case MSG.FRAME:
        renderer.render(decodeFrame(view, offset));
        // First FRAME processed means the canvas now shows something real --
        // Playwright scripts wait on this before interacting (see
        // src/automation/DESIGN.md's "Readiness").
        window.wish.ready = true;
        break;
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
        const snapshot = JSON.parse(new TextDecoder().decode(jsonBytes));
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
        const event = JSON.parse(new TextDecoder().decode(jsonBytes));
        window.wish.logs.push(...event.logs);
        break;
      }
      default:
        console.warn("[wish] unknown message type", type);
    }
  });

  window.addEventListener("resize", sendResize);

  // DOM MouseEvent.button: 0=left,1=middle,2=right. ImGui: 0=left,1=right,2=middle.
  const DOM_BUTTON_TO_IMGUI = { 0: 0, 1: 2, 2: 1 };

  function canvasPos(e) {
    const rect = canvas.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }

  window.addEventListener("mousemove", (e) => {
    const p = canvasPos(e);
    send(encodeMouseMove(p.x, p.y));
  });
  canvas.addEventListener("mousedown", (e) => {
    e.preventDefault();
    canvas.focus();
    const p = canvasPos(e);
    const btn = DOM_BUTTON_TO_IMGUI[e.button];
    if (btn !== undefined)
      send(encodeMouseButton(btn, true, p.x, p.y));
  });
  window.addEventListener("mouseup", (e) => {
    const btn = DOM_BUTTON_TO_IMGUI[e.button];
    if (btn !== undefined) {
      const p = canvasPos(e);
      send(encodeMouseButton(btn, false, p.x, p.y));
    }
  });
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    send(encodeMouseWheel(-e.deltaX / 100, -e.deltaY / 100));
  }, { passive: false });
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());

  canvas.tabIndex = 0; // make the canvas focusable so it receives key events
  window.addEventListener("keydown", (e) => {
    const key = domCodeToImGuiKey(e.code);
    if (key !== undefined) {
      send(encodeKey(key, true));
      e.preventDefault();
    }
    if (e.key.length === 1 && !e.ctrlKey && !e.metaKey)
      send(encodeChar(e.key.codePointAt(0)));
  });
  window.addEventListener("keyup", (e) => {
    const key = domCodeToImGuiKey(e.code);
    if (key !== undefined) {
      send(encodeKey(key, false));
      e.preventDefault();
    }
  });
})();
