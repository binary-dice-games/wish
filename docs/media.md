# Audio playback

A wish server can play audio files for its clients. The server does all of
the work: it decodes the file and plays the sound on its own sound card,
both with [miniaudio](https://miniaud.io), which is compiled into the
server (no system library to install). A client only sends commands over
RMI (open, play, pause, stop, seek, volume, loop), so the client libraries
and every language binding link no media library.

Supported formats: **WAV**, **FLAC**, **MP3** and **Ogg Vorbis**. wish does
not play video.

## Contents

- [Quick start](#quick-start)
- [The sandbox rule](#the-sandbox-rule)
- [Players](#players)
- [Client API by language](#client-api-by-language)
- [Building with audio support](#building-with-audio-support)
- [Behavior and limits](#behavior-and-limits)
- [Server-side design](#server-side-design)

## Quick start

```cpp
// 1. Put the file in the session sandbox.
c.upload_file("intro.mp3", read_file("intro.mp3")).get();

// 2. Open it as player "intro" and give it a Play button.
auto player = c.open_media("intro", "intro.mp3").get();
c.register_template_from_json("ui"_key, R"({
  "type": "Window", "title": "Player", "width": 300, "height": 100,
  "children": {
    "play": { "type": "Button", "label": "Play" }
  }
})").get();
auto ui = c.instantiate_template("ui"_key).get();
ui["play"].onEvent("clicked"_key, [player](bison::dynamic) mutable { player.play(); });

// 3. Hear about the end.
player.on_ended([] { std::puts("done"); });
```

`examples/media_player/` is a complete version with Play/Pause/Stop, a
seek bar and a volume slider. The bundled **mpv** tool
([modules/bdg/desktop/mpv](../modules/bdg/desktop/mpv/README.md),
`wish client --run=mpv`) is a full audio player built the same way: it
uploads local files to the sandbox and plays them.

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
- The decoder reads only that one file. None of the supported formats can
  reference another file (there are no playlists).

## Players

`open_media(name, path, options)` opens a file as a named player and
returns a handle. The name is scoped to the session, and opening a name
that is already open replaces that player. Options are `autoplay`, `loop`
and `volume` (0..1).

| Call | Effect |
|------|--------|
| `play()` | Start or resume. From the end, restart from the beginning. |
| `pause()` | Pause at the current position. |
| `stop()` | Pause and rewind to the start. |
| `seek(seconds)` | Move to a time (clamped to the file), keeping the playing/paused state. |
| `set_volume(v)` | 0 (mute) .. 1. |
| `set_loop(b)` | When on, the end restarts from the beginning instead of ending. |
| `status()` | State, position, duration, volume, loop, `audio_output`. |
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

`has_media()` tells whether the server plays audio at all. A server built
with `WISH_ENABLE_MEDIA=OFF`, or one with no renderer, has no media
service, and `open_media()` fails (`WISH_ERR_UNAVAILABLE` in the C ABI).

## Client API by language

| Binding | Open | Events |
|---------|------|--------|
| C++ (`wish::client`, `wish::standalone`, `wish_app_host`) | `auto p = c.open_media("intro", "intro.mp3", {.autoplay = true}).get();` | `p.on_state_changed(...)`, `p.on_ended(...)`, `p.on_error(...)` |
| C ABI | `wish_media_open(c, "intro", "intro.mp3", WISH_MEDIA_AUTOPLAY, 1.0f, &m)` | `wish_media_on_event(m, fn, ud)` |
| C++ binding (`wish_cpp`) | `auto p = c.open_media("intro", "intro.mp3");` | `p.on_event([](const wish::media_player::event& e) {...})` |
| Python | `p = c.open_media("intro", "intro.mp3", autoplay=True)` | `p.on_event(lambda event, state, message: ...)` |
| C# | `using var p = c.OpenMedia("intro", "intro.mp3", autoplay: true);` | `p.Event += (kind, state, message) => ...` |
| Rust | `let mut p = c.open_media("intro", "intro.mp3", &MediaOptions::default())?;` | `p.on_event(\|e\| ...)?` |
| Go | `p, err := c.OpenMedia("intro", "intro.mp3", wish.DefaultMediaOptions())` | `p.OnEvent(func(e wish.MediaEvent) {...})` |
| Java (Android) | `MediaPlayer p = c.openMedia("intro", "intro.mp3");` | `p.setListener((event, state, message) -> ...)` |

Calls in the C ABI and the bindings block until the server answers. The
native C++ API returns futures. Event callbacks run on the client's event
thread.

## Building with audio support

Audio playback is a server-side build option, `WISH_ENABLE_MEDIA` (on by
default). It has no system dependency: miniaudio is a header-only submodule
(`extern/miniaudio`), and Ogg Vorbis decoding uses `stb_vorbis` from the
`extern/stb` submodule the server already uses for images. miniaudio loads
the platform's audio API (WASAPI, Core Audio, PulseAudio/ALSA/JACK) at
runtime, so the same binary runs on machines with and without sound. It is
off on Android and in builds without the imgui renderer.

## Behavior and limits

- At most 8 players per session. Every player stops when its session ends.
- Every file is converted to stereo float at 48 kHz, whatever its own
  sample rate and channel count.
- Playback follows the server's clock. Audio is kept in sync with it,
  dropping late samples or padding with silence. A server without a sound
  card (headless, CI) plays silently, still in real time, and reports
  `audio_output = false`.
- The sound always comes out of the server's sound card, also with
  `--renderer web`: the browser gets no audio.
- Volume applies to the player's own audio. There is no mixing bus or
  master volume.

## Server-side design

`src/media/`:

| File | Role |
|------|------|
| `miniaudio_config.hpp`, `miniaudio_impl.cpp` | The `MA_NO_*` feature switches shared by every user of miniaudio, and the one translation unit compiling miniaudio and stb_vorbis. |
| `audio_decoder.*` | miniaudio decoder: opens one file, converts it to stereo 48 kHz float, decodes in chunks, seeks sample-accurately. |
| `audio_output.*` | miniaudio device per player; one backend context per process. |
| `playback.*` | Decode thread, audio buffer, clock and the play/pause/seek/end state machine. Thread-safe. |
| `media_player.*` | The `__WishMediaPlayer` RMI object: methods and events. |
| `media_service.*` | The `__WishMedia` per-session service: `open`/`close`/`list`, sandbox checks, limits, and `pump()` for the render loop. |

The render loop (`server::render_loop()` and `standalone::render_loop()`)
calls `media::pump_sessions()` on every iteration, whether or not it draws a
frame. For each session that has players, this advances each player
(dropping audio a silent player won't play, detecting the end or looping),
queues the players' events, and marks the session dirty so the events are
sent.
