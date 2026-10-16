// MIT License © 2026 Binary Dice Games

// Package wish: this file wraps wish_client_c.h's media playback -- audio
// and video files from the session sandbox, played by the server (see
// docs/media.md). The server decodes and plays them; this package only
// sends commands and links no media library.
package wish

/*
#include <stdlib.h>
#include "wish_client_c.h"
#include "shim.h"

extern void goMediaEventTrampoline(wish_media_handle media, int32_t event, int32_t state,
    char* message, void* userdata);
*/
import "C"

import (
	"runtime"
	"runtime/cgo"
	"sync"
	"unsafe"
)

// MediaState is a player's state (WISH_MEDIA_STATE_*).
type MediaState int32

const (
	MediaStopped MediaState = 0
	MediaPlaying MediaState = 1
	MediaPaused  MediaState = 2
	MediaEnded   MediaState = 3 // reached the end without looping
	MediaError   MediaState = 4 // decoding failed; see MediaPlayer.ErrorMessage
)

// MediaEventKind says what a MediaEvent reports (WISH_MEDIA_EVENT_*).
type MediaEventKind int32

const (
	MediaEventStateChanged MediaEventKind = 0
	MediaEventEnded        MediaEventKind = 1
	MediaEventError        MediaEventKind = 2
)

// MediaEvent is one player event, passed to MediaPlayer.OnEvent callbacks.
type MediaEvent struct {
	Kind    MediaEventKind
	State   MediaState // the player's new state
	Message string     // error text for MediaEventError
}

// MediaOptions configures Client.OpenMedia.
type MediaOptions struct {
	Autoplay bool    // start playing right away
	Loop     bool    // restart from the beginning at the end
	Volume   float32 // 0 (mute) .. 1
}

// DefaultMediaOptions is paused, not looping, at full volume.
func DefaultMediaOptions() MediaOptions { return MediaOptions{Volume: 1} }

// MediaStatus is a snapshot of a player (MediaPlayer.Status).
type MediaStatus struct {
	State       MediaState
	Position    float64 // seconds
	Duration    float64 // seconds; 0 when the file doesn't say
	Width       int32
	Height      int32
	HasVideo    bool
	HasAudio    bool
	Volume      float32
	Loop        bool
	AudioOutput bool // false when the server has no sound card (silent)
}

// MediaPlayer is a media file opened on the server by Client.OpenMedia.
// Close it to stop and free it on the server.
type MediaPlayer struct {
	mu     sync.Mutex
	handle C.wish_media_handle
	client C.wish_client_handle
	name   string
	cb     cgo.Handle // 0: no callback registered
}

// HasMedia reports whether the server plays media (it was built with FFmpeg).
func (c *Client) HasMedia() (bool, error) {
	var avail C.int
	rc := C.wish_media_available(c.handle, &avail)
	if err := checkWish(rc, "client.has_media", c.handle); err != nil {
		return false, err
	}
	return avail != 0, nil
}

// OpenMedia opens path, a file already uploaded to the session sandbox, as
// player name. The server plays it; show its video with a Video widget
// whose "player" is name. Absolute paths, ".." escapes and URLs are
// refused. Fails with WishErrUnavailable when the server plays no media.
func (c *Client) OpenMedia(name, path string, opts MediaOptions) (*MediaPlayer, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	var flags C.int32_t
	if opts.Autoplay {
		flags |= C.WISH_MEDIA_AUTOPLAY
	}
	if opts.Loop {
		flags |= C.WISH_MEDIA_LOOP
	}
	var out C.wish_media_handle
	rc := C.wish_media_open(c.handle, cName, cPath, flags, C.float(opts.Volume), &out)
	if err := checkWish(rc, "client.open_media", c.handle); err != nil {
		return nil, err
	}
	p := &MediaPlayer{handle: out, client: c.handle, name: name}
	runtime.SetFinalizer(p, (*MediaPlayer).Close)
	return p, nil
}

// Name returns the name widgets refer to.
func (p *MediaPlayer) Name() string { return p.name }

func (p *MediaPlayer) call(context string, fn func(C.wish_media_handle) C.wish_error) error {
	p.mu.Lock()
	h := p.handle
	p.mu.Unlock()
	if h == nil {
		return newWishError(WishErrNull, context, nil)
	}
	return checkWish(fn(h), context, p.client)
}

// Play starts or resumes; from the end, it restarts from the beginning.
func (p *MediaPlayer) Play() error {
	return p.call("media_player.play", func(h C.wish_media_handle) C.wish_error { return C.wish_media_play(h) })
}

// Pause pauses at the current position.
func (p *MediaPlayer) Pause() error {
	return p.call("media_player.pause", func(h C.wish_media_handle) C.wish_error { return C.wish_media_pause(h) })
}

// Stop pauses and rewinds to the start.
func (p *MediaPlayer) Stop() error {
	return p.call("media_player.stop", func(h C.wish_media_handle) C.wish_error { return C.wish_media_stop(h) })
}

// Seek moves to seconds (clamped to the file), keeping playing/paused.
func (p *MediaPlayer) Seek(seconds float64) error {
	return p.call("media_player.seek", func(h C.wish_media_handle) C.wish_error {
		return C.wish_media_seek(h, C.double(seconds))
	})
}

// SetVolume sets the volume, 0 (mute) .. 1.
func (p *MediaPlayer) SetVolume(volume float32) error {
	return p.call("media_player.set_volume", func(h C.wish_media_handle) C.wish_error {
		return C.wish_media_set_volume(h, C.float(volume))
	})
}

// SetLoop sets whether reaching the end restarts from the beginning.
func (p *MediaPlayer) SetLoop(loop bool) error {
	v := C.int32_t(0)
	if loop {
		v = 1
	}
	return p.call("media_player.set_loop", func(h C.wish_media_handle) C.wish_error {
		return C.wish_media_set_loop(h, v)
	})
}

// Status returns the player's state, position and stream info.
func (p *MediaPlayer) Status() (MediaStatus, error) {
	var s C.wish_media_status
	err := p.call("media_player.status", func(h C.wish_media_handle) C.wish_error {
		return C.wish_media_get_status(h, &s)
	})
	if err != nil {
		return MediaStatus{}, err
	}
	return MediaStatus{
		State:       MediaState(s.state),
		Position:    float64(s.position),
		Duration:    float64(s.duration),
		Width:       int32(s.width),
		Height:      int32(s.height),
		HasVideo:    s.has_video != 0,
		HasAudio:    s.has_audio != 0,
		Volume:      float32(s.volume),
		Loop:        s.loop != 0,
		AudioOutput: s.audio_output != 0,
	}, nil
}

// ErrorMessage returns why the player is in MediaError ("" otherwise).
func (p *MediaPlayer) ErrorMessage() (string, error) {
	var out *C.char
	err := p.call("media_player.error_message", func(h C.wish_media_handle) C.wish_error {
		return C.wish_media_get_error(h, &out)
	})
	if err != nil {
		return "", err
	}
	if out == nil {
		return "", nil
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// OnEvent calls fn for every event (replacing an earlier callback), on the
// client's event thread. Events raised before this call are not replayed.
func (p *MediaPlayer) OnEvent(fn func(MediaEvent)) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.handle == nil {
		return newWishError(WishErrNull, "media_player.on_event", nil)
	}
	h := cgo.NewHandle(fn)
	rc := C.wish_media_on_event_shim(p.handle,
		(C.wish_media_event_fn)(unsafe.Pointer(C.goMediaEventTrampoline)), C.uintptr_t(h))
	if err := checkWish(rc, "media_player.on_event", p.client); err != nil {
		h.Delete()
		return err
	}
	// The C side swapped callbacks under the lock its calls run in: the old
	// handle is no longer referenced.
	if p.cb != 0 {
		p.cb.Delete()
	}
	p.cb = h
	return nil
}

// Close stops and frees the player on the server; a no-op when closed.
func (p *MediaPlayer) Close() {
	p.mu.Lock()
	h, cb := p.handle, p.cb
	p.handle, p.cb = nil, 0
	p.mu.Unlock()
	if h == nil {
		return
	}
	C.wish_media_close(h) // no callback runs after this
	if cb != 0 {
		cb.Delete()
	}
	runtime.SetFinalizer(p, nil)
}
