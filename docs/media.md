# Media playback

A wish server can play audio and video files for its clients. The server
does all of the work: it decodes the file (FFmpeg), plays the sound on its
own sound card (miniaudio) and turns video frames into a texture that the
`Video` widget, or an `Image` with `src = "media://<name>"`, draws. A client
only sends commands over RMI (open, play, pause, stop, seek, volume, loop),
so the client libraries and every language binding link no media library.

## Contents

- [Quick start](#quick-start)
- [The sandbox rule](#the-sandbox-rule)
- [Players](#players)
- [Showing video](#showing-video)
- [Client API by language](#client-api-by-language)
- [Building with media support](#building-with-media-support)
- [Behavior and limits](#behavior-and-limits)
- [Server-side design](#server-side-design)

## Quick start

```cpp
// 1. Put the file in the session sandbox.
c.upload_file("intro.mp4", read_file("intro.mp4")).get();

// 2. Open it as player "intro" and show it.
auto player = c.open_media("intro", "intro.mp4").get();
c.register_template_from_json("ui"_key, R"({
  "type": "Window", "title": "Player", "width": 680, "height": 460,
  "children": {
    "video": { "type": "Video", "player": "intro", "width": -1, "height": 360 },
    "play":  { "type": "Button", "label": "Play" }
  }
})").get();
auto ui = c.instantiate_template("ui"_key).get();
ui["play"].onEvent("clicked"_key, [player](bison::dynamic) mutable { player.play(); });

// 3. Hear about the end.
player.on_ended([] { std::puts("done"); });
```

`examples/media_player/` is a complete version with Play/Pause/Stop, a
seek bar and a volume slider.

## The sandbox rule

The server only plays files that are already in the session sandbox,
uploaded with `upload_file()` or `sync_resource_package()`. A client can't
stream a file to the server through the media API, and can't make the
server open anything else:

- `path` is resolved with `file_service::resolve_path()`: a `..` escape is
  refused, and so is an absolute path, unless the server allows absolute
  paths (`set_allow_absolute_paths(true)`, meant for single-process hosts).
- URLs (`http://`, `file://`, ...) are refused. Media files are not fetched
  the way `Image.src` URLs are.
- FFmpeg reads the file only through a stream the server opened itself, and
  it can't open any other file or URL. Playlists and concat lists (HLS, DASH,
  `ffconcat`) that name other files fail to open.

## Players

`open_media(name, path, options)` opens a file as a named player and
returns a handle. The name is how widgets find the player. It is scoped to
the session, and opening a name that is already open replaces that player.
Options are `autoplay`, `loop` and `volume` (0..1).

| Call | Effect |
|------|--------|
| `play()` | Start or resume. From the end, restart from the beginning. |
| `pause()` | Pause at the current position. |
| `stop()` | Pause and rewind to the start. |
| `seek(seconds)` | Move to a time (clamped to the file), keeping the playing/paused state. |
| `set_volume(v)` | 0 (mute) .. 1. |
| `set_loop(b)` | When on, the end restarts from the beginning instead of ending. |
| `status()` | State, position, duration, video size, `has_video`/`has_audio`, volume, loop, `audio_output`. |
| `close()` | Stop and free the player on the server. |

The states are `stopped`, `playing`, `paused`, `ended` and `error` (the
file failed to decode; `status().error` says why). Players send three
events:

| Event | Payload | When |
|-------|---------|------|
| `state_changed` | `state`, `state_name` | On every state change. |
| `ended` | – | Playback reached the end and `loop` is off. |
| `error` | `message` | Decoding failed. |

A handler only gets events raised after it was registered. With
`autoplay`, the change to "playing" happens during `open_media()`, so it is
not reported.

`has_media()` tells whether the server plays media at all. A server built
without FFmpeg, or one with no renderer, has no media service, and
`open_media()` fails (`WISH_ERR_UNAVAILABLE` in the C ABI).

## Showing video

- `Video` widget: `{ "type": "Video", "player": "intro" }`. It draws the
  frame letterboxed (`keep_aspect`) and emits `clicked`. For sizing, see
  [ui-elements.md](ui-elements.md#video).
- Any `Image`: `"src": "media://intro"` draws the same texture with
  `Image`'s own size, tint and badge options.

Until the first frame is decoded, both draw nothing (`Video` draws a black
box). An audio-only player has no frame.

## Client API by language

| Binding | Open | Events |
|---------|------|--------|
| C++ (`wish::client`, `wish::standalone`, `wish_app_host`) | `auto p = c.open_media("intro", "intro.mp4", {.autoplay = true}).get();` | `p.on_state_changed(...)`, `p.on_ended(...)`, `p.on_error(...)` |
| C ABI | `wish_media_open(c, "intro", "intro.mp4", WISH_MEDIA_AUTOPLAY, 1.0f, &m)` | `wish_media_on_event(m, fn, ud)` |
| C++ binding (`wish_cpp`) | `auto p = c.open_media("intro", "intro.mp4");` | `p.on_event([](const wish::media_player::event& e) {...})` |
| Python | `p = c.open_media("intro", "intro.mp4", autoplay=True)` | `p.on_event(lambda event, state, message: ...)` |
| C# | `using var p = c.OpenMedia("intro", "intro.mp4", autoplay: true);` | `p.Event += (kind, state, message) => ...` |
| Rust | `let mut p = c.open_media("intro", "intro.mp4", &MediaOptions::default())?;` | `p.on_event(\|e\| ...)?` |
| Go | `p, err := c.OpenMedia("intro", "intro.mp4", wish.DefaultMediaOptions())` | `p.OnEvent(func(e wish.MediaEvent) {...})` |
| Java (Android) | `MediaPlayer p = c.openMedia("intro", "intro.mp4");` | `p.setListener((event, state, message) -> ...)` |

Calls in the C ABI and the bindings block until the server answers. The
native C++ API returns futures. Event callbacks run on the client's event
thread.

## Building with media support

Media support is a server-side build option, `WISH_ENABLE_MEDIA` (on by
default). It needs FFmpeg's development libraries (libavformat, libavcodec,
libavutil, libswscale, libswresample; FFmpeg 5.1 or newer) installed on the
build machine. miniaudio is a header-only submodule (`extern/miniaudio`).
When FFmpeg isn't found, configure prints `wish: media playback OFF` and
builds the server without it. See
[building.md](building.md#media-playback-ffmpeg) for the package names on
each platform.

FFmpeg is linked as shared libraries and used under the LGPL. A build that
redistributes the server should either ship FFmpeg's shared libraries (an
LGPL build) next to it or depend on the system's packages. The release
`.deb` depends on the system packages. The prebuilt release zips are built
without media support, so they still run on machines without FFmpeg.

## Behavior and limits

- At most 8 players per session. Every player stops when its session ends.
- Videos up to 4096x4096. Every decoded frame is converted to RGBA and
  uploaded whole. With `--renderer web`, each frame then goes over the
  WebSocket to the browser as raw RGBA, so keep videos small there (e.g.
  640x360).
- Playback follows the server's clock. Audio is kept in sync with it,
  dropping late samples or padding with silence. A server without a sound
  card (headless, CI) plays silently, still in real time, and reports
  `audio_output = false`.
- Volume applies to the player's own audio. There is no mixing bus or
  master volume.
- Frames are pulled by the render loop. While a video plays, its session is
  redrawn at up to the renderer's frame rate (~60 fps).

## Server-side design

`src/media/`:

| File | Role |
|------|------|
| `media_decoder.*` | FFmpeg: sandboxed custom I/O, best video/audio stream, RGBA (swscale) and stereo 48 kHz float (swresample) conversion, seek. |
| `audio_output.*` | miniaudio device per player; one backend context per process. |
| `playback.*` | Decode thread, frame queue, audio buffer, clock and the play/pause/seek/end state machine. Thread-safe. |
| `media_player.*` | The `__WishMediaPlayer` RMI object: methods, events, and the frame texture. |
| `media_service.*` | The `__WishMedia` per-session service: `open`/`close`/`list`, sandbox checks, limits, and `pump()` for the render loop. |

The render loop (`server::render_loop()` and `standalone::render_loop()`)
calls `media::pump_sessions()` on every iteration, whether or not it draws a
frame. For each session that has players, this:

1. picks each player's newest due frame and uploads it to that player's
   `dynamic_texture`;
2. queues the players' events;
3. marks the session dirty, so a frame is drawn and the events are sent.

`renderer::create_dynamic_texture()` gives an `ImTextureData` registered as
an ImGui user texture. The SDL3 and web backends already upload texture
create/update/destroy requests, so one implementation in `imgui_renderer`
serves both. A texture released on another thread (a session ending) is
destroyed by the render thread on its next frame.
